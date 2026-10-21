#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include "codegen_sym.h"
#include <ctype.h>
#include "codegen.h"
#include "come_map.h"
#include "ast.h"
#include "common.h"

typedef come_map_t* map;

/* small helper: write indentation spaces */
static void emit_indent(FILE* f, int indent_spaces) {
    for (int i = 0; i < indent_spaces; i++) fputc(' ', f);
}

static void emit_c_string_literal(FILE* f, const char* s) {
    // s includes quotes from lexer?
    // Parser copies text. Lexer includes quotes for TOKEN_STRING_LITERAL?
    // Lexer: "foo" -> text="foo" (with quotes).
    // So just print it.
    fprintf(f, "%s", s);
}

static void emit_c_string_escaped(FILE* f, const char* s) {
    for (; *s; s++) {
        if (*s == '\\' || *s == '"') {
            fputc('\\', f);
        }
        fputc(*s, f);
    }
}

// Track source file for #line directives
static const char* source_filename = NULL;
static int last_emitted_line = -1;
static int g_gen_line_map = 1;

// Track current function return type for correct return statement generation
static char current_function_return_type[128] = "";
static char current_module[256] = "main"; // Default to main if unspecified
static char* current_imports[256];
static int current_import_count = 0;
static int codegen_error_count = 0;

typedef struct {
    char source_name[256];
    char c_name[512];
    char tuple_type[512];
    char struct_name[512];
    char elem_types[16][128];
    int elem_count;
} TupleReturnInfo;

static TupleReturnInfo tuple_returns[128];
static int tuple_return_count = 0;
static TupleReturnInfo* current_tuple_return = NULL;
static int tuple_temp_counter = 0;

typedef struct {
    char module[256];
    char name[256];
    char return_type[128];
    char arg_types[16][128];
    int arg_count;
    int has_arg_types;
} ImportedFunctionProto;

static ImportedFunctionProto imported_function_protos[256];
static int imported_function_proto_count = 0;

typedef struct {
    char name[256];
    char type[128];
} ConstSymbol;

static ConstSymbol const_symbols[512];
static int const_symbol_count = 0;



// Emit #line directive if needed
static void emit_line_directive(FILE* f, ASTNode* node) {
    if (!g_gen_line_map || !source_filename || !node || node->source_line <= 0) return;
    
    // Only emit if line changed to avoid clutter
    if (node->source_line != last_emitted_line) {
        fprintf(f, "\n#line %d \"", node->source_line);
        emit_c_string_escaped(f, source_filename);
        fprintf(f, "\"\n");
        last_emitted_line = node->source_line;
    }
}

#include "utils.h"
#include <ctype.h>

static void generate_node(FILE* f, ASTNode* node, int indent);
static void generate_expression(FILE* f, ASTNode* node);
static void generate_expression_as_type(FILE* f, ASTNode* node, const char* type);
static const char* infer_expression_type(ASTNode* node);

static int is_pointer_expression(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_IDENTIFIER) {
        const char* ptrs[] = {"self", "http", "req", "resp", "conn", "tls_listener", "args", "dyn", "buf", "transport"};
        for (int i=0; i<sizeof(ptrs)/sizeof(char*); i++) {
            if (strcmp(node->text, ptrs[i]) == 0) return 1;
        }
        // Also check if it's a string literal or something that becomes a pointer?
    }
    if (node->type == AST_MEMBER_ACCESS || node->type == AST_ARRAY_ACCESS) {
        // If the root is a pointer, assume access on it is a pointer (common for string/list items)
        return is_pointer_expression(node->children[0]);
    }
    if (node->type == AST_METHOD_CALL) {
        // Methods like accept(), new() return pointers. 
        if (strcmp(node->text, "accept") == 0 || strcmp(node->text, "new") == 0 || 
            strcmp(node->text, "at") == 0 || strcmp(node->text, "byte_array") == 0) return 1;
        return is_pointer_expression(node->children[0]);
    }
    return 0;
}

static int enum_counter = 0;
static char* seen_structs[256];
static int seen_count = 0;

static int is_struct_seen(const char* name) {
    for (int i=0; i<seen_count; i++) {
        if (strcmp(seen_structs[i], name) == 0) return 1;
    }
    return 0;
}

static void mark_struct_seen(const char* name) {
    if (!is_struct_seen(name) && seen_count < 256) {
        seen_structs[seen_count++] = strdup(name);
    }
}

static void reset_const_symbols(void) {
    const_symbol_count = 0;
}

static void add_const_symbol(const char* name, const char* type) {
    if (!name || !type) return;
    for (int i = 0; i < const_symbol_count; i++) {
        if (strcmp(const_symbols[i].name, name) == 0) {
            strncpy(const_symbols[i].type, type, sizeof(const_symbols[i].type) - 1);
            const_symbols[i].type[sizeof(const_symbols[i].type) - 1] = '\0';
            return;
        }
    }
    if (const_symbol_count < (int)(sizeof(const_symbols) / sizeof(const_symbols[0]))) {
        strncpy(const_symbols[const_symbol_count].name, name, sizeof(const_symbols[const_symbol_count].name) - 1);
        const_symbols[const_symbol_count].name[sizeof(const_symbols[const_symbol_count].name) - 1] = '\0';
        strncpy(const_symbols[const_symbol_count].type, type, sizeof(const_symbols[const_symbol_count].type) - 1);
        const_symbols[const_symbol_count].type[sizeof(const_symbols[const_symbol_count].type) - 1] = '\0';
        const_symbol_count++;
    }
}

static const char* find_const_symbol_type(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < const_symbol_count; i++) {
        if (strcmp(const_symbols[i].name, name) == 0) return const_symbols[i].type;
    }
    return NULL;
}

static const char* infer_const_type(ASTNode* node) {
    if (!node) return "int";
    
    // String literals -> char*
    if (node->type == AST_STRING_LITERAL) {
        return "char*";
    }
    
    if (node->type != AST_NUMBER) return "int";
    
    char* text = node->text;
    if (strchr(text, '.') || strstr(text, "f") || strstr(text, "F")) {
        // Default floating point literals to float per user preference
        return "float";
    }
    
    int is_unsigned = (strstr(text, "u") != NULL || strstr(text, "U") != NULL);
    int is_long = (strstr(text, "l") != NULL || strstr(text, "L") != NULL);
    int is_long_long = (strstr(text, "LL") != NULL || strstr(text, "ll") != NULL);
    
    if (is_unsigned) {
        if (is_long_long) return "unsigned long long";
        if (is_long) return "unsigned long";
        return "unsigned int";
    }
    if (is_long_long) return "long long";
    if (is_long) return "long";
    
    return "int";
}

static const char* c_type_name(const char* type) {
    if (strcmp(type, "long") == 0) return "long long";
    if (strcmp(type, "ulong") == 0) return "unsigned long long";
    return type;
}

static void codegen_error(ASTNode* node, const char* fmt, ...) {
    fprintf(stderr, "Codegen error");
    if (source_filename && node && node->source_line > 0) {
        fprintf(stderr, " at %s:%d", source_filename, node->source_line);
    }
    fprintf(stderr, ": ");

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    codegen_error_count++;
}

static int is_numeric_type_name(const char* type) {
    if (!type) return 0;
    const char* numeric[] = {
        "byte", "ubyte", "short", "ushort", "int", "uint", "long", "ulong",
        "float", "double", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64",
        "char", "wchar", "bool", "unsigned int", "unsigned long", "long long",
        "unsigned long long"
    };
    for (int i = 0; i < (int)(sizeof(numeric) / sizeof(numeric[0])); i++) {
        if (strcmp(type, numeric[i]) == 0) return 1;
    }
    return 0;
}

static int is_string_type_name(const char* type) {
    return type && (strcmp(type, "string") == 0 || strcmp(type, "come_string_t*") == 0);
}

static int is_string_list_type_name(const char* type) {
    return type && (strcmp(type, "string[]") == 0 || strcmp(type, "come_string_list_t*") == 0);
}

static int is_file_type_name(const char* type) {
    return type && (strcmp(type, "FILE") == 0 || strcmp(type, "come_std__FILE_t*") == 0);
}

static int is_map_type_name(const char* type) {
    return type && (strcmp(type, "map") == 0 || strcmp(type, "come_map_t*") == 0);
}

static int is_system_module_name(const char* name) {
    return name && (strcmp(name, "std") == 0 || strcmp(name, "string") == 0 ||
                    strcmp(name, "array") == 0 || strcmp(name, "map") == 0);
}

static int is_imported_module_name(const char* name) {
    if (!name) return 0;
    for (int i = 0; i < current_import_count; i++) {
        if (strcmp(current_imports[i], name) == 0) return 1;
    }
    return 0;
}

static void emit_c_type(FILE* f, const char* type) {
    if (is_string_type_name(type)) {
        fprintf(f, "come_string_t*");
    } else if (is_string_list_type_name(type)) {
        fprintf(f, "come_string_list_t*");
    } else if (is_file_type_name(type)) {
        fprintf(f, "come_std__FILE_t*");
    } else if (type && strcmp(type, "Proc") == 0) {
        fprintf(f, "come_std__Proc_t*");
    } else {
        fprintf(f, "%s", c_type_name(type ? type : "int"));
    }
}

static int type_compatible(const char* locked, const char* incoming) {
    if (!locked || !incoming || strcmp(locked, "var") == 0) return 1;
    if (strcmp(locked, incoming) == 0) return 1;
    if (is_string_type_name(locked) || is_string_type_name(incoming)) {
        return is_string_type_name(locked) && is_string_type_name(incoming);
    }
    if (is_string_list_type_name(locked) || is_string_list_type_name(incoming)) {
        return is_string_list_type_name(locked) && is_string_list_type_name(incoming);
    }
    if (is_file_type_name(locked) || is_file_type_name(incoming)) {
        return is_file_type_name(locked) && is_file_type_name(incoming);
    }
    if (is_map_type_name(locked) || is_map_type_name(incoming)) {
        return is_map_type_name(locked) && is_map_type_name(incoming);
    }
    return is_numeric_type_name(locked) && is_numeric_type_name(incoming);
}

static void sanitize_ident(const char* in, char* out, size_t out_sz) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < out_sz; i++) {
        unsigned char c = (unsigned char)in[i];
        out[j++] = (isalnum(c) || c == '_') ? (char)c : '_';
    }
    out[j] = '\0';
}

static void mangle_function_name(const char* module, const char* source_name, char* out, size_t out_sz) {
    if (strcmp(source_name, "init") == 0) {
        snprintf(out, out_sz, "come_%s__init_local", module);
        return;
    }
    if (strcmp(source_name, "exit") == 0) {
        snprintf(out, out_sz, "come_%s__exit_local", module);
        return;
    }

    char* underscore = strchr(source_name, '_');
    if (underscore && isupper((unsigned char)source_name[0])) {
        long prefix_len = underscore - source_name;
        snprintf(out, out_sz, "come_%s__%.*s__%s", module, (int)prefix_len, source_name, underscore + 1);
    } else {
        snprintf(out, out_sz, "come_%s__%s", module, source_name);
    }
}

static int parse_tuple_types(const char* tuple_type, char elem_types[][128], int max_elems) {
    if (!tuple_type || tuple_type[0] != '(') return 0;
    int count = 0;
    const char* p = tuple_type + 1;
    while (*p && *p != ')' && count < max_elems) {
        while (*p == ' ' || *p == '\t') p++;
        char* out = elem_types[count];
        int j = 0;
        while (*p && *p != ',' && *p != ')' && j < 127) {
            if (*p != ' ' && *p != '\t') out[j++] = *p;
            p++;
        }
        out[j] = '\0';
        if (j > 0) count++;
        if (*p == ',') p++;
    }
    return count;
}

static TupleReturnInfo* find_tuple_info_by_source(const char* source_name) {
    for (int i = 0; i < tuple_return_count; i++) {
        if (strcmp(tuple_returns[i].source_name, source_name) == 0) return &tuple_returns[i];
    }
    return NULL;
}

static void register_imported_function_proto(const char* module, const char* name, const char* return_type,
                                             char arg_types[][128], int arg_count) {
    if (!module || !name || is_system_module_name(module)) return;

    const char* ret = (return_type && return_type[0] && strcmp(return_type, "var") != 0) ? return_type : "int";
    if (ret[0] == '(') return;

    for (int i = 0; i < imported_function_proto_count; i++) {
        ImportedFunctionProto* proto = &imported_function_protos[i];
        if (strcmp(proto->module, module) == 0 && strcmp(proto->name, name) == 0) {
            if (strcmp(proto->return_type, ret) != 0 && strcmp(proto->return_type, "int") == 0) {
                strncpy(proto->return_type, ret, sizeof(proto->return_type) - 1);
                proto->return_type[sizeof(proto->return_type) - 1] = '\0';
            } else if (strcmp(proto->return_type, ret) != 0 && strcmp(ret, "int") != 0) {
                codegen_error(NULL, "conflicting inferred return types for imported function %s.%s: %s vs %s",
                              module, name, proto->return_type, ret);
                return;
            }

            if (arg_types && !proto->has_arg_types) {
                proto->arg_count = arg_count;
                proto->has_arg_types = 1;
                for (int j = 0; j < arg_count && j < 16; j++) {
                    strncpy(proto->arg_types[j], arg_types[j], sizeof(proto->arg_types[j]) - 1);
                }
            }
            return;
        }
    }

    if (imported_function_proto_count >= (int)(sizeof(imported_function_protos) / sizeof(imported_function_protos[0]))) {
        codegen_error(NULL, "too many imported function prototypes");
        return;
    }

    ImportedFunctionProto* proto = &imported_function_protos[imported_function_proto_count++];
    memset(proto, 0, sizeof(*proto));
    strncpy(proto->module, module, sizeof(proto->module) - 1);
    strncpy(proto->name, name, sizeof(proto->name) - 1);
    strncpy(proto->return_type, ret, sizeof(proto->return_type) - 1);
    if (arg_types) {
        proto->arg_count = arg_count;
        proto->has_arg_types = 1;
        for (int j = 0; j < arg_count && j < 16; j++) {
            strncpy(proto->arg_types[j], arg_types[j], sizeof(proto->arg_types[j]) - 1);
        }
    }
}

static ImportedFunctionProto* find_imported_function_proto(const char* module, const char* name) {
    for (int i = 0; i < imported_function_proto_count; i++) {
        ImportedFunctionProto* proto = &imported_function_protos[i];
        if (strcmp(proto->module, module) == 0 && strcmp(proto->name, name) == 0) {
            return proto;
        }
    }
    return NULL;
}


static int is_string_expression(ASTNode* arg) {
    if (!arg) return 0;

    if (arg->type == AST_IDENTIFIER) {
        const char* type = get_local_variable_type(arg->text);
        return type && (strcmp(type, "string") == 0 || strcmp(type, "come_string_t*") == 0);
    }

    if (arg->type == AST_METHOD_CALL || arg->type == AST_CALL) {
        const char* name = arg->text;
        return strcmp(name, "upper") == 0 || strcmp(name, "lower") == 0 ||
            strcmp(name, "repeat") == 0 || strcmp(name, "replace") == 0 ||
            strcmp(name, "trim") == 0 || strcmp(name, "ltrim") == 0 ||
            strcmp(name, "rtrim") == 0 || strcmp(name, "substr") == 0 ||
            strcmp(name, "join") == 0 || strcmp(name, "new") == 0 ||
            strcmp(name, "str") == 0 || strcmp(name, "gets") == 0;
    }

    if (arg->type == AST_ARRAY_ACCESS) {
        ASTNode* arr = arg->children[0];
        if (arr->type == AST_IDENTIFIER) {
            const char* type = get_local_variable_type(arr->text);
            if (type && (strstr(type, "string[]") || strstr(type, "come_string_list_t"))) return 1;
            return strcmp(arr->text, "parts") == 0 || strcmp(arr->text, "groups") == 0 ||
                strcmp(arr->text, "regex_parts") == 0 || strcmp(arr->text, "args") == 0;
        }
    }

    return 0;
}

static int is_std_member(ASTNode* node, const char* member) {
    return node && node->type == AST_MEMBER_ACCESS &&
        node->children[0] && node->children[0]->type == AST_IDENTIFIER &&
        strcmp(node->children[0]->text, "std") == 0 &&
        strcmp(node->text, member) == 0;
}

static const char* infer_expression_type(ASTNode* node) {
    if (!node) return NULL;

    switch (node->type) {
        case AST_STRING_LITERAL:
            return "string";
        case AST_BOOL_LITERAL:
            return "bool";
        case AST_NUMBER:
            return infer_const_type(node);
        case AST_IDENTIFIER: {
            if (strcmp(node->text, "null") == 0) return "null";
            const char* local_type = get_local_variable_type(node->text);
            if (local_type) return local_type;
            return find_const_symbol_type(node->text);
        }
        case AST_CAST:
            return node->children[0] ? node->children[0]->text : NULL;
        case AST_TERNARY:
            return infer_expression_type(node->children[1]);
        case AST_BINARY_OP:
            if (strcmp(node->text, "==") == 0 || strcmp(node->text, "!=") == 0 ||
                strcmp(node->text, "<") == 0 || strcmp(node->text, ">") == 0 ||
                strcmp(node->text, "<=") == 0 || strcmp(node->text, ">=") == 0 ||
                strcmp(node->text, "&&") == 0 || strcmp(node->text, "||") == 0) {
                return "bool";
            }
            return infer_expression_type(node->children[0]);
        case AST_ARRAY_ACCESS: {
            const char* arr_type = infer_expression_type(node->children[0]);
            if (is_string_list_type_name(arr_type)) return "string";
            if (arr_type && strstr(arr_type, "[]")) {
                static char elem_type[128];
                size_t len = (size_t)(strstr(arr_type, "[]") - arr_type);
                if (len >= sizeof(elem_type)) len = sizeof(elem_type) - 1;
                strncpy(elem_type, arr_type, len);
                elem_type[len] = '\0';
                return elem_type;
            }
            return NULL;
        }
        case AST_CALL: {
            TupleReturnInfo* tuple = find_tuple_info_by_source(node->text);
            if (tuple) return tuple->tuple_type;
            return NULL;
        }
        case AST_METHOD_CALL: {
            ASTNode* receiver = node->children[0];
            const char* method = node->text;

            if (receiver->type == AST_IDENTIFIER && strcmp(receiver->text, "std") == 0) {
                if (strcmp(method, "tmpfile") == 0) return "FILE";
                if (strcmp(method, "tmpname") == 0) return "string";
                if (strcmp(method, "remove") == 0) return "bool";
            }

            if (is_std_member(receiver, "proc")) {
                if (strcmp(method, "getenv") == 0) return "string";
                if (strcmp(method, "system") == 0) return "int";
            }

            const char* recv_type = infer_expression_type(receiver);
            if (is_file_type_name(recv_type) || is_std_member(receiver, "in") ||
                is_std_member(receiver, "out") || is_std_member(receiver, "err")) {
                if (strcmp(method, "open") == 0 || strcmp(method, "fdopen") == 0 ||
                    strcmp(method, "reopen") == 0 || strcmp(method, "isopen") == 0 ||
                    strcmp(method, "eof") == 0 || strcmp(method, "error") == 0) return "bool";
                if (strcmp(method, "fileno") == 0 || strcmp(method, "printf") == 0 ||
                    strcmp(method, "scanf") == 0 || strcmp(method, "vprintf") == 0 ||
                    strcmp(method, "vscanf") == 0) return "int";
                if (strcmp(method, "read") == 0 || strcmp(method, "write") == 0 ||
                    strcmp(method, "puts") == 0) return "uint";
                if (strcmp(method, "getc") == 0) return "wchar";
                if (strcmp(method, "gets") == 0 || strcmp(method, "fname") == 0) return "string";
                if (strcmp(method, "tell") == 0) return "long";
                return "void";
            }

            if (strcmp(method, "upper") == 0 || strcmp(method, "lower") == 0 ||
                strcmp(method, "repeat") == 0 || strcmp(method, "replace") == 0 ||
                strcmp(method, "trim") == 0 || strcmp(method, "ltrim") == 0 ||
                strcmp(method, "rtrim") == 0 || strcmp(method, "substr") == 0 ||
                strcmp(method, "join") == 0 || strcmp(method, "regex_replace") == 0 ||
                strcmp(method, "at") == 0) return "string";
            if (strcmp(method, "split") == 0 || strcmp(method, "split_n") == 0 ||
                strcmp(method, "regex_split") == 0 || strcmp(method, "regex_groups") == 0) return "string[]";
            if (strcmp(method, "regex") == 0 || strcmp(method, "isdigit") == 0 ||
                strcmp(method, "isalpha") == 0 || strcmp(method, "isalnum") == 0 ||
                strcmp(method, "isspace") == 0 || strcmp(method, "isascii") == 0) return "bool";
            if (strcmp(method, "len") == 0 || strcmp(method, "length") == 0 ||
                strcmp(method, "size") == 0 || strcmp(method, "count") == 0) return "uint";
            if (strcmp(method, "cmp") == 0 || strcmp(method, "casecmp") == 0 ||
                strcmp(method, "find") == 0 || strcmp(method, "rfind") == 0 ||
                strcmp(method, "chr") == 0 || strcmp(method, "rchr") == 0 ||
                strcmp(method, "memchr") == 0 || strcmp(method, "tol") == 0) return "int";
            if (strcmp(method, "owner") == 0) return "owner";
            return NULL;
        }
        default:
            return NULL;
    }
}

static int is_string_expr_for_cmp(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_STRING_LITERAL) return 1;
    return is_string_type_name(infer_expression_type(node));
}

static void generate_expression_as_type(FILE* f, ASTNode* node, const char* type) {
    if (is_string_type_name(type)) {
        if (node && node->type == AST_STRING_LITERAL) {
            fprintf(f, "come_string_new(COME_CTX, ");
            generate_expression(f, node);
            fprintf(f, ")");
            return;
        }
        if (node && node->type == AST_TERNARY) {
            fprintf(f, "(");
            generate_expression(f, node->children[0]);
            fprintf(f, " ? ");
            generate_expression_as_type(f, node->children[1], "string");
            fprintf(f, " : ");
            generate_expression_as_type(f, node->children[2], "string");
            fprintf(f, ")");
            return;
        }
    }
    generate_expression(f, node);
}

static void emit_printf_call(FILE* f, const char* prefix, ASTNode* node) {
    char* fmt_modified = NULL;
    int* bool_args = NULL;
    int* wide_int_args = NULL;
    int arg_count = node->child_count;

    fprintf(f, "%s", prefix);

    if (arg_count > 1 && node->children[1]->type == AST_STRING_LITERAL) {
        bool_args = calloc(arg_count + 1, sizeof(int));
        wide_int_args = calloc(arg_count + 1, sizeof(int));
        char* raw_fmt = node->children[1]->text;
        fmt_modified = calloc(strlen(raw_fmt) * 2 + 100, 1);

        char* src = raw_fmt;
        char* dst = fmt_modified;
        int current_arg_idx = 2;

        while (*src) {
            if (*src == '%' && *(src + 1) != '%') {
                *dst++ = *src++;

                while (*src && strchr("-+ #0", *src)) *dst++ = *src++;

                if (*src == '*') {
                    *dst++ = *src++;
                    current_arg_idx++;
                } else {
                    while (*src && isdigit((unsigned char)*src)) *dst++ = *src++;
                }

                if (*src == '.') {
                    *dst++ = *src++;
                    if (*src == '*') {
                        *dst++ = *src++;
                        current_arg_idx++;
                    } else {
                        while (*src && isdigit((unsigned char)*src)) *dst++ = *src++;
                    }
                }

                char length_mod[8];
                int length_len = 0;
                while (*src && strchr("hljzL", *src) && length_len < 7) {
                    length_mod[length_len++] = *src++;
                }
                length_mod[length_len] = '\0';

                char spec = *src;
                if (strcmp(length_mod, "l") == 0 && spec && strchr("diuoxX", spec)) {
                    if (current_arg_idx < arg_count) {
                        wide_int_args[current_arg_idx] = strchr("uoxX", spec) ? 2 : 1;
                    }
                    *dst++ = 'l';
                    *dst++ = 'l';
                } else {
                    for (int li = 0; li < length_len; li++) *dst++ = length_mod[li];
                }

                if (*src == 't') {
                    if (current_arg_idx < arg_count) bool_args[current_arg_idx] = 1;
                    *dst++ = 's';
                    src++;
                } else if (*src == 'T') {
                    if (current_arg_idx < arg_count) bool_args[current_arg_idx] = 2;
                    *dst++ = 's';
                    src++;
                } else {
                    if (*src) *dst++ = *src++;
                }
                current_arg_idx++;
            } else if (*src == '%' && *(src + 1) == '%') {
                *dst++ = *src++;
                *dst++ = *src++;
            } else {
                *dst++ = *src++;
            }
        }
        *dst = '\0';
    }

    for (int i = 1; i < node->child_count; i++) {
        if (i > 1) fprintf(f, ", ");

        if (i == 1 && fmt_modified) {
            fprintf(f, "%s", fmt_modified);
            continue;
        }

        if (bool_args && bool_args[i] != 0) {
            fprintf(f, "(");
            generate_expression(f, node->children[i]);
            fprintf(f, bool_args[i] == 1 ? " ? \"true\" : \"false\")" : " ? \"TRUE\" : \"FALSE\")");
            continue;
        }

        if (wide_int_args && wide_int_args[i] != 0) {
            fprintf(f, "((%s)(", wide_int_args[i] == 2 ? "unsigned long long" : "long long");
            generate_expression(f, node->children[i]);
            fprintf(f, "))");
            continue;
        }

        ASTNode* arg = node->children[i];
        if (arg->type == AST_STRING_LITERAL) {
            generate_expression(f, arg);
        } else if (is_string_expression(arg)) {
            fprintf(f, "(");
            generate_expression(f, arg);
            fprintf(f, " ? ");
            generate_expression(f, arg);
            fprintf(f, "->data : \"NULL\")");
        } else {
            generate_expression(f, arg);
        }
    }

    fprintf(f, ")");
    free(fmt_modified);
    free(bool_args);
    free(wide_int_args);
}


static void generate_expression(FILE* f, ASTNode* node) {
    if (!node) {
        fprintf(f, "/* AST ERROR: NULL NODE */ 0");
        return;
    }
    if (node->type == AST_STRING_LITERAL) {
        // Emit raw literal. Context handles wrapping.
        emit_c_string_literal(f, node->text);
    } else if (node->type == AST_BOOL_LITERAL) {
        fprintf(f, "%s", node->text); // true or false
    } else if (node->type == AST_NUMBER) {
        if (node->text[0] == '\'') {
            // Check for multi-byte char
            int len = strlen(node->text);
            int is_multibyte = 0;
            for (int i=1; i<len-1; i++) {
                if ((unsigned char)node->text[i] >= 0x80) { is_multibyte = 1; break; }
            }
            if (is_multibyte) fprintf(f, "L");
        }
        fprintf(f, "%s", node->text);
    } else if (node->type == AST_IDENTIFIER) {
        if (strcmp(node->text, "null") == 0) fprintf(f, "NULL");
        else fprintf(f, "%s", node->text);
    } else if (node->type == AST_UNARY_OP) {
        fprintf(f, "(%s", node->text);
        generate_expression(f, node->children[0]);
        fprintf(f, ")");
    } else if (node->type == AST_ARRAY_ACCESS) {
        // COME_ARR_GET(arr, index)
        fprintf(f, "COME_ARR_GET(");
        generate_expression(f, node->children[0]);
        fprintf(f, ", ");
        generate_expression(f, node->children[1]);
        fprintf(f, ")");
    } else if (node->type == AST_ASSIGN) {
        generate_expression(f, node->children[0]);
        fprintf(f, " %s ", node->text);
        generate_expression(f, node->children[1]);
    } else if (node->type == AST_MEMBER_ACCESS) {
        // Special case: "data" access on "scaled"/"dyn"/"buf" array access -> just the value.
        // This fixes the issue where parser/codegen erroneously treats int/byte array access as needing .data
        if (strcmp(node->text, "data") == 0 && node->children[0]->type == AST_ARRAY_ACCESS) {
             ASTNode* arr = node->children[0]->children[0];
             
             if (arr->type == AST_IDENTIFIER && (
                 strcmp(arr->text, "scaled") == 0 ||
                 strcmp(arr->text, "dyn") == 0 ||
                 strcmp(arr->text, "buf") == 0 ||
                 strcmp(arr->text, "arr") == 0)) { /* Added 'arr' just in case */
                 
                 // Do NOT dereference '*' because COME_ARR_GET returns 'int' (the value), not pointer.
                 // Just emit the array access expression itself.
                 generate_expression(f, node->children[0]);
                 return;
             }
        }

        // Member access - use dot for struct values, arrow for pointers
        fprintf(f, "(");
        generate_expression(f, node->children[0]);
        
        int is_ptr = is_pointer_expression(node->children[0]);
        // Debug print to stderr (will show in compiler output)
        if (node->children[0]->type == AST_IDENTIFIER && strcmp(node->children[0]->text, "p1") == 0) {
             fprintf(f, ").%s", node->text);
        } else if (is_ptr) {
             fprintf(f, ")->%s", node->text);
        } else {
             fprintf(f, ").%s", node->text);
        }
    } else if (node->type == AST_METHOD_CALL) {
        char* method = node->text;
        char c_func[16384];
        int skip_receiver = 0;
        ASTNode* receiver = node->children[0];
        const char* std_file_object = NULL;
        const char* std_proc_object = NULL;
        int std_wrap_string_args = 0;
        
        // Detect module static calls
        int is_import = 0;
        if (receiver->type == AST_IDENTIFIER) {
            for (int i=0; i<current_import_count; i++) {
                if (strcmp(receiver->text, current_imports[i]) == 0) { is_import = 1; break; }
            }
        }
        ImportedFunctionProto* imported_proto = NULL;
        if (is_import && receiver->type == AST_IDENTIFIER) {
            imported_proto = find_imported_function_proto(receiver->text, method);
        }
        
        if (receiver->type == AST_IDENTIFIER && (
            strcmp(receiver->text, "net")==0 || 
            strcmp(receiver->text, "conv")==0 || 
            strcmp(receiver->text, "mem")==0 ||
            strcmp(receiver->text, "std")==0 ||
            strcmp(receiver->text, "ERR")==0 ||
            is_import)) {
            
            skip_receiver = 1;
            
             if (strcmp(receiver->text, "mem")==0 && strcmp(method, "cpy")==0) {
                 strcpy(c_func, "memcpy");
             } else if (strcmp(receiver->text, "std")==0 && strcmp(method, "printf")==0) {
                 emit_printf_call(f, "printf(", node);
                 return;
             } else if (strcmp(receiver->text, "std")==0 &&
                        (strcmp(method, "remove") == 0 || strcmp(method, "tmpfile") == 0 || strcmp(method, "tmpname") == 0)) {
                 snprintf(c_func, sizeof(c_func), "come_std__%s", method);
                 std_wrap_string_args = 1;
             } else {
                 if (is_import) {
                     // New schema for imported modules: come_MODULE__FUNC
                     snprintf(c_func, sizeof(c_func), "come_%s__%s", receiver->text, method);
                 } else {
                     // Legacy schema for core modules
                     snprintf(c_func, sizeof(c_func), "come_%s_%s", receiver->text, method);
                 }
             }
        } 
        // Detect std.out.printf / std.err.printf
        else if (receiver->type == AST_MEMBER_ACCESS && 
                 strcmp(receiver->children[0]->text, "std") == 0) {
            
             if ((strcmp(receiver->text, "out") == 0 || strcmp(receiver->text, "err") == 0) && strcmp(method, "printf") == 0) {
                 emit_printf_call(f, strcmp(receiver->text, "out") == 0 ? "fprintf(stdout, " : "fprintf(stderr, ", node);
                 return;
             }
             if (strcmp(receiver->text, "in") == 0 || strcmp(receiver->text, "out") == 0 || strcmp(receiver->text, "err") == 0) {
                 if (strcmp(receiver->text, "in") == 0) std_file_object = "&std_in";
                 else if (strcmp(receiver->text, "out") == 0) std_file_object = "&std_out";
                 else std_file_object = "&std_err";
                 snprintf(c_func, sizeof(c_func), "come_std__FILE__%s", method);
                 skip_receiver = 1;
                 std_wrap_string_args = 1;
             } else if (strcmp(receiver->text, "proc") == 0) {
                 std_proc_object = "&std_proc";
                 snprintf(c_func, sizeof(c_func), "come_std__Proc__%s", method);
                 skip_receiver = 1;
                 std_wrap_string_args = 1;
             }
        }
        else if (strcmp(method, "owner") == 0) {
            fprintf(f, "mem_talloc_parent(");
            generate_expression(f, receiver);
            fprintf(f, ")");
            return;
        }
        else if (strcmp(method, "chown") == 0 && node->child_count > 1) {
            fprintf(f, "mem_talloc_steal(");
            generate_expression(f, node->children[1]);
            fprintf(f, ", ");
            generate_expression(f, receiver);
            fprintf(f, ")");
            return;
        }
        else if (receiver->type == AST_IDENTIFIER && is_file_type_name(get_local_variable_type(receiver->text))) {
            snprintf(c_func, sizeof(c_func), "come_std__FILE__%s", method);
            std_wrap_string_args = 1;
        }
        else if (receiver->type == AST_IDENTIFIER && get_local_variable_type(receiver->text) &&
                 strcmp(get_local_variable_type(receiver->text), "Proc") == 0) {
            snprintf(c_func, sizeof(c_func), "come_std__Proc__%s", method);
            std_wrap_string_args = 1;
        }
        // Detect net.tls calls
        else if (receiver->type == AST_MEMBER_ACCESS &&
            strcmp(receiver->text, "tls") == 0 &&
            receiver->children[0]->type == AST_IDENTIFIER &&
            strcmp(receiver->children[0]->text, "net") == 0) {
            
            if (strcmp(method, "listen") == 0) {
                 snprintf(c_func, sizeof(c_func), "come_net_tls_%s_helper", method);
            } else {
                 snprintf(c_func, sizeof(c_func), "net_tls_%s", method);
            }
            skip_receiver = 1;
            // Also we need to inject NULL as mem_ctx?
            // Existing logic loops arguments.
            // come_net_tls_listen_helper(mem_ctx, ip, port, ctx).
            // We need to inject mem_ctx FIRST.
            // generate_expression logic:
            // fprintf(f, "%s(", c_func); // func name
            // Loop children[1..]
            // We need to inject "NULL, " before first arg.
            // We can modify 'c_func' to include it? No.
            // We need to flag "inject_null_ctx".
        }
        // Detect net.http calls (static) like net.http.new()
        else if (receiver->type == AST_MEMBER_ACCESS &&
            strcmp(receiver->text, "http") == 0 &&
            receiver->children[0]->type == AST_IDENTIFIER &&
            strcmp(receiver->children[0]->text, "net") == 0) {
            
            if (strcmp(method, "new") == 0) {
                 snprintf(c_func, sizeof(c_func), "come_net_http_%s_default", method);
                 // Need to inject mem_ctx (NULL)
                 // If child_count == 1 (only receiver), then args are empty.
                 // We need to pass NULL.
            } else {
                 snprintf(c_func, sizeof(c_func), "net_http_%s", method);
            }
            skip_receiver = 1;
        } 
        else if (strcmp(method, "accept") == 0) {
            strcpy(c_func, "come_call_accept");
        }
        else if (strcmp(method, "attach") == 0) {
            strcpy(c_func, "net_http_attach");
        }
        else if (strcmp(method, "send") == 0) {
            if (receiver->type == AST_IDENTIFIER && strcmp(receiver->text, "resp") == 0) {
                strcpy(c_func, "net_http_response_send");
            } else {
                strcpy(c_func, "net_http_request_send");
            }
        }
        else if (strcmp(method, "on") == 0 && node->child_count > 1) {
             ASTNode* event = node->children[1];
             if (event->type == AST_IDENTIFIER) {
                 if (strcmp(event->text, "ACCEPT") == 0) strcpy(c_func, "net_tls_on_accept");
                 else if (strcmp(event->text, "READ_DONE") == 0) strcpy(c_func, "net_http_req_on_ready");
             } else if (event->type == AST_NUMBER) {
                 // Enum values?
                 strcpy(c_func, "on"); 
             }
        }
        // Detect Map methods (put, get, remove only - not len which is shared with string)
        else if (strcmp(method, "put") == 0 || strcmp(method, "get") == 0 || strcmp(method, "remove") == 0) {
             int is_map = 0;
             if (receiver->type == AST_IDENTIFIER) {
                 const char* type = get_local_variable_type(receiver->text);
                 if (is_map_type_name(type)) {
                     is_map = 1;
                 }
             }
             
             if (is_map) {
                 snprintf(c_func, sizeof(c_func), "come_map_%s", method);
                 
                 fprintf(f, "%s(", c_func);
                 if (strcmp(method, "put") == 0) {
                     fprintf(f, "&"); // come_map_put needs map**
                 }
                 generate_expression(f, receiver);
                 for (int i = 1; i < node->child_count; i++) {
                     fprintf(f, ", ");
                     if (i == 1 && node->children[i]->type == AST_STRING_LITERAL) {
                         fprintf(f, "come_string_new(COME_CTX, ");
                         generate_expression(f, node->children[i]);
                         fprintf(f, ")");
                     } else {
                         generate_expression(f, node->children[i]);
                     }
                 }
                 fprintf(f, ")");
                 return;
             }
        }
        // Detect String methods (including len/length which are shared)
        else if (strcmp(method, "length") == 0 || strcmp(method, "len") == 0 || 
                 strcmp(method, "cmp") == 0 || strcmp(method, "casecmp") == 0 ||
                 strcmp(method, "upper") == 0 || strcmp(method, "lower") == 0 ||
                 strcmp(method, "trim") == 0 || strcmp(method, "ltrim") == 0 || strcmp(method, "rtrim") == 0 ||
                 strcmp(method, "replace") == 0 || strcmp(method, "split") == 0 ||
                 strcmp(method, "join") == 0 || strcmp(method, "substr") == 0 || 
                 strcmp(method, "find") == 0 || strcmp(method, "rfind") == 0 || strcmp(method, "count") == 0 ||
                 strcmp(method, "chr") == 0 || strcmp(method, "rchr") == 0 || strcmp(method, "memchr") == 0 ||
                 strcmp(method, "isdigit") == 0 || strcmp(method, "isalpha") == 0 || 
                 strcmp(method, "isalnum") == 0 || strcmp(method, "isspace") == 0 || strcmp(method, "isascii") == 0 ||
                 strcmp(method, "repeat") == 0 || strcmp(method, "split_n") == 0 ||
                  strcmp(method, "regex") == 0 || strncmp(method, "regex_", 6) == 0 ||
                  strcmp(method, "tol") == 0 ||
                  strcmp(method, "byte_array") == 0) {
            
            // Check if receiver is a map for len() - maps also have len()
            if (strcmp(method, "len") == 0 && receiver->type == AST_IDENTIFIER) {
                const char* type = get_local_variable_type(receiver->text);
                if (is_map_type_name(type)) {
                    // Map len
                    fprintf(f, "come_map_len(");
                    generate_expression(f, receiver);
                    fprintf(f, ")");
                    return;
                }
                // Check if receiver is a string list (string[] or args)
                if (type && (strstr(type, "string[]") != NULL || strstr(type, "come_string_list_t") != NULL)) {
                    fprintf(f, "come_string_list_len(");
                    generate_expression(f, receiver);
                    fprintf(f, ")");
                    return;
                }
                // Special case: args is always a string list
                if (strcmp(receiver->text, "args") == 0) {
                    fprintf(f, "come_string_list_len(");
                    generate_expression(f, receiver);
                    fprintf(f, ")");
                    return;
                }
            }
                 
            if (strcmp(method, "length") == 0) strcpy(c_func, "come_string_list_len"); 
            else if (strcmp(method, "tol") == 0) strcpy(c_func, "come_string_tol");
            else snprintf(c_func, sizeof(c_func), "come_string_%s", method);
        }
        // Detect Array methods
        else if (strcmp(method, "size") == 0 || strcmp(method, "resize") == 0 || strcmp(method, "free") == 0 || strcmp(method, "slice") == 0) {
             if (strcmp(method, "free") == 0) strcpy(c_func, "come_free");
             else if (strcmp(method, "size") == 0) strcpy(c_func, "come_array_size");
             else if (strcmp(method, "slice") == 0) strcpy(c_func, "come_array_slice");
             else snprintf(c_func, sizeof(c_func), "come_array_%s", method);
        }
        else {
             // User defined struct method?
             // Try to resolve receiver variable type
             ASTNode* receiver = node->children[0];
             char struct_name[64] = "";
             if (receiver->type == AST_IDENTIFIER) {
                 const char* type = get_local_variable_type(receiver->text);
                 if (type) {
                     if (strncmp(type, "struct ", 7) == 0) {
                         strcpy(struct_name, type + 7);
                     } else {
                         strcpy(struct_name, type);
                     }
                 }
             }

             if (struct_name[0]) {
                 // Found struct type: generate Struct_Method(&receiver, ...)
                 // New rule: come_MMM__SSS__FFF
                 fprintf(f, "come_%s__%s__%s(", current_module, struct_name, method);
                 // Need address of receiver if it is a struct value
                 // If receiver is a pointer?
                 // Current logic: we only track "struct Rect" -> we need &?
                 // Yes, "struct Rect r" -> &r passed to "Rect_area(Rect* self)".
                 // If "Rect* r" -> r passed.
                 // Our tracker should distinguish?
                 // For now assume stack structs need &.
                 const char* full_type = get_local_variable_type(receiver->text);
                 if (strstr(full_type, "*") == NULL) {
                     fprintf(f, "&");
                 }
                 generate_expression(f, receiver);
                 // Args
                 for (int i = 1; i < node->child_count; i++) {
                     fprintf(f, ", ");
                     generate_expression(f, node->children[i]);
                 }
                 fprintf(f, ")");
                 return;
             }

            // Generic method: method(receiver, ...)
            // e.g. nport(addr)
            strcpy(c_func, method);
        }
        
        fprintf(f, "%s(", c_func);
        
        // Handle arguments
        int first_arg = 1;
        
        // Append ctx for specific functions?
        if (strcmp(c_func, "come_string_sprintf") == 0) {
            fprintf(f, "COME_CTX");
            first_arg = 0;
        }

        if (strcmp(c_func, "come_net_tls_listen_helper") == 0 || strcmp(c_func, "come_net_http_new_default") == 0) {
            fprintf(f, "NULL"); // Inject mem_ctx
            if (node->child_count > 1) { // If there are args, add comma
                fprintf(f, ", ");
            }
            first_arg = 1; 
        }

        if (std_file_object) {
            fprintf(f, "%s", std_file_object);
            first_arg = 0;
        } else if (std_proc_object) {
            fprintf(f, "%s", std_proc_object);
            first_arg = 0;
        }
        
        // Receiver mechanism (skip_receiver handles skipping actual printing of receiver)
        if (!skip_receiver) {
            if (!first_arg) fprintf(f, ", ");
            
             if (strcmp(method, "join") == 0) {
                  ASTNode* list = (node->child_count > 1) ? node->children[1] : NULL;
                  if (list) generate_expression(f, list);
                  else fprintf(f, "NULL");
                  fprintf(f, ", ");
                  
                  if (receiver->type == AST_STRING_LITERAL) {
                      fprintf(f, "come_string_new(NULL, ");
                      generate_expression(f, receiver);
                      fprintf(f, ")");
                  } else {
                      generate_expression(f, receiver);
                  }
                  first_arg = 0;
             } else {
                if (receiver->type == AST_STRING_LITERAL) {
                     fprintf(f, "come_string_new(NULL, ");
                     generate_expression(f, receiver);
                     fprintf(f, ")");
                } else {
                     generate_expression(f, receiver);
                }
                first_arg = 0;
             }
        }
        
        // Arguments
        for (int i = 1; i < node->child_count; i++) {
             if (strcmp(method, "join") == 0 && i == 1) continue; // Handled
             
             ASTNode* arg = node->children[i];
             if (arg->type == AST_BLOCK) {
                 // Trailing closure!
                 fprintf(f, ", ({ ");
                 if (strcmp(c_func, "net_tls_on_accept") == 0) {
                      fprintf(f, "void __cb(net_tls_listener* l, net_tls_connection* c) ");
                 } else if (strcmp(c_func, "net_http_req_on_ready") == 0) {
                      fprintf(f, "void __cb(net_http_request* r) ");
                 } else {
                      fprintf(f, "void __cb(void* a, void* b) "); // dummy
                 }
                 fprintf(f, "{ ");
                 generate_node(f, arg, 0); // Emit block
                 fprintf(f, " } __cb; })");
                 continue;
             }
             
             if (!first_arg) fprintf(f, ", ");
             
             // Wrapper logic for string methods
              const char* imported_arg_type = NULL;
              if (imported_proto && imported_proto->has_arg_types && (i - 1) < imported_proto->arg_count) {
                  imported_arg_type = imported_proto->arg_types[i - 1];
              }
              if (imported_arg_type && is_string_type_name(imported_arg_type) && arg->type == AST_STRING_LITERAL) {
                    fprintf(f, "come_string_new(COME_CTX, ");
                    generate_expression(f, arg);
                    fprintf(f, ")");
              } else if (std_wrap_string_args && arg->type == AST_STRING_LITERAL) {
                    fprintf(f, "come_string_new(COME_CTX, ");
                    generate_expression(f, arg);
                    fprintf(f, ")");
              } else if ((strcmp(method, "cmp") == 0 || strcmp(method, "casecmp") == 0) && arg->type == AST_STRING_LITERAL) {
                     fprintf(f, "come_string_new(NULL, ");
                     generate_expression(f, arg);
                    fprintf(f, ")");
             } else {
                 generate_expression(f, arg);
             }
             first_arg = 0;
        }
        
        // Optional args for string methods
        if ((strcmp(method, "cmp") == 0 || strcmp(method, "casecmp") == 0) && node->child_count == 2) {
            fputs(", 0", f);
        }
        if (strcmp(method, "replace") == 0 && node->child_count == 3) {
            fputs(", 0", f);
        }
        if (strcmp(method, "regex_split") == 0 && node->child_count == 2) {
            fputs(", 0", f);
        }
        if (strcmp(method, "regex_replace") == 0 && node->child_count == 3) {
            fputs(", 0", f);
        }
        if ((strcmp(method, "trim") == 0 || strcmp(method, "ltrim") == 0 || strcmp(method, "rtrim") == 0) && node->child_count == 1) {
            fputs(", NULL", f);
        }
        fprintf(f, ")");
    } else if (node->type == AST_CALL) {
        // Function call: func(args)
        // node->text is function name (e.g. "print", "foo")
        
        // Aliases like printf are replaced by parser? 
        // If parser alias replacement happens, node->text might be std_out_printf?
        // Wait, parser alias substitution replaces the node or text.
        // If it's a known C function alias (e.g. printf), we might NOT want to mangle it if it's meant to be C.
        // BUT user said "translate come function to C".
        // Standard lib functions...
        // If I call `add(1,2)`, it should become `come_main__add(1,2)`.
        // If I call `printf`, and `printf` is aliased to `std.out.printf`.
        // `std` module functions should be `come_std__...`?
        // `printf` is usually a macro or extern.
        // The parser handles alias.
        
        char mangled_name[8192];
        // Heuristic: if contains '.', it's fully qualified? No, AST_CALL text usually simple.
        // If it starts with "come_", assume already mangled/internal?
        if (strncmp(node->text, "come_", 5) == 0 || strncmp(node->text, "std_", 4) == 0) {
             strcpy(mangled_name, node->text);
        } else {
             // Local function call
             snprintf(mangled_name, sizeof(mangled_name), "come_%s__%s", current_module, node->text);
        }
        
        fprintf(f, "%s(", mangled_name);
        for (int i = 0; i < node->child_count; i++) {
            if (i > 0) fprintf(f, ", ");
            generate_expression(f, node->children[i]);
        }
        fprintf(f, ")");
    } else if (node->type == AST_AGGREGATE_INIT) {
        // { val, val } or { .field = val, ... }
        fprintf(f, "{ ");
        if (node->child_count == 0) {
            fprintf(f, "0");
        } else {
            for (int i = 0; i < node->child_count; i++) {
                ASTNode* child = node->children[i];
                
                // Check if this is a designated initializer (.field = value)
                if (child->type == AST_ASSIGN && child->child_count >= 2) {
                    ASTNode* designator = child->children[0];
                    ASTNode* value = child->children[1];
                    
                    // Check if designator starts with '.'
                    if (designator->type == AST_IDENTIFIER && designator->text[0] == '.') {
                        // Emit as designated initializer
                        fprintf(f, "%s = ", designator->text);
                        generate_expression(f, value);
                    } else {
                        // Regular assignment, shouldn't happen in initializer
                        generate_expression(f, child);
                    }
                } else {
                    generate_expression(f, child);
                }
                
        if (i < node->child_count - 1) fprintf(f, ", ");
            }
        }
        fprintf(f, " }");
    } else if (node->type == AST_CAST) {
        fprintf(f, "(%s) ", node->children[0]->text);
        generate_expression(f, node->children[1]);
    } else if (node->type == AST_TERNARY) {
        fprintf(f, "(");
        generate_expression(f, node->children[0]);
        fprintf(f, " ? ");
        generate_expression(f, node->children[1]);
        fprintf(f, " : ");
        generate_expression(f, node->children[2]);
        fprintf(f, ")");
    } else if (node->type == AST_UNARY_OP) {
        fprintf(f, "%s", node->text); 
        generate_expression(f, node->children[0]);
    } else if (node->type == AST_POST_INC) {
        generate_expression(f, node->children[0]);
        fprintf(f, "++");
    } else if (node->type == AST_POST_DEC) {
        generate_expression(f, node->children[0]);
        fprintf(f, "--");
    } else if (node->type == AST_BINARY_OP) {
        // Check if this is a string comparison (== or !=)
        int is_string_cmp = 0;
        int is_eq = (strcmp(node->text, "==") == 0);
        int is_neq = (strcmp(node->text, "!=") == 0);
        
        ASTNode* left = node->children[0];
        ASTNode* right = node->children[1];
        
        // Check if either operand is null - use pointer comparison, not string comparison
        int left_is_null = (left->type == AST_IDENTIFIER && strcmp(left->text, "null") == 0);
        int right_is_null = (right->type == AST_IDENTIFIER && strcmp(right->text, "null") == 0);
        
        if (is_eq || is_neq) {
            // If comparing with null, use pointer comparison
            if (left_is_null || right_is_null) {
                is_string_cmp = 0; // Don't use string comparison for null checks
            } else {
                int left_is_str = is_string_expr_for_cmp(left);
                int right_is_str = is_string_expr_for_cmp(right);
                is_string_cmp = (left_is_str && right_is_str);
            }
        }
        
        if (is_string_cmp) {
            // Generate strcmp() call
            fprintf(f, "(come_string_cmp(");
            generate_expression_as_type(f, node->children[0], "string");
            fprintf(f, ", ");
            generate_expression_as_type(f, node->children[1], "string");
            fprintf(f, ", 0) %s 0)", is_eq ? "==" : "!=");
        } else {
            fprintf(f, "(");
            generate_expression(f, node->children[0]);
            fprintf(f, " %s ", node->text);
            generate_expression(f, node->children[1]);
            fprintf(f, ")");
        }
    } else if (node->type == AST_CALL) {
        // Function Call or Operator
        // Check if text is operator
        char* op = node->text;
        int is_op = 0;
        const char* ops[] = {"+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=", "&&", "||", "&", "|", "^", "<<", ">>", "!"};
        for(int i=0; i<sizeof(ops)/sizeof(char*); i++) {
             if (strcmp(op, ops[i])==0) is_op = 1;
        }
        
        if (is_op) {
             if (strcmp(op, "!") == 0) {
                 fprintf(f, "(!");
                 generate_expression(f, node->children[0]);
                 fprintf(f, ")");
             } else {
                 fprintf(f, "(");
                 generate_expression(f, node->children[0]);
                 fprintf(f, " %s ", op);
                 if (node->child_count > 1) generate_expression(f, node->children[1]);
                 fprintf(f, ")");
             }
        } else {
            fprintf(f, "%s(", node->text);
            for (int i=0; i < node->child_count; i++) {
                 generate_expression(f, node->children[i]);
                 if (i < node->child_count - 1) fprintf(f, ", ");
            }
            fprintf(f, ")");
        }
    }
}

static void generate_node(FILE* f, ASTNode* node, int indent);

static void generate_program(FILE* f, ASTNode* node) {
    for (int i = 0; i < node->child_count; i++) {
        generate_node(f, node->children[i], 0);
        fputc('\n', f);
    }
}

static void generate_node(FILE* f, ASTNode* node, int indent) {
    if (!node) return;
    
    switch (node->type) {
        case AST_PROGRAM:
            generate_program(f, node);
            break;


      case AST_EXPORT:
          // Ignore exports in C codegen, visibility handled by C static/extern rules or just everything is visible for now
          break;

      case AST_FUNCTION: {
        // [RetType] [Name] [Args...] [Block/Body]
        emit_line_directive(f, node);

        reset_local_variables();
        // Register arguments
        // Children: 0=ret, 1..=args (until block)
        for (int i = 1; i < node->child_count; i++) {
             ASTNode* child = node->children[i];
             if (child->type == AST_VAR_DECL) {
                 if (child->child_count > 1) {
                     add_local_variable(child->text, child->children[1]->text);
                 }
             }
             if (child->type == AST_BLOCK) break;
        }
        
        ASTNode* ret_type = node->children[0];
        int body_idx = node->child_count - 1;
        
        current_tuple_return = find_tuple_info_by_source(node->text);
        strncpy(current_function_return_type, ret_type->text, sizeof(current_function_return_type) - 1);
        current_function_return_type[sizeof(current_function_return_type) - 1] = '\0';
        
        int is_main = (strcmp(node->text, "main") == 0);
        char func_name[8192];
        // Rule: come_MMM__FFF
        if (is_main && strcmp(current_module, "main") == 0) {
             // Main function in main module -> come_main__main
             // But we need to make sure our entry point knows this.
             // Existing entry point calls _come_user_main.
             // We will change that entry point to call come_main__main.
             snprintf(func_name, sizeof(func_name), "come_%s__%s", current_module, node->text);
        } else {
             snprintf(func_name, sizeof(func_name), "come_%s__%s", current_module, node->text);
        }
        
        // Check for Struct Method mangling (Struct_Method)
        // If the parser already mangled it to Struct_Method, we need to convert to come_MMM__Struct__Method
        // Wait, parser does mangle Struct.Method -> Struct_Method.
        // But the user wants: come_MMM__SSS__FFF
        // So if name contains '_', split it?
        // Parser mangles `Rect.area` -> `Rect_area`.
        // We should check if it looks like a struct method.
        // We can't be 100% sure just by name, but assuming convention.
        // Actually, for AST_FUNCTION, `node->text` is the function name.
        // If it was `Rect.area`, parser made it `Rect_area`.
        // Let's replace the first '_' with '__'.
        // No, user wants come_MMM__SSS__FFF.
        // If node->text is "Rect_area", we want "come_MMM__Rect__area".
        // Let's see if we can detect it.
        char* underscore = strchr(node->text, '_');
        if (strcmp(node->text, "init") == 0) {
             snprintf(func_name, sizeof(func_name), "come_%s__init_local", current_module);
        } else if (strcmp(node->text, "exit") == 0) {
             snprintf(func_name, sizeof(func_name), "come_%s__exit_local", current_module);
        } else if (underscore && !is_main && isupper(node->text[0])) {

            // Struct method: first char is uppercase (e.g., "Rect_area")
            // Convert "Rect_area" -> "come_MMM__Rect__area"
            long prefix_len = underscore - node->text;
            snprintf(func_name, sizeof(func_name), "come_%s__%.*s__%s", current_module, (int)prefix_len, node->text, underscore + 1);
        } else {
             // Regular function (including those with underscores like "demo_types")
             snprintf(func_name, sizeof(func_name), "come_%s__%s", current_module, node->text);
        }


        emit_indent(f, indent);
        
        // Return type
        // Handle "byte" etc alias?? no, just print text
        if (current_tuple_return) {
            fprintf(f, "%s %s(", current_tuple_return->struct_name, func_name);
        } else {
            emit_c_type(f, ret_type->text);
            fprintf(f, " %s(", func_name);
        }
        
        // Args
        int has_args = 0;
        
        // Special case: nport injector
        if (strcmp(node->text, "nport") == 0) {
            fprintf(f, "struct TCP_ADDR* self");
            has_args = 1;
        } else if (strcmp(node->text, "module_init") == 0) {
            fprintf(f, "TALLOC_CTX* ctx");
            has_args = 1;
        }

        
        // Iterate manual args
        for (int i = 1; i < body_idx; i++) {
            if (has_args) fprintf(f, ", ");
            
            ASTNode* arg = node->children[i];
            if (arg->type == AST_VAR_DECL) {
                // int x
                ASTNode* type = arg->children[1];
                
                // array?
                if (strstr(type->text, "[]")) {
                    // int input[] -> come_int_array_t* input
                     char raw[64];
                     strncpy(raw, type->text, strlen(type->text)-2);
                     raw[strlen(type->text)-2] = 0;
                     if (strcmp(raw, "int")==0) fprintf(f, "come_int_array_t* %s", arg->text);
                     else if (strcmp(raw, "byte")==0) fprintf(f, "come_byte_array_t* %s", arg->text);
                     else if (strcmp(raw, "string")==0) fprintf(f, "come_string_list_t* %s", arg->text);
                     else fprintf(f, "come_array_t* %s", arg->text);
                } else if (is_main && strncmp(arg->text, "args", 4) == 0 && (strcmp(type->text, "string") == 0 || strcmp(type->text, "string[]") == 0)) {
                    // special case for main(string args) -> we pass string list
                    fprintf(f, "come_string_list_t* %s", arg->text);
                } else {
                    emit_c_type(f, type->text);
                    fprintf(f, " %s", arg->text);
                }
            } else {
                // Fallback
                fprintf(f, "void* %s", arg->text);
            }
            has_args = 1;
        }

        if (!has_args) {
            fprintf(f, "void");
        }
        
        fprintf(f, ")");
        
        ASTNode* body = node->children[body_idx];
        if (body->type == AST_BLOCK) {
            fprintf(f, " {\n");
            if (strcmp(node->text, "module_init") == 0) {
                fprintf(f, "    COME_CTX = ctx;\n");
                for (int i=0; i<current_import_count; i++) {
                    fprintf(f, "    come_%s__ctx = ctx;\n", current_imports[i]);
                }
            }

            
            for (int i = 0; i < body->child_count; i++) {
                generate_node(f, body->children[i], indent + 4);
            }
            if (is_main) {
                emit_indent(f, indent + 4);
                fprintf(f, "return 0;\n");
            }
            emit_indent(f, indent);
            fprintf(f, "}\n");
        } else {
            fprintf(f, ";\n");
        }

        
        current_tuple_return = NULL;
        return;
    }
    
    case AST_TYPE_ALIAS: {
        // Handled in Pass -1
        // fprintf(f, "typedef %s %s;\n", node->children[0]->text, node->text);
        break;
    }

    
    case AST_VAR_DECL: {
        emit_line_directive(f, node);  // Emit #line for variable declaration
        ASTNode* type_node = node->children[1];
        ASTNode* init_expr = node->children[0];

        if (strcmp(type_node->text, "var") == 0) {
            if (!init_expr) {
                add_local_variable(node->text, "var");
                break;
            }

            const char* inferred = infer_expression_type(init_expr);
            if (!inferred || strcmp(inferred, "null") == 0 || inferred[0] == '(') {
                codegen_error(node, "cannot infer concrete type for var '%s'", node->text);
                break;
            }

            add_local_variable(node->text, inferred);
            emit_indent(f, indent);
            emit_c_type(f, inferred);
            fprintf(f, " %s = ", node->text);
            generate_expression_as_type(f, init_expr, inferred);
            fprintf(f, ";\n");
            break;
        }

        add_local_variable(node->text, type_node->text);
        
        emit_indent(f, indent);
            if (strcmp(type_node->text, "string") == 0) {
                fprintf(f, "come_string_t* %s = ", node->text);
                if (init_expr->type == AST_STRING_LITERAL) {
                    fprintf(f, "come_string_new(COME_CTX, ");
                    generate_expression(f, init_expr);
                    fprintf(f, ")");
                } else {
                    generate_expression(f, init_expr);
                }
                fprintf(f, ";\n");
            } else if (strcmp(type_node->text, "string[]") == 0) {
                fprintf(f, "come_string_list_t* %s = ", node->text);
                if (init_expr->type == AST_STRING_LITERAL && strcmp(init_expr->text, "\"__ARGS__\"") == 0) {
                    fprintf(f, "come_string_list_from_argv(COME_CTX, argc, argv)");
                } else {
                    generate_expression(f, init_expr);
                }
                fprintf(f, ";\n");
                // Mark as potentially unused to avoid warnings
                emit_indent(f, indent);
                fprintf(f, "(void)%s;\n", node->text);
            } else if (is_map_type_name(type_node->text)) {
                fprintf(f, "map %s = ", node->text);
                if (!init_expr ||
                    (init_expr->type == AST_AGGREGATE_INIT && init_expr->child_count == 0) ||
                    (init_expr->type == AST_NUMBER && strcmp(init_expr->text, "0") == 0)) {
                    fprintf(f, "come_map_new(COME_CTX)");
                } else {
                    generate_expression(f, init_expr);
                }
                fprintf(f, ";\n");
            } else if (strcmp(type_node->text, "bool") == 0) {
                fprintf(f, "bool %s = ", node->text);
                generate_expression(f, init_expr);
                fprintf(f, ";\n");
            } else if (strcmp(type_node->text, "var") == 0) {
                // Type inference
                if (init_expr->type == AST_STRING_LITERAL) {
                    fprintf(f, "come_string_t* %s = come_string_new(COME_CTX, ", node->text);
                    generate_expression(f, init_expr);
                    fprintf(f, ");\n");
                } else {
                    fprintf(f, "__auto_type %s = ", node->text);
                    generate_expression(f, init_expr);
                    fprintf(f, ";\n");
                }
            } else {
                // Generic case: T x = ...
                // Check if type ends in []
                char* lbracket = strchr(type_node->text, '[');
                if (lbracket) {
                    char raw_type[64];
                    strncpy(raw_type, type_node->text, lbracket - type_node->text);
                    raw_type[lbracket - type_node->text] = '\0';
                    
                    int fixed_size = 0;
                    if (lbracket[1] != ']') {
                        fixed_size = atoi(lbracket + 1);
                    }

                    char arr_type[128];
                    char elem_type[64];
                    strcpy(elem_type, raw_type);
                    if (strcmp(raw_type, "int")==0) { strcpy(arr_type, "come_int_array_t"); }
                    else if (strcmp(raw_type, "byte")==0) { strcpy(arr_type, "come_byte_array_t"); strcpy(elem_type, "uint8_t"); }
                    else if (strcmp(raw_type, "var")==0) { strcpy(arr_type, "come_int_array_t"); strcpy(elem_type, "int"); }
                    else { snprintf(arr_type, sizeof(arr_type), "come_array_%s_t", raw_type); }
                    
                    if (init_expr && init_expr->type == AST_AGGREGATE_INIT) {
                        int count = init_expr->child_count;
                        int alloc_count = (fixed_size > count) ? fixed_size : count;
                        
                        fprintf(f, "%s* %s = (%s*)mem_talloc_alloc(COME_CTX, sizeof(uint32_t)*2 + %u * sizeof(%s));\n", 
                                arr_type, node->text, arr_type, alloc_count, elem_type);
                        emit_indent(f, indent);
                        fprintf(f, "%s->size = %u; %s->count = %u;\n", node->text, alloc_count, node->text, count);
                        emit_indent(f, indent);
                        fprintf(f, "{ %s _vals[] = ", elem_type);
                        generate_expression(f, init_expr);
                        fprintf(f, "; memcpy(%s->items, _vals, sizeof(_vals)); }\n", node->text);
                    } else if (init_expr) {
                        // Initialized from expression (e.g. slice, function return)
                        fprintf(f, "%s* %s = ", arr_type, node->text);
                        generate_expression(f, init_expr);
                        fprintf(f, ";\n");
                    } else if (fixed_size > 0) {
                        fprintf(f, "%s* %s = (%s*)mem_talloc_alloc(COME_CTX, sizeof(uint32_t)*2 + %u * sizeof(%s));\n", 
                                arr_type, node->text, arr_type, fixed_size, elem_type);
                        emit_indent(f, indent);
                        fprintf(f, "memset(%s->items, 0, %u * sizeof(%s));\n", node->text, fixed_size, elem_type);
                        emit_indent(f, indent);
                        fprintf(f, "%s->size = %u; %s->count = %u;\n", node->text, fixed_size, node->text, fixed_size);
                    } else {
                        // Empty dynamic
                        fprintf(f, "%s* %s = (%s*)mem_talloc_alloc(COME_CTX, sizeof(uint32_t)*2);\n", arr_type, node->text, arr_type);
                        emit_indent(f, indent);
                        fprintf(f, "%s->size = 0; %s->count = 0;\n", node->text, node->text);
                    }
                }
 else {
                     emit_c_type(f, type_node->text);
                     fprintf(f, " %s = ", node->text);
                     
                     // For struct types with aggregate initializers, preserve the syntax
                     if (init_expr && init_expr->type == AST_AGGREGATE_INIT && 
                         strncmp(type_node->text, "struct", 6) == 0) {
                         // Just emit the aggregate initializer as-is for structs
                         generate_expression(f, init_expr);
                     } else if (init_expr && init_expr->type == AST_NUMBER && strcmp(init_expr->text, "0") == 0) {
                          // Check if type is struct or union?
                          if (strncmp(type_node->text, "struct", 6) == 0 || strncmp(type_node->text, "union", 5) == 0) {
                              fprintf(f, "{0}");
                          } else {
                          generate_expression_as_type(f, init_expr, type_node->text);
                      }
                     } else {
                         generate_expression(f, init_expr);
                     }
                     fprintf(f, ";\n");
                }
            }
            break;
        }

    case AST_TUPLE_DESTRUCT: {
            emit_line_directive(f, node);
            if (node->child_count < 2) {
                codegen_error(node, "tuple destructuring requires names and a value");
                break;
            }

            ASTNode* rhs = node->children[node->child_count - 1];
            TupleReturnInfo* tuple = NULL;
            if (rhs->type == AST_CALL) {
                tuple = find_tuple_info_by_source(rhs->text);
            }

            if (!tuple) {
                const char* rhs_type = infer_expression_type(rhs);
                if (rhs_type && rhs_type[0] == '(' && rhs->type == AST_CALL) {
                    tuple = find_tuple_info_by_source(rhs->text);
                }
            }

            int target_count = node->child_count - 1;
            if (!tuple || tuple->elem_count != target_count) {
                codegen_error(node, "tuple destructuring arity/type mismatch");
                break;
            }

            char temp_name[64];
            snprintf(temp_name, sizeof(temp_name), "__come_tuple_%d", tuple_temp_counter++);

            emit_indent(f, indent);
            fprintf(f, "%s %s = ", tuple->struct_name, temp_name);
            generate_expression(f, rhs);
            fprintf(f, ";\n");

            for (int i = 0; i < target_count; i++) {
                ASTNode* ident = node->children[i];
                const char* elem_type = tuple->elem_types[i];
                add_local_variable(ident->text, elem_type);
                emit_indent(f, indent);
                emit_c_type(f, elem_type);
                fprintf(f, " %s = %s._%d;\n", ident->text, temp_name, i);
            }
            break;
        }

    case AST_PRINTF: {
            emit_indent(f, indent);
            fputs("printf(", f);
            emit_c_string_literal(f, node->text);
            
            for (int i = 0; i < node->child_count; i++) {
                fputs(", ", f);
                ASTNode* arg = node->children[i];
                
                if (arg->type == AST_STRING_LITERAL) {
                    emit_c_string_literal(f, arg->text);
                } else if (arg->type == AST_IDENTIFIER) {
                    const char* type = get_local_variable_type(arg->text);
                    int is_str = (type && (strcmp(type, "string") == 0 || strcmp(type, "come_string_t*") == 0));
                    if (is_str) {
                        fprintf(f, "(%s ? %s->data : \"NULL\")", arg->text, arg->text);
                    } else {
                        generate_expression(f, arg);
                    }
                      } else if (arg->type == AST_METHOD_CALL) {
                    char* m = arg->text;
                    if (strcmp(m, "upper")==0 || strcmp(m, "lower")==0 || strcmp(m, "repeat")==0 || 
                        strcmp(m, "replace")==0 || strcmp(m, "trim")==0 || strcmp(m, "ltrim")==0 || 
                        strcmp(m, "rtrim")==0 || strcmp(m, "join")==0 || strcmp(m, "substr")==0 || 
                        strcmp(m, "regex_replace")==0 || strcmp(m, "str")==0) {
                         fprintf(f, "(");
                         generate_expression(f, arg);
                         fprintf(f, ")->data");
                    } else {
                        // Cast to int for numeric results to satisfy printf %d
                        fprintf(f, "(int)(");
                        generate_expression(f, arg);
                        fprintf(f, ")");
                    }
                } else if (arg->type == AST_ARRAY_ACCESS) {
                    // ((arr)->items[index])->data ?
                    // Only if it's a string array!
                    // Check array name for known int/byte arrays: scaled, dyn, buf, arr
                    int is_numeric = 0;
                    ASTNode* arr_node = arg->children[0];
                    if (arr_node->type == AST_IDENTIFIER) {
                         if (strcmp(arr_node->text, "scaled")==0 || 
                             strcmp(arr_node->text, "dyn")==0 ||
                             strcmp(arr_node->text, "buf")==0 ||
                             strcmp(arr_node->text, "arr")==0) {
                             is_numeric = 1;
                         }
                    }
                    
                    if (is_numeric) {
                        generate_expression(f, arg);
                    } else {
                        fprintf(f, "(");
                        generate_expression(f, arg);
                        fprintf(f, ")->data");
                    }
                } else {
                    generate_expression(f, arg);
                }
            }
            fputs(");\n", f);
            break;
        }

        case AST_IF: {
            emit_line_directive(f, node);  // Emit #line for if statement
            emit_indent(f, indent);
            fprintf(f, "if (");
            generate_expression(f, node->children[0]);
            fprintf(f, ") {\n");
            generate_node(f, node->children[1], indent + 4);
            emit_indent(f, indent);
            fprintf(f, "}");
            if (node->child_count > 2) {
                fprintf(f, " else {\n");
                generate_node(f, node->children[2], indent + 4);
                emit_indent(f, indent);
                fprintf(f, "}\n");
            } else {
                fputc('\n', f);
            }
            break;
        }
        
        case AST_ELSE: {
            // Just generate the child statement
            generate_node(f, node->children[0], indent);
            break;
        }
        
        case AST_BLOCK: {
            for (int i = 0; i < node->child_count; i++) {
                generate_node(f, node->children[i], indent);
            }
            break;
        }

        case AST_RETURN: {
            emit_line_directive(f, node);
            emit_indent(f, indent);
            if (current_tuple_return) {
                fprintf(f, "return (%s){", current_tuple_return->struct_name);
                for (int i = 0; i < current_tuple_return->elem_count; i++) {
                    if (i > 0) fprintf(f, ", ");
                    if (i < node->child_count) {
                        generate_expression_as_type(f, node->children[i], current_tuple_return->elem_types[i]);
                    } else {
                        fprintf(f, "0");
                    }
                }
                fprintf(f, "};\n");
            } else if (strcmp(current_function_return_type, "void") == 0) {
                 fprintf(f, "return;\n");
            } else {
                fprintf(f, "return");
                if (node->child_count > 0) {
                    fprintf(f, " ");
                    generate_expression_as_type(f, node->children[0], current_function_return_type);
                } else {
                    fprintf(f, " 0");
                }
                fprintf(f, ";\n");
            }
            break;
        }
        
        case AST_METHOD_CALL: {
            emit_indent(f, indent);
            generate_expression(f, node);
            fprintf(f, ";\n");
            break;
        }


        
        case AST_STRUCT_DECL: {
            emit_line_directive(f, node);
            emit_indent(f, indent);
            fprintf(f, "struct %s {\n", node->text);
            for (int i = 0; i < node->child_count; i++) {
                 // Skip methods
                 if (node->children[i]->type == AST_FUNCTION) continue; 
                 
                 // Handle Fields (AST_VAR_DECL) without init
                 ASTNode* field = node->children[i];
                 if (field->type == AST_VAR_DECL) {
                     ASTNode* type = field->children[1];
                     emit_indent(f, indent + 4);
                     // Check array
                     int len = strlen(type->text);
                     if (len > 2 && strcmp(type->text + len - 2, "[]") == 0) {
                         char raw_type[64];
                         strncpy(raw_type, type->text, len - 2);
                         raw_type[len-2] = '\0';
                         // Fixed size array in struct? 
                         // "byte ipaddr[16]" -> parser logic?
                         // Parser likely parsed "byte" and name "ipaddr[16]"?
                         // Or type "byte[]"?
                         // If parser put dimensions in name, just print name.
                         // If type is "byte[]", we don't know size here unless in name.
                         // Let's assume standard type printing.
                         // Fix: if type ends in [], map to come_byte_array_t* for structs
                         // Or use pointer? byte* items.
                         // But we want to support size?
                         // "byte[]" usually come_byte_array_t* in my codegen.
                         fprintf(f, "come_%s_array_t* %s;\n", raw_type, field->text);
                     } else {
                         fprintf(f, "%s %s;\n", c_type_name(type->text), field->text);
                     }
                 } else {
                     generate_node(f, field, indent + 4);
                 }
            }
            fprintf(f, "};\n");
            emit_indent(f, indent);
            if (!is_struct_seen(node->text)) {
                fprintf(f, "typedef struct %s %s;\n", node->text, node->text);
                mark_struct_seen(node->text);
            }
            break;
        }

        case AST_ASSIGN: {
            emit_line_directive(f, node);  // Emit #line for assignment
            ASTNode* lhs = node->children[0];
            ASTNode* rhs = node->children[1];

            if (lhs && lhs->type == AST_IDENTIFIER && strcmp(node->text, "=") == 0) {
                const char* lhs_type = get_local_variable_type(lhs->text);
                const char* rhs_type = infer_expression_type(rhs);

                if (lhs_type && strcmp(lhs_type, "var") == 0) {
                    if (!rhs_type || strcmp(rhs_type, "null") == 0 || rhs_type[0] == '(') {
                        codegen_error(node, "cannot infer concrete type for var '%s'", lhs->text);
                        break;
                    }
                    update_local_variable_type(lhs->text, rhs_type);
                    emit_indent(f, indent);
                    emit_c_type(f, rhs_type);
                    fprintf(f, " %s = ", lhs->text);
                    generate_expression_as_type(f, rhs, rhs_type);
                    fprintf(f, ";\n");
                    break;
                }

                if (lhs_type && rhs_type && strcmp(rhs_type, "null") != 0 && !type_compatible(lhs_type, rhs_type)) {
                    codegen_error(node, "cannot assign %s to locked variable '%s' of type %s", rhs_type, lhs->text, lhs_type);
                    break;
                }

                if (lhs_type && is_string_type_name(lhs_type)) {
                    emit_indent(f, indent);
                    generate_expression(f, lhs);
                    fprintf(f, " = ");
                    generate_expression_as_type(f, rhs, lhs_type);
                    fprintf(f, ";\n");
                    break;
                }
            }

            emit_indent(f, indent);
            generate_expression(f, lhs);
            fprintf(f, " %s ", node->text);
            generate_expression(f, rhs);
            fprintf(f, ";\n");
            break;
        }





        case AST_CONST_GROUP: {
            // Check if it's an enum group
            int is_enum_group = 0;
            if (node->child_count > 0 && node->children[0]->child_count > 0 && 
                node->children[0]->children[0]->type == AST_ENUM_DECL) {
                is_enum_group = 1;
            }

            if (is_enum_group) {
                emit_line_directive(f, node);
                emit_indent(f, indent);
                fprintf(f, "enum {\n");
                for (int i = 0; i < node->child_count; i++) {
                    ASTNode* const_decl = node->children[i];
                    ASTNode* enum_decl = const_decl->children[0];
                    emit_indent(f, indent + 4);
                    fprintf(f, "%s", const_decl->text);
                    if (enum_decl->child_count > 0 && enum_decl->children[0]->type == AST_NUMBER) {
                        fprintf(f, " = %s", enum_decl->children[0]->text);
                        enum_counter = atoi(enum_decl->children[0]->text);
                    }
                    enum_counter++;
                    if (i < node->child_count - 1) fprintf(f, ",");
                    fprintf(f, "\n");
                }
                emit_indent(f, indent);
                fprintf(f, "};\n");
            } else {
                for (int i = 0; i < node->child_count; i++) {
                    generate_node(f, node->children[i], indent);
                }
            }
            break;
        }

        case AST_CONST_DECL: {
            emit_indent(f, indent);
            if (node->child_count > 0 && node->children[0]->type == AST_ENUM_DECL) {
                // Enum
                ASTNode* en = node->children[0];
                int val = enum_counter++;
                
                // Check if explicit init
                if (en->child_count > 0 && en->children[0]->type == AST_NUMBER) {
                     val = atoi(en->children[0]->text);
                     enum_counter = val + 1;
                }
                
                fprintf(f, "enum { %s = %d };\n", node->text, val);
            } else {
                const char* type = infer_const_type(node->children[0]);
                fprintf(f, "const %s %s = ", type, node->text);
                generate_expression(f, node->children[0]);
                fprintf(f, ";\n");
            }
            break;
        }
        
        case AST_UNION_DECL: {
            // union Name { ... };
            emit_indent(f, indent);
            fprintf(f, "union %s {\n", node->text);
            for (int i = 0; i < node->child_count; i++) {
                // Handle Fields (AST_VAR_DECL) without init
                ASTNode* field = node->children[i];
                if (field->type == AST_VAR_DECL) {
                    ASTNode* type = field->children[1];
                    emit_indent(f, indent + 4);
                    fprintf(f, "%s %s;\n", c_type_name(type->text), field->text);
                } else {
                    generate_node(f, field, indent + 4);
                }
            }
            fprintf(f, "};\n");
            fprintf(f, "typedef union %s %s;\n", node->text, node->text);
            break;
        }

        case AST_SWITCH: {
            emit_indent(f, indent);
            fprintf(f, "switch (");
            generate_expression(f, node->children[0]);
            fprintf(f, ") {\n");
            for (int i=1; i < node->child_count; i++) {
                generate_node(f, node->children[i], indent+4);
            }
            emit_indent(f, indent);
            fprintf(f, "}\n");
            break;
        }
        
        case AST_CASE: {
            emit_indent(f, indent);
            fprintf(f, "case ");
            generate_expression(f, node->children[0]);
            fprintf(f, ": {\n");
            int has_fallthrough = node->child_count > 1 &&
                node->children[node->child_count - 1]->type == AST_FALLTHROUGH;
            int stmt_end = has_fallthrough ? node->child_count - 1 : node->child_count;
            for (int i=1; i < stmt_end; i++) {
                generate_node(f, node->children[i], indent+4);
            }
            if (!has_fallthrough) {
                emit_indent(f, indent+4);
                fprintf(f, "break;\n");
            }
            emit_indent(f, indent);
            fprintf(f, "}\n");
            break;
        }
        
        case AST_DEFAULT: {
            emit_indent(f, indent);
            fprintf(f, "default: {\n");
            for (int i=0; i < node->child_count; i++) {
                generate_node(f, node->children[i], indent+4);
            }
            fprintf(f, "}\n");
            break;
        }
        
        case AST_WHILE: {
            emit_line_directive(f, node);
            emit_indent(f, indent);
            fprintf(f, "while (");
            generate_expression(f, node->children[0]);
            fprintf(f, ") {\n");
            // Body is a block usually?
            ASTNode* body = node->children[1];
            if (body->type == AST_BLOCK) {
                 for(int i=0; i<body->child_count; i++) generate_node(f, body->children[i], indent+4);
            } else {
                 generate_node(f, body, indent+4);
            }
            emit_indent(f, indent);
            fprintf(f, "}\n");
            break;
        }
        
        case AST_DO_WHILE: {
            emit_line_directive(f, node);
            emit_indent(f, indent);
            fprintf(f, "do {\n");
             ASTNode* body = node->children[0];
            if (body->type == AST_BLOCK) {
                 for(int i=0; i<body->child_count; i++) generate_node(f, body->children[i], indent+4);
            } else {
                 generate_node(f, body, indent+4);
            }
            emit_indent(f, indent);
            fprintf(f, "} while (");
            generate_expression(f, node->children[1]);
            fprintf(f, ");\n");
            break;
        }
        
        
        case AST_CALL:
        case AST_POST_INC:
        case AST_POST_DEC:
        case AST_BINARY_OP:
        case AST_IDENTIFIER: {
            emit_line_directive(f, node);
            emit_indent(f, indent);
            generate_expression(f, node);
            fprintf(f, ";\n");
            break;
        }

        case AST_FOR: {
            emit_line_directive(f, node);
            emit_indent(f, indent);
            fprintf(f, "for (");
            // children[0]: init
            if (node->children[0]) {
                if (node->children[0]->type == AST_VAR_DECL) {
                    // Variable declaration in for: int i = 0
                    // generate_node(f, node->children[0], 0); // This adds indent and \n; we need raw.
                    // Actually let's assume it's common.
                    ASTNode* decl = node->children[0];
                    ASTNode* type = decl->children[1];
                    fprintf(f, "%s %s = ", c_type_name(type->text), decl->text);
                    generate_expression(f, decl->children[0]);
                } else {
                    generate_expression(f, node->children[0]);
                }
            }
            fprintf(f, "; ");
            
            // children[1]: cond
            if (node->children[1]) {
                generate_expression(f, node->children[1]);
            }
            fprintf(f, "; ");

            // children[2]: iter
            if (node->children[2]) {
                generate_expression(f, node->children[2]);
            }
            fprintf(f, ") ");

            // children[3]: body
            ASTNode* body = node->children[3];
            if (body->type == AST_BLOCK) {
                fprintf(f, "{\n");
                for (int i = 0; i < body->child_count; i++) {
                    generate_node(f, body->children[i], indent + 4);
                }
                emit_indent(f, indent);
                fprintf(f, "}\n");
            } else {
                fprintf(f, "\n");
                generate_node(f, body, indent + 4);
            }
            break;
        }

        case AST_BREAK: {
            emit_indent(f, indent);
            fprintf(f, "break;\n");
            break;
        }
        case AST_CONTINUE: {
            emit_indent(f, indent);
            fprintf(f, "continue;\n");
            break;
        }
        case AST_FALLTHROUGH: {
            emit_indent(f, indent);
            fprintf(f, "/* fallthrough */\n");
            break;
        }
        default:
            break;
    }
}

static void collect_tuple_returns(ASTNode* ast) {
    tuple_return_count = 0;
    if (!ast || ast->type != AST_PROGRAM) return;

    for (int i = 0; i < ast->child_count && tuple_return_count < 128; i++) {
        ASTNode* child = ast->children[i];
        if (!child || child->type != AST_FUNCTION || child->child_count == 0) continue;
        ASTNode* ret = child->children[0];
        if (!ret || ret->type != AST_IDENTIFIER || ret->text[0] != '(') continue;

        TupleReturnInfo* info = &tuple_returns[tuple_return_count++];
        memset(info, 0, sizeof(*info));
        strncpy(info->source_name, child->text, sizeof(info->source_name) - 1);
        strncpy(info->tuple_type, ret->text, sizeof(info->tuple_type) - 1);
        info->elem_count = parse_tuple_types(ret->text, info->elem_types, 16);
        mangle_function_name(current_module, child->text, info->c_name, sizeof(info->c_name));

        char safe_name[512];
        sanitize_ident(info->c_name, safe_name, sizeof(safe_name));
        snprintf(info->struct_name, sizeof(info->struct_name), "%.*s__tuple_ret",
                 (int)sizeof(info->struct_name) - 32, safe_name);
    }
}

static void collect_imported_function_prototypes_node(ASTNode* node, const char* expected_type, const char* function_return_type) {
    if (!node) return;

    if (node->type == AST_VAR_DECL) {
        const char* declared_type = NULL;
        ASTNode* init = NULL;
        if (node->child_count > 1) {
            init = node->children[0];
            declared_type = node->children[1] ? node->children[1]->text : NULL;
        }
        collect_imported_function_prototypes_node(init, declared_type, function_return_type);
        if (declared_type) {
            add_local_variable(node->text, declared_type);
        }
        return;
    }

    if (node->type == AST_FUNCTION) {
        const char* ret = (node->child_count > 0 && node->children[0]) ? node->children[0]->text : NULL;
        reset_local_variables();
        for (int i = 1; i < node->child_count; i++) {
            ASTNode* child = node->children[i];
            if (child && child->type == AST_VAR_DECL && child->child_count > 1 && child->children[1]) {
                add_local_variable(child->text, child->children[1]->text);
            }
            if (child && child->type == AST_BLOCK) break;
        }
        for (int i = 1; i < node->child_count; i++) {
            collect_imported_function_prototypes_node(node->children[i], NULL, ret);
        }
        reset_local_variables();
        return;
    }

    if (node->type == AST_RETURN) {
        for (int i = 0; i < node->child_count; i++) {
            collect_imported_function_prototypes_node(node->children[i], function_return_type, function_return_type);
        }
        return;
    }

    if (node->type == AST_METHOD_CALL && node->child_count > 0) {
        ASTNode* receiver = node->children[0];
        if (receiver && receiver->type == AST_IDENTIFIER && is_imported_module_name(receiver->text)) {
            char arg_types[16][128];
            int arg_count = 0;
            memset(arg_types, 0, sizeof(arg_types));
            for (int i = 1; i < node->child_count && arg_count < 16; i++) {
                const char* arg_type = infer_expression_type(node->children[i]);
                if (!arg_type || !arg_type[0] || arg_type[0] == '(') arg_type = "void*";
                strncpy(arg_types[arg_count], arg_type, sizeof(arg_types[arg_count]) - 1);
                arg_count++;
            }
            register_imported_function_proto(receiver->text, node->text, expected_type, arg_types, arg_count);
        }
    }

    for (int i = 0; i < node->child_count; i++) {
        collect_imported_function_prototypes_node(node->children[i], NULL, function_return_type);
    }
}

static void collect_imported_function_prototypes(ASTNode* ast) {
    imported_function_proto_count = 0;
    collect_imported_function_prototypes_node(ast, NULL, NULL);
}

static void collect_const_symbols(ASTNode* ast) {
    reset_const_symbols();
    if (!ast || ast->type != AST_PROGRAM) return;

    for (int i = 0; i < ast->child_count; i++) {
        ASTNode* child = ast->children[i];
        if (!child) continue;

        if (child->type == AST_CONST_DECL) {
            if (child->child_count > 0 && child->children[0] &&
                child->children[0]->type == AST_ENUM_DECL) {
                add_const_symbol(child->text, "int");
            } else if (child->child_count > 0) {
                add_const_symbol(child->text, infer_const_type(child->children[0]));
            }
        } else if (child->type == AST_CONST_GROUP) {
            for (int j = 0; j < child->child_count; j++) {
                ASTNode* const_decl = child->children[j];
                if (!const_decl || const_decl->type != AST_CONST_DECL) continue;
                if (const_decl->child_count > 0 && const_decl->children[0] &&
                    const_decl->children[0]->type == AST_ENUM_DECL) {
                    add_const_symbol(const_decl->text, "int");
                } else if (const_decl->child_count > 0) {
                    add_const_symbol(const_decl->text, infer_const_type(const_decl->children[0]));
                }
            }
        }
    }
}

static void emit_prototype_arg_type(FILE* f, ASTNode* arg) {
    if (!arg || arg->type != AST_VAR_DECL || arg->child_count < 2 || !arg->children[1]) {
        fprintf(f, "void*");
        return;
    }

    ASTNode* type = arg->children[1];
    if (strstr(type->text, "[]")) {
        char raw[64];
        size_t len = strlen(type->text) - 2;
        if (len >= sizeof(raw)) len = sizeof(raw) - 1;
        strncpy(raw, type->text, len);
        raw[len] = '\0';

        if (strcmp(raw, "int") == 0) fprintf(f, "come_int_array_t*");
        else if (strcmp(raw, "byte") == 0) fprintf(f, "come_byte_array_t*");
        else if (strcmp(raw, "string") == 0) fprintf(f, "come_string_list_t*");
        else fprintf(f, "come_array_t*");
        return;
    }

    emit_c_type(f, type->text);
}

static void emit_function_prototype(FILE* f, ASTNode* func) {
    if (!func || func->type != AST_FUNCTION || strcmp(func->text, "main") == 0) return;

    char func_name[8192];
    mangle_function_name(current_module, func->text, func_name, sizeof(func_name));

    if (func->child_count > 0 && func->children[0] && func->children[0]->type != AST_BLOCK) {
        TupleReturnInfo* tuple = find_tuple_info_by_source(func->text);
        if (tuple) {
            fprintf(f, "%s %s(", tuple->struct_name, func_name);
        } else {
            emit_c_type(f, func->children[0]->text);
            fprintf(f, " %s(", func_name);
        }
    } else {
        fprintf(f, "void %s(", func_name);
    }

    int first = 1;
    int start_args = (func->child_count > 0 && func->children[0]->type == AST_BLOCK) ? 0 : 1;
    for (int j = start_args; j < func->child_count; j++) {
        if (func->children[j]->type == AST_BLOCK) break;
        if (!first) fprintf(f, ", ");
        emit_prototype_arg_type(f, func->children[j]);
        first = 0;
    }

    if (first) fprintf(f, "void");
    fprintf(f, ");\n");
}


int generate_c_from_ast(ASTNode* ast, const char* out_file, const char* source_file, int gen_line_map) {
    FILE* f = fopen(out_file, "w");
    if (!f) return 1;
    codegen_error_count = 0;
    tuple_temp_counter = 0;
    
    // Set source filename for #line directives
    static char src_filename[1024];
    strncpy(src_filename, source_file, sizeof(src_filename) - 1);
    src_filename[sizeof(src_filename) - 1] = '\0';
    source_filename = src_filename;
    g_gen_line_map = gen_line_map;
    
    // Reset seen structs tracker
    for (int i=0; i<seen_count; i++) free(seen_structs[i]);
    seen_count = 0;
    
    // Reset imports tracker
    for (int i=0; i<current_import_count; i++) free(current_imports[i]);
    current_import_count = 0;

    // First collect module name and imports
    if (ast->type == AST_PROGRAM) {
        if (ast->text[0] != 0) {
            strncpy(current_module, ast->text, 255);
            current_module[255] = '\0';
        } else {
            strcpy(current_module, "main");
        }
        for (int i=0; i<ast->child_count; i++) {
            if (ast->children[i]->type == AST_IMPORT) {
                current_imports[current_import_count++] = strdup(ast->children[i]->text);
            }
        }
    } else {
        strcpy(current_module, "main");
    }

    collect_tuple_returns(ast);
    collect_const_symbols(ast);
    collect_imported_function_prototypes(ast);


    fprintf(f, "#include <stdio.h>\n");
    fprintf(f, "#include <string.h>\n");
    fprintf(f, "#include <stdbool.h>\n");
    fprintf(f, "#include <stdint.h>\n");
    fprintf(f, "#include <stdarg.h>\n");
    fprintf(f, "#include \"come_string.h\"\n");
    fprintf(f, "#include \"come_array.h\"\n");
    fprintf(f, "#include \"come_map.h\"\n");
    fprintf(f, "#include \"come_types.h\"\n");
    fprintf(f, "#include \"mem/talloc.h\"\n");
    fprintf(f, "#include <errno.h>\n");
    fprintf(f, "#define come_errno_wrapper() (errno)\n");
    fprintf(f, "static __attribute__((unused)) const char* come_strerror() { return strerror(errno); }\n");
    // Auto-include headers for simple modules detection
    // In a real compiler this would be driven by the symbol table/imports
    // Net includes removed

    // Global ERR object externs if std is imported
    int std_imported = 0;
    for (int i = 0; i < current_import_count; i++) {
        if (strcmp(current_imports[i], "std") == 0) {
            std_imported = 1;
            break;
        }
    }
    if (std_imported) {
        fprintf(f, "extern int come_ERR_no(void);\n");
        fprintf(f, "extern come_string_t* come_ERR_str(void);\n");
        fprintf(f, "extern void come_ERR_clear(void);\n");
        // We need the type for ERR object too
        fprintf(f, "typedef struct come_std__ERR_t come_std__ERR_t;\n");
        fprintf(f, "extern come_std__ERR_t come_std__ERR;\n");
        fprintf(f, "typedef struct come_std__FILE come_std__FILE_t;\n");
        fprintf(f, "typedef struct come_std__Proc come_std__Proc_t;\n");
        fprintf(f, "extern come_std__FILE_t std_in;\n");
        fprintf(f, "extern come_std__FILE_t std_out;\n");
        fprintf(f, "extern come_std__FILE_t std_err;\n");
        fprintf(f, "extern come_std__Proc_t std_proc;\n");
        fprintf(f, "bool come_std__remove(come_string_t* path);\n");
        fprintf(f, "come_std__FILE_t* come_std__tmpfile(void);\n");
        fprintf(f, "come_string_t* come_std__tmpname(void);\n");
        fprintf(f, "bool come_std__FILE__open(come_std__FILE_t* self, come_string_t* path, come_string_t* mode);\n");
        fprintf(f, "void come_std__FILE__close(come_std__FILE_t* self);\n");
        fprintf(f, "bool come_std__FILE__fdopen(come_std__FILE_t* self, int fd, come_string_t* mode);\n");
        fprintf(f, "bool come_std__FILE__reopen(come_std__FILE_t* self, come_string_t* path, come_string_t* mode);\n");
        fprintf(f, "int come_std__FILE__fileno(come_std__FILE_t* self);\n");
        fprintf(f, "int come_std__FILE__printf(come_std__FILE_t* self, come_string_t* fmt, ...);\n");
        fprintf(f, "int come_std__FILE__scanf(come_std__FILE_t* self, come_string_t* fmt, ...);\n");
        fprintf(f, "int come_std__FILE__vprintf(come_std__FILE_t* self, come_string_t* fmt, va_list ap);\n");
        fprintf(f, "int come_std__FILE__vscanf(come_std__FILE_t* self, come_string_t* fmt, va_list ap);\n");
        fprintf(f, "uint come_std__FILE__read(come_std__FILE_t* self, come_byte_array_t* buf, uint n);\n");
        fprintf(f, "uint come_std__FILE__write(come_std__FILE_t* self, come_byte_array_t* buf, uint n);\n");
        fprintf(f, "wchar come_std__FILE__getc(come_std__FILE_t* self);\n");
        fprintf(f, "void come_std__FILE__putc(come_std__FILE_t* self, wchar c);\n");
        fprintf(f, "come_string_t* come_std__FILE__gets(come_std__FILE_t* self);\n");
        fprintf(f, "uint come_std__FILE__puts(come_std__FILE_t* self, come_string_t* s);\n");
        fprintf(f, "come_string_t* come_std__FILE__fname(come_std__FILE_t* self);\n");
        fprintf(f, "void come_std__FILE__ungetc(come_std__FILE_t* self, wchar c);\n");
        fprintf(f, "void come_std__FILE__seek(come_std__FILE_t* self, long offset, int whence);\n");
        fprintf(f, "long come_std__FILE__tell(come_std__FILE_t* self);\n");
        fprintf(f, "void come_std__FILE__rewind(come_std__FILE_t* self);\n");
        fprintf(f, "bool come_std__FILE__isopen(come_std__FILE_t* self);\n");
        fprintf(f, "bool come_std__FILE__eof(come_std__FILE_t* self);\n");
        fprintf(f, "bool come_std__FILE__error(come_std__FILE_t* self);\n");
        fprintf(f, "void come_std__FILE__flush(come_std__FILE_t* self);\n");
        fprintf(f, "void come_std__FILE__clearerr(come_std__FILE_t* self);\n");
        fprintf(f, "void come_std__FILE__setbuf(come_std__FILE_t* self, come_byte_array_t* buf, uint size);\n");
        fprintf(f, "void come_std__FILE__setvbuf(come_std__FILE_t* self, come_byte_array_t* buf, int mode, uint size);\n");
        fprintf(f, "void come_std__FILE__setlinebuf(come_std__FILE_t* self);\n");
        fprintf(f, "void come_std__Proc__abort(come_std__Proc_t* self);\n");
        fprintf(f, "void come_std__Proc__exit(come_std__Proc_t* self, int status);\n");
        fprintf(f, "void come_std__Proc__atexit(come_std__Proc_t* self, void* cb);\n");
        fprintf(f, "come_string_t* come_std__Proc__getenv(come_std__Proc_t* self, come_string_t* name);\n");
        fprintf(f, "int come_std__Proc__system(come_std__Proc_t* self, come_string_t* cmd);\n");
    }
    // Macros for method dispatch
    fprintf(f, "#define COME_CTX come_%s__ctx\n\n", current_module);
    
    // Module memory context
    fprintf(f, "TALLOC_CTX* come_%s__ctx = NULL;\n", current_module);
    
    for (int i = 0; i < current_import_count; i++) {
        if (!is_system_module_name(current_imports[i])) {
            fprintf(f, "extern TALLOC_CTX* come_%s__ctx;\n", current_imports[i]);
        }
    }
    for (int i = 0; i < imported_function_proto_count; i++) {
        ImportedFunctionProto* proto = &imported_function_protos[i];
        emit_c_type(f, proto->return_type);
        fprintf(f, " come_%s__%s(", proto->module, proto->name);
        if (!proto->has_arg_types || proto->arg_count == 0) {
            fprintf(f, "void");
        } else {
            for (int j = 0; j < proto->arg_count; j++) {
                if (j > 0) fprintf(f, ", ");
                emit_c_type(f, proto->arg_types[j]);
            }
        }
        fprintf(f, ");\n");
    }

    int has_main_function = 0;
    for (int i = 0; i < ast->child_count; i++) {
        ASTNode* child = ast->children[i];
        if (child && child->type == AST_FUNCTION && strcmp(child->text, "main") == 0) {
            has_main_function = 1;
            break;
        }
    }

    if (has_main_function) {

        // Scan AST to find main function and check if it has parameters
        int main_has_params = 0;
        for (int i = 0; i < ast->child_count; i++) {
            ASTNode* child = ast->children[i];
            if (child && child->type == AST_FUNCTION && strcmp(child->text, "main") == 0) {
                // Check if main has any arguments
                // Arguments are in children[1] if present and NOT a block
                if (child->child_count > 1 && child->children[1] && child->children[1]->type != AST_BLOCK) {
                    ASTNode* args_node = child->children[1];
                    if (args_node->child_count > 0) {
                        main_has_params = 1;
                    }
                }
                break;
            }
        }

        // Forward declare user main with correct signature
        if (main_has_params) {
            fprintf(f, "int come_%s__main(come_string_list_t* args);\n", current_module);
        } else {
            fprintf(f, "int come_%s__main(void);\n", current_module);
        }

        // Forward declare module init/exit
        fprintf(f, "void come_%s__init(void);\n", current_module);
        fprintf(f, "void come_%s__exit(void);\n", current_module);
        
        fprintf(f, "\nint main(int argc, char* argv[]) {\n");
        fprintf(f, "    COME_CTX = mem_talloc_new_ctx(NULL);\n");
        fprintf(f, "    if (!COME_CTX) { fprintf(stderr, \"OOM\\n\"); return 1; }\n");
        
        fprintf(f, "    come_%s__init();\n", current_module);
        fprintf(f, "    \n");
        
        if (main_has_params) {
            fprintf(f, "    // Convert argv to string[]\n");
            fprintf(f, "    come_string_list_t* args = come_string_list_from_argv(COME_CTX, argc, argv);\n");
            fprintf(f, "    \n");
            fprintf(f, "    // Call user main\n");
            fprintf(f, "    int ret = come_%s__main(args);\n", current_module);
        } else {
            fprintf(f, "    // Call user main (no args)\n");
            fprintf(f, "    int ret = come_%s__main();\n", current_module);
        }
        
        fprintf(f, "    \n");
        fprintf(f, "    come_%s__exit();\n", current_module);
        fprintf(f, "    mem_talloc_free(COME_CTX);\n");
        fprintf(f, "    return ret;\n");
        fprintf(f, "}\n");
    }

    // Generate Module Init/Exit Chain
    fprintf(f, "\n/* Module Init/Exit Chain */\n");
    
    // Forward declare imported init/exit
    for (int i = 0; i < current_import_count; i++) {
        fprintf(f, "extern void come_%s__init(void);\n", current_imports[i]);
        fprintf(f, "extern void come_%s__exit(void);\n", current_imports[i]);
    }

    // Module Init
    fprintf(f, "void come_%s__init(void) {\n", current_module);
    fprintf(f, "    static bool initialized = false;\n");
    fprintf(f, "    if (initialized) return;\n");
    fprintf(f, "    initialized = true;\n");
    for (int i = 0; i < current_import_count; i++) {
        if (!is_system_module_name(current_imports[i])) {
            fprintf(f, "    if (!come_%s__ctx) come_%s__ctx = mem_talloc_new_ctx(COME_CTX);\n",
                    current_imports[i], current_imports[i]);
        }
        fprintf(f, "    come_%s__init();\n", current_imports[i]);
    }
    // Call local init if defined (mangled as come_module__init_local to avoid collision)
    int has_local_init = 0;
    for (int i = 0; i < ast->child_count; i++) {
        if (ast->children[i]->type == AST_FUNCTION && strcmp(ast->children[i]->text, "init") == 0) {
            has_local_init = 1;
            break;
        }
    }
    if (has_local_init) {
        fprintf(f, "    come_%s__init_local();\n", current_module);
    }
    fprintf(f, "}\n\n");

    // Module Exit
    fprintf(f, "void come_%s__exit(void) {\n", current_module);
    fprintf(f, "    static bool exited = false;\n");
    fprintf(f, "    if (exited) return;\n");
    fprintf(f, "    exited = true;\n");
    // Call local exit if defined
    int has_local_exit = 0;
    for (int i = 0; i < ast->child_count; i++) {
        if (ast->children[i]->type == AST_FUNCTION && strcmp(ast->children[i]->text, "exit") == 0) {
            has_local_exit = 1;
            break;
        }
    }
    if (has_local_exit) {
        fprintf(f, "    come_%s__exit_local();\n", current_module);
    }
    // Call imported exits in reverse order
    for (int i = current_import_count - 1; i >= 0; i--) {
        fprintf(f, "    come_%s__exit();\n", current_imports[i]);
        if (!is_system_module_name(current_imports[i])) {
            fprintf(f, "    if (come_%s__ctx) { mem_talloc_free(come_%s__ctx); come_%s__ctx = NULL; }\n",
                    current_imports[i], current_imports[i], current_imports[i]);
        }
    }
    fprintf(f, "}\n");
    // Map type (not in come_types.h as it's a special case)
    // Map type moved to come_map.h

    fprintf(f, "#include <math.h>\n");
    fprintf(f, "#include <stdlib.h>\n");
    fprintf(f, "#ifdef _WIN32\n");
    fprintf(f, "#ifdef __GNUC__\n");
    fprintf(f, "#define COME_UNUSED __attribute__((unused))\n");
    fprintf(f, "#else\n");
    fprintf(f, "#define COME_UNUSED\n");
    fprintf(f, "#endif\n");
    fprintf(f, "static COME_UNUSED unsigned short come_htons(unsigned short x) { return (unsigned short)((x << 8) | (x >> 8)); }\n");
    fprintf(f, "#else\n");
    fprintf(f, "#include <arpa/inet.h>\n");
    fprintf(f, "#define come_htons(x) htons(x)\n");
    fprintf(f, "#endif\n");

    // Runtime Preamble
    fprintf(f, "\n/* Runtime Preamble */\n");

    
    fprintf(f, "#define come_free(p) mem_talloc_free(p)\n");
    fprintf(f, "#define come_net_hton(x) come_htons((unsigned short)(x))\n");
    
    
    
    // Array Resize Helpers
    

    fprintf(f, "/* Runtime Preamble additions */\n");

    
    fprintf(f, "#define come_std_eprintf(...) fprintf(stderr, __VA_ARGS__)\n");

    for (int i = 0; i < tuple_return_count; i++) {
        TupleReturnInfo* tuple = &tuple_returns[i];
        fprintf(f, "typedef struct %s {\n", tuple->struct_name);
        for (int j = 0; j < tuple->elem_count; j++) {
            fprintf(f, "    ");
            emit_c_type(f, tuple->elem_types[j]);
            fprintf(f, " _%d;\n", j);
        }
        fprintf(f, "} %s;\n", tuple->struct_name);
    }

    // Pass -1: Aliases (typedefs)
    if (g_verbose) printf("DEBUG: Starting Pass -1 Aliases\n");
    for (int i = 0; i < ast->child_count; i++) {
        ASTNode* child = ast->children[i];
        if (child->type == AST_TYPE_ALIAS) {
             if (g_verbose) printf("DEBUG: Generating Alias %s\n", child->text);
             
             // Hack: Skip FILE as it causes conflict with stdio.h
             if (strcmp(child->text, "FILE") == 0) {
                 mark_struct_seen("FILE");
                 continue;
             }

             emit_line_directive(f, child);
             if (!is_struct_seen(child->text)) {
                 fprintf(f, "typedef %s %s;\n", child->children[0]->text, child->text);
                 // If it's a struct alias, mark it seen
                 if (strncmp(child->children[0]->text, "struct ", 7) == 0) {
                     mark_struct_seen(child->children[0]->text + 7);
                 }
                 // Also mark the alias name itself as seen if it's the same or similar
                 mark_struct_seen(child->text);
             }
        }
    }

    // Pass 0: Forward decls for Structs
    if (g_verbose) printf("DEBUG: Starting Pass 0: Structs\n");
    for (int i = 0; i < ast->child_count; i++) {
        ASTNode* child = ast->children[i];
        if (child->type == AST_STRUCT_DECL) {
             if (!is_struct_seen(child->text)) {
                 emit_line_directive(f, child);
                 fprintf(f, "typedef struct %s %s;\n", child->text, child->text);
                 mark_struct_seen(child->text);
             }
        }
    }

    // Forward Prototypes
    if (g_verbose) printf("DEBUG: Starting Pass forward prototypes\n");
    for (int i=0; i<ast->child_count; i++) {
        ASTNode* child = ast->children[i];
        if (child->type == AST_FUNCTION) {
             if (strcmp(child->text, "main") == 0) continue; // Skip main prototype
             if (g_verbose) printf("DEBUG: Mapping prototype for %s\n", child->text);
             // But `add` error requires it.
             // Simple loop:
             if (child->child_count > 0 && child->children[0]->type != AST_BLOCK) {
                  ASTNode* ret = child->children[0];
                  
                  char func_name[8192];
                  int is_main = (strcmp(child->text, "main") == 0);
                  char* underscore = strchr(child->text, '_');
                  if (underscore && !is_main && isupper(child->text[0])) {
                      // Struct method: first char is uppercase
                      long prefix_len = underscore - child->text;
                      snprintf(func_name, sizeof(func_name), "come_%s__%.*s__%s", current_module, (int)prefix_len, child->text, underscore + 1);
                  } else if (strcmp(child->text, "init") == 0) {
                      snprintf(func_name, sizeof(func_name), "come_%s__init_local", current_module);
                  } else if (strcmp(child->text, "exit") == 0) {
                      snprintf(func_name, sizeof(func_name), "come_%s__exit_local", current_module);
                  } else {
                      // Regular function
                      snprintf(func_name, sizeof(func_name), "come_%s__%s", current_module, child->text);
                  }


                  TupleReturnInfo* tuple = find_tuple_info_by_source(child->text);
                  if (tuple) {
                       fprintf(f, "%s %s(", tuple->struct_name, func_name);
                  } else {
                       if (strcmp(ret->text, "string") == 0) fprintf(f, "come_string_t* %s(", func_name);
                       else if (strcmp(ret->text, "FILE") == 0) fprintf(f, "come_std__FILE_t* %s(", func_name);
                       else fprintf(f, "%s %s(", c_type_name(ret->text), func_name);
                  }
             } else {
                  // Fallback for void return without explicit type? or AST_FUNCTION without children?
                  // Should check if we have mangled name logic here too just in case
                  char func_name[8192];
                  snprintf(func_name, sizeof(func_name), "come_%s__%s", current_module, child->text);
                  fprintf(f, "void %s(", func_name);
             }
             // Args?
             // Iterate children until AST_BLOCK
             int start_args = 1; // 0 is return
             if (child->child_count > 0 && child->children[0]->type == AST_BLOCK) start_args = 0;
             
             // If nport, inject self?
             if (strcmp(child->text, "nport")==0) {
                 fprintf(f, "struct TCP_ADDR* self"); 
             }
             
             int first = (strcmp(child->text, "nport")==0) ? 0 : 1;
             
             for (int j=start_args; j<child->child_count; j++) {
                 if (child->children[j]->type == AST_BLOCK) break;
                 if (!first) fprintf(f, ", ");
                 ASTNode* arg = child->children[j];
                 if (arg->type == AST_VAR_DECL) {
                     ASTNode* type = arg->children[1];
                     // Array check
                       if (strstr(type->text, "[]")) {
                            char raw[64];
                            strncpy(raw, type->text, strlen(type->text)-2);
                            raw[strlen(type->text)-2] = 0;
                            
                            if (strcmp(raw, "int")==0) fprintf(f, "come_int_array_t*");
                            else if (strcmp(raw, "byte")==0) fprintf(f, "come_byte_array_t*");
                            else if (strcmp(raw, "string")==0) fprintf(f, "come_string_list_t*");
                            else fprintf(f, "come_array_t*");
                       } else {
                            emit_c_type(f, type->text);
                       }
                  } else {
                     fprintf(f, "void*"); // Fallback
                 }
                 first = 0;
             }
             fprintf(f, ");\n");
        } else if (child->type == AST_STRUCT_DECL) {
             for (int j = 0; j < child->child_count; j++) {
                 if (child->children[j]->type == AST_FUNCTION) {
                     emit_function_prototype(f, child->children[j]);
                 }
             }
        }
    }

    if (ast->type == AST_PROGRAM) {
        if (ast->text[0] != 0) {
            strncpy(current_module, ast->text, 255);
        }
        generate_program(f, ast);
    } else {
        generate_node(f, ast, 0);
    }

    fclose(f);
    return codegen_error_count == 0 ? 0 : 1;
}
