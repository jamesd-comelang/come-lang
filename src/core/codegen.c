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

typedef come_map_t map;

/* small helper: write indentation spaces */
static void emit_indent(FILE* f, int indent_spaces) {
    for (int i = 0; i < indent_spaces; i++) fputc(' ', f);
}

// Semantic errors found during generation (COIL-101). Generation
// continues so the user can see every error, but generate_c_from_ast
// returns nonzero when any occurred and the driver refuses the output.
static int g_codegen_errors = 0;

// COIL-108: directory-module group control (see codegen.h).
int g_codegen_suppress_singletons = 0;
int g_codegen_state_home = 1; // COIL-140: this file declares the module variables (the driver clears it for non-home files of a group)
int g_codegen_group_has_init = -1;
int g_codegen_group_has_exit = -1;
const char** g_codegen_chain_imports = NULL;
int g_codegen_chain_import_count = 0;
// COIL-111: system-module generation (see codegen.h).
int g_codegen_system_module = 0;
static void codegen_error(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "Error: %s\n", buf);
    g_codegen_errors++;
}

// Come array types are strings like "int[5]" (fixed) or "int[]"
// (dynamic). A fixed array's length is part of its type.
static int type_is_array(const char* t) {
    return t && strchr(t, '[') != NULL;
}
static int type_is_fixed_array(const char* t) {
    if (!type_is_array(t)) return 0;
    const char* lb = strchr(t, '[');
    return lb[1] >= '0' && lb[1] <= '9';
}
static int type_fixed_size(const char* t) {
    const char* lb = t ? strchr(t, '[') : NULL;
    return lb ? atoi(lb + 1) : 0;
}

// Register a variable's type for later lookups. For a fixed array the
// parser records the size on the type node (array_size); fold it back
// into the type string ("int[5]") so fixed and dynamic arrays stay
// distinguishable - the plain type text is always "int[]" (COIL-101).
static void register_local_type(const char* name, ASTNode* type_node) {
    if (type_node->array_size > 0 && type_is_array(type_node->text)) {
        char eff[300];
        size_t bl = strlen(type_node->text);
        if (bl >= 2 && strcmp(type_node->text + bl - 2, "[]") == 0) bl -= 2;
        snprintf(eff, sizeof(eff), "%.*s[%d]", (int)bl, type_node->text, type_node->array_size);
        add_local_variable(name, eff);
        return;
    }
    add_local_variable(name, type_node->text);
}

// Fixed arrays stay fixed (COIL-101): no promotion to dynamic, no
// conversion between fixed sizes, in either direction.
static void check_array_transfer(const char* verb, const char* dst_name, const char* dst_type,
                                 const char* src_name, const char* src_type) {
    if (!type_is_array(dst_type) || !type_is_array(src_type)) return;
    int df = type_is_fixed_array(dst_type), sf = type_is_fixed_array(src_type);
    if (df != sf || (df && sf && type_fixed_size(dst_type) != type_fixed_size(src_type))) {
        codegen_error("array type mismatch: %s array '%s' (%s) cannot %s %s array '%s' (%s) - "
                      "fixed arrays stay fixed and are never promoted; use matching types or copy element by element",
                      sf ? "fixed" : "dynamic", src_name, src_type, verb,
                      df ? "fixed" : "dynamic", dst_name, dst_type);
    }
}

static void emit_c_string_literal(FILE* f, const char* s) {
    // s includes quotes from lexer?
    // Parser copies text. Lexer includes quotes for TOKEN_STRING_LITERAL?
    // Lexer: "foo" -> text="foo" (with quotes).
    // So just print it.
    fprintf(f, "%s", s);
}

// Track source file for #line directives
static const char* source_filename = NULL;
static int last_emitted_line = -1;
static int g_gen_line_map = 1;

// Track current function return type for correct return statement generation
static char current_function_return_type[128] = "";
static char current_function_name[128] = "";
static int g_in_fn_body = 0;
static int g_fn_instance_form = 0; // COIL-140: body being generated is instance-form
static int g_cur_fn_frameless = 0; // A5: body being generated needs no marker frame
// Design 3 (redesign): a .view()/.byte_view() call
// emits a true view only when this flag is clear.
// Storing positions (declaration initializers,
// assignments, returns, spawn arguments, and call
// arguments) set it, so a view there promotes to a
// copy (substr / byte_range) instead. A view that
// survives to codegen is therefore a temporary
// consumed within its statement, where its parent is
// in scope and therefore alive.
// Design 2 (redesign): the same flag gates arena
// concats - a string `+` in a temporary position
// (flag clear) goes to the bump arena, while one in
// a storing position (flag set) keeps the talloc
// concat. It is, in effect, the "storing position"
// flag for both features. Reset at every statement
// boundary (generate_node entry), so a set flag can
// never leak into an unrelated statement.
static int g_view_promote = 0;
static int g_module_level = 0;     // COIL-140: generating a top-level node   // arming ERR source sites (COIL-139)
static int g_raw_aggregate = 0;    // COIL-145: emit an array literal's raw braces (the declaration path's own C array) instead of materializing the header value
static char current_module[256] = "main"; // Default to main if unspecified
static char* current_imports[256];
static int current_import_count = 0;



// Emit #line directive if needed
static void emit_line_directive(FILE* f, ASTNode* node) {
    if (!g_gen_line_map || !source_filename || !node || node->source_line <= 0) return;

    // Only emit if line changed to avoid clutter
    if (node->source_line != last_emitted_line) {
        fprintf(f, "\n#line %d \"%s\"\n", node->source_line, source_filename);
        last_emitted_line = node->source_line;
    }
}

// COIL-157: a return expands to several C statements
// (temp, adopt, pop, return). Without a reset before
// each one, #line arithmetic sprays them across the
// following source lines and line coverage misattributes
// the return - a taken branch's pop lands on the next
// branch's line. Force each statement back to the
// return's own line.
static void emit_line_directive_forced(FILE* f, ASTNode* node) {
    if (!g_gen_line_map || !source_filename || !node || node->source_line <= 0) return;
    fprintf(f, "\n#line %d \"%s\"\n", node->source_line, source_filename);
    last_emitted_line = node->source_line;
}

// COIL-139: ERR source-site emission. Generated code arms a
// pending location before each statement inside a function
// body; the runtime records it with the next error set.
static void fprint_escaped_cstr(FILE* f, const char* str) {
    if (!str) return;
    for (const char* p = str; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        fputc(*p, f);
    }
}

static void emit_err_arm(FILE* f, ASTNode* node, int indent) {
    emit_indent(f, indent);
    fprintf(f, "come_err_arm(\"%s\", \"%s\", \"", current_module, current_function_name);
    fprint_escaped_cstr(f, source_filename);
    fprintf(f, "\", %d);\n", node->source_line);
}

static void emit_err_site_args(FILE* f, ASTNode* node) {
    fprintf(f, ", \"%s\", \"%s\", \"", current_module, current_function_name);
    fprint_escaped_cstr(f, source_filename);
    fprintf(f, "\", %d", node->source_line);
}

#include "utils.h"
#include <ctype.h>

static void generate_node(FILE* f, ASTNode* node, int indent);
static void generate_expression(FILE* f, ASTNode* node);
static void emit_expr_as(FILE* f, ASTNode* e, const char* ct);
static const char* expr_come_type(ASTNode* e, char* buf, size_t n);

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

// The Come type a first assignment declares its name with
// (COIL-126): the spec's examples assign to fresh names -
// `reply = b.read()` - and section 10.1 locks a name's type
// at its first assignment, like `var`. Returns NULL when the
// value's type cannot be inferred; the caller errors and
// asks for an explicit declaration. A link read infers
// string, the common payload; other payloads are declared
// explicitly (int xs[] = s.read()).
static const char* infer_assign_type(ASTNode* rhs) {
    if (!rhs) return NULL;
    switch (rhs->type) {
        case AST_STRING_LITERAL: return "string";
        case AST_BOOL_LITERAL: return "bool";
        case AST_NUMBER:
            return (strchr(rhs->text, '.') || strchr(rhs->text, 'e') ||
                    strchr(rhs->text, 'E')) ? "double" : "int";
        case AST_IDENTIFIER:
            return get_local_variable_type(rhs->text);
        case AST_AGGREGATE_INIT:
            // A typed struct literal (COIL-136).
            if (strncmp(rhs->text, "struct ", 7) == 0) return rhs->text;
            return NULL;
        case AST_MEMBER_ACCESS: {
            static char ibuf[160];
            return expr_come_type(rhs, ibuf, sizeof(ibuf));
        }
        case AST_METHOD_CALL:
            if ((strcmp(rhs->text, "dup") == 0 || strcmp(rhs->text, "slice") == 0) &&
                rhs->child_count >= 1) {
                // dup/slice keep the receiver's buffer type.
                static char dbuf[160];
                return expr_come_type(rhs->children[0], dbuf, sizeof(dbuf));
            }
            if (strcmp(rhs->text, "read") == 0 && rhs->child_count >= 1 &&
                rhs->children[0]->type == AST_IDENTIFIER) {
                const char* rt = get_local_variable_type(rhs->children[0]->text);
                if (rt && strcmp(rt, "link") == 0) return "string";
            }
            return NULL;
        case AST_CALL:
            if (strcmp(rhs->text, "wait") == 0) return "link";
            return NULL;
        default:
            return NULL;
    }
}

// Emit `name = rhs` as a declaration of the inferred type.
static void emit_inferred_decl(FILE* f, const char* type, const char* name, ASTNode* rhs) {
    if (strcmp(type, "string") == 0) {
        fprintf(f, "come_string_t %s = ", name);
        if (rhs->type == AST_STRING_LITERAL) {
            fprintf(f, "come_string_new(COME_CTX, ");
            generate_expression(f, rhs);
            fprintf(f, ")");
        } else {
            emit_expr_as(f, rhs, "come_string_t");
        }
        fprintf(f, ";\n");
    } else if (strcmp(type, "link") == 0) {
        fprintf(f, "come_link_t* %s = ", name);
        generate_expression(f, rhs);
        fprintf(f, ";\n");
    } else if (strcmp(type, "bool") == 0 || strcmp(type, "int") == 0 ||
               strcmp(type, "double") == 0 || strcmp(type, "float") == 0 ||
               strcmp(type, "long") == 0) {
        fprintf(f, "%s %s = ", type, name);
        generate_expression(f, rhs);
        fprintf(f, ";\n");
    } else if (strncmp(type, "struct ", 7) == 0) {
        // A struct value (COIL-136): the initializer emits
        // as a compound literal or an aggregate.
        fprintf(f, "%s %s = ", type, name);
        generate_expression(f, rhs);
        fprintf(f, ";\n");
    } else {
        codegen_error("cannot infer a declaration for type '%s'; declare the variable explicitly", type);
    }
}


// ---- Typed maps: map{K, V} (COIL-102) ----

// Split "map{K, V}" into K and V (nested-map aware). Returns 1 on success.
static int map_type_parts(const char* t, char* k, size_t ksz, char* v, size_t vsz) {
    if (!t || strncmp(t, "map{", 4) != 0) return 0;
    const char* s = t + 4;
    size_t len = strlen(s);
    if (len == 0 || s[len - 1] != '}') return 0;
    int depth = 0;
    const char* comma = NULL;
    for (const char* p = s; p < s + len - 1; p++) {
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
        else if (*p == ',' && depth == 0) { comma = p; break; }
    }
    if (!comma) return 0;
    size_t klen = (size_t)(comma - s);
    while (klen > 0 && s[klen - 1] == ' ') klen--;
    if (klen >= ksz) klen = ksz - 1;
    memcpy(k, s, klen);
    k[klen] = 0;
    const char* vs = comma + 1;
    while (*vs == ' ') vs++;
    size_t vlen = (size_t)((s + len - 1) - vs);
    while (vlen > 0 && vs[vlen - 1] == ' ') vlen--;
    if (vlen >= vsz) vlen = vsz - 1;
    memcpy(v, vs, vlen);
    v[vlen] = 0;
    return 1;
}

// The combinations the runtime supports today: string keys (the
// runtime hashes key contents) with int or string values.
static int map_combo_supported(const char* k, const char* v) {
    return strcmp(k, "string") == 0 && (strcmp(v, "int") == 0 || strcmp(v, "string") == 0);
}

static void map_combo_error(const char* k, const char* v) {
    codegen_error("map{%s, %s} is not supported yet: v1 maps currently use string keys with int or string values", k, v);
}

// Coarse static type of an expression for map checks: "string", "int",
// an identifier's declared type, or "" when not statically known
// (unchecked - method calls, subexpressions, ...).
static void expr_coarse_type(ASTNode* e, char* out, size_t n);

// ---- COIL-107: no implicit narrowing (spec 6.8) ----
// The numeric family with kind (0 signed, 1 unsigned, 2 float) and
// width. wchar is int32_t (come_types.h); byte is int8_t.
typedef struct { const char* name; int kind; int bits; } NumInfo;
static const NumInfo NUM_TYPES[] = {
    {"byte", 0, 8}, {"ubyte", 1, 8}, {"short", 0, 16}, {"ushort", 1, 16},
    {"int", 0, 32}, {"wchar", 0, 32}, {"uint", 1, 32},
    {"long", 0, 64}, {"ulong", 1, 64}, {"float", 2, 32}, {"double", 2, 64},
};
static const NumInfo* num_info(const char* name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(NUM_TYPES) / sizeof(NUM_TYPES[0]); i++)
        if (strcmp(NUM_TYPES[i].name, name) == 0) return &NUM_TYPES[i];
    return NULL;
}
// Can every value of `from` be represented exactly in `to`?
static int conversion_is_lossless(const NumInfo* from, const NumInfo* to) {
    if (from->kind == 2 && to->kind == 2) return to->bits >= from->bits;
    if (from->kind == 2) return 0; // float -> integer can drop a fraction
    if (to->kind == 2) {
        int mantissa = (to->bits == 32) ? 24 : 53;
        int valbits = (from->kind == 0) ? from->bits - 1 : from->bits;
        return valbits <= mantissa;
    }
    if (to->kind == 0) // to signed
        return from->kind == 0 ? to->bits >= from->bits : to->bits > from->bits;
    // to unsigned: the source must be unsigned and fit
    return from->kind == 1 && to->bits >= from->bits;
}
// Does this literal's value fit the target type with nothing lost?
static int literal_fits(const char* text, const NumInfo* to) {
    int is_hex = text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    int is_float = 0;
    if (!is_hex)
        for (const char* p = text; *p; p++)
            if (*p == '.' || *p == 'e' || *p == 'E') { is_float = 1; break; }
    if (is_float) {
        if (to->kind == 2) return 1; // float literal -> float/double
        double d = strtod(text, NULL);
        if (d != (double)(long long)d) return 0; // a fraction would be lost
        char buf[40];
        snprintf(buf, sizeof(buf), "%lld", (long long)d);
        return literal_fits(buf, to); // range-check the integral value
    }
    if (to->kind == 2) {
        // integer literal -> float/double: an exact round-trip is required
        long long v = strtoll(text, NULL, 0);
        if (to->bits == 32) return (long long)(float)v == v;
        return v >= -9007199254740992LL && v <= 9007199254740992LL &&
               (long long)(double)v == v;
    }
    if (to->kind == 1) {
        if (text[0] == '-') return 0; // a negative never fits unsigned
        unsigned long long v = strtoull(text, NULL, 0);
        if (to->bits == 8) return v <= 255ULL;
        if (to->bits == 16) return v <= 65535ULL;
        if (to->bits == 32) return v <= 4294967295ULL;
        return 1;
    }
    long long v = strtoll(text, NULL, 0);
    if (to->bits == 8) return v >= -128 && v <= 127;
    if (to->bits == 16) return v >= -32768 && v <= 32767;
    if (to->bits == 32) return v >= -2147483647LL - 1 && v <= 2147483647LL;
    return 1;
}
// Decode a character literal's text ('A', '\n', a UTF-8 character)
// to its code point, or -1 when the form is not recognized.
static long decode_char_literal(const char* text) {
    if (!text || text[0] != '\'') return -1;
    const unsigned char* p = (const unsigned char*)text + 1;
    if (*p == '\\') {
        switch (p[1]) {
            case 'n': return '\n';
            case 't': return '\t';
            case 'r': return '\r';
            case '0': return 0;
            case '\\': return '\\';
            case '\'': return '\'';
            default: return -1;
        }
    }
    if (*p < 0x80) return *p;
    // UTF-8 multibyte
    long cp;
    int extra;
    if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; extra = 1; }
    else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; extra = 2; }
    else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; extra = 3; }
    else return -1;
    for (int i = 0; i < extra; i++) {
        p++;
        if ((*p & 0xC0) != 0x80) return -1;
        cp = (cp << 6) | (*p & 0x3F);
    }
    return cp;
}
// Reject a store (declaration init or assignment) that would narrow
// implicitly. Explicit casts (AST_CAST) are the sanctioned path and
// pass; expressions of unknown static type are not checked.
static void check_numeric_store(const char* target_type, ASTNode* expr,
                                const char* target_name) {
    const NumInfo* to = num_info(target_type);
    if (!to || !expr) return;
    if (expr->type == AST_CAST) return;
    // A literal, possibly negated: allowed iff its value fits.
    ASTNode* lit = expr;
    int neg = 0;
    if (lit->type == AST_UNARY_OP && strcmp(lit->text, "-") == 0 &&
        lit->child_count == 1) { neg = 1; lit = lit->children[0]; }
    if (lit->type == AST_NUMBER && lit->text[0] == '\'') {
        // A character literal is a wchar value with a known code
        // point: it is allowed iff that value fits the target.
        long cp = decode_char_literal(lit->text);
        if (cp >= 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%ld", cp);
            if (!literal_fits(buf, to))
                codegen_error("character literal %s does not fit in %s '%s' without loss: use an explicit cast if intended",
                              lit->text, target_type, target_name);
            return;
        }
        // Unrecognized form: fall through to the type check.
    }
    if (lit->type == AST_NUMBER && lit->text[0] != '\'') {
        char buf[80];
        snprintf(buf, sizeof(buf), "%s%.60s", neg ? "-" : "", lit->text);
        if (!literal_fits(buf, to))
            codegen_error("numeric literal %s does not fit in %s '%s' without loss: use an explicit cast if intended",
                          buf, target_type, target_name);
        return;
    }
    char from[300];
    expr_coarse_type(expr, from, sizeof(from));
    const NumInfo* fi = num_info(from);
    if (!fi) return; // unknown or non-numeric: not checked
    if (!conversion_is_lossless(fi, to))
        codegen_error("implicit narrowing: cannot store %s in %s '%s' - use an explicit cast %s(value)",
                      from, target_type, target_name, target_type);
}
// ---- COIL-109: functions as arguments (spec 8.3) ----
// A table of the current module's functions with their signatures
// in the parser's fn: form ("fn:RET(T1,T2)"), built once per file.
// Struct methods (Rect.area style) are excluded: they need a
// receiver and cannot be passed as bare names.
typedef struct { char name[128]; char sig[600]; int is_extern; } FnSig;
static FnSig g_module_fns[256];
static int g_module_fn_count = 0;
static int is_extern_fn(const char* name) {
    for (int i = 0; i < g_module_fn_count; i++)
        if (g_module_fns[i].is_extern && strcmp(g_module_fns[i].name, name) == 0) return 1;
    return 0;
}

static void build_fn_sig(ASTNode* fn, char* out, size_t n) {
    const char* ret = "void";
    if (fn->child_count > 0 && fn->children[0] && fn->children[0]->text[0] != '(')
        ret = fn->children[0]->text;
    char params[512] = "";
    int first = 1;
    for (int i = 1; i < fn->child_count; i++) {
        ASTNode* p = fn->children[i];
        if (!p || p->type == AST_BLOCK) break;
        if (p->type != AST_VAR_DECL || p->child_count < 2) continue;
        const char* pt = p->children[1]->text;
        char eff[300];
        if (p->children[1]->array_size > 0) {
            size_t bl = strlen(pt);
            if (bl >= 2 && strcmp(pt + bl - 2, "[]") == 0) bl -= 2;
            snprintf(eff, sizeof(eff), "%.*s[%d]", (int)bl, pt, p->children[1]->array_size);
            pt = eff;
        }
        if (!first) strncat(params, ",", sizeof(params) - strlen(params) - 1);
        strncat(params, pt, sizeof(params) - strlen(params) - 1);
        first = 0;
    }
    snprintf(out, n, "fn:%s(%s)", ret, params);
}
static const char* find_module_fn_sig(const char* name) {
    for (int i = 0; i < g_module_fn_count; i++)
        if (strcmp(g_module_fns[i].name, name) == 0) return g_module_fns[i].sig;
    return NULL;
}

// Module export registry (spec 3.4, COIL-127): see codegen.h.
typedef struct {
    char module[128];
    char names[COME_MAX_EXPORTS][128];
    int count;
} ModuleExports;
static ModuleExports g_exports[64];
static int g_exports_count = 0;

int codegen_register_module_exports(const char* module, const char** names, int count) {
    ModuleExports* e = NULL;
    for (int i = 0; i < g_exports_count; i++) {
        if (strcmp(g_exports[i].module, module) == 0) { e = &g_exports[i]; break; }
    }
    if (!e) {
        if (g_exports_count >= 64) return -1;
        e = &g_exports[g_exports_count++];
        snprintf(e->module, sizeof(e->module), "%s", module);
        e->count = 0;
    }
    int overflow = 0;
    for (int i = 0; i < count; i++) {
        int dup = 0;
        for (int j = 0; j < e->count; j++) {
            if (strcmp(e->names[j], names[i]) == 0) dup = 1;
        }
        if (dup) continue;
        if (e->count >= COME_MAX_EXPORTS) { overflow = 1; continue; }
        snprintf(e->names[e->count++], sizeof(e->names[0]), "%s", names[i]);
    }
    return overflow ? -1 : 0;
}

// 1 when `symbol` may be referenced from outside `module`:
// always, unless the module declared an export list that
// does not contain it.
static int module_symbol_visible(const char* module, const char* symbol) {
    for (int i = 0; i < g_exports_count; i++) {
        if (strcmp(g_exports[i].module, module) != 0) continue;
        for (int j = 0; j < g_exports[i].count; j++) {
            if (strcmp(g_exports[i].names[j], symbol) == 0) return 1;
        }
        return 0;
    }
    return 1;
}

// Struct declarations in the file (COIL-127): the universal
// methods on a struct value (spec 7.4) need the field count.
static char g_struct_names[64][128];
static int g_struct_fields[64];
static int g_struct_count = 0;

// Field types per struct (COIL-135): indexing a struct field
// array needs the field's element type.
static char g_sfield_struct[512][128];
static char g_sfield_name[512][128];
static char g_sfield_type[512][160];
static int g_sfield_count = 0;

static const char* struct_field_type(const char* sname, const char* fname) {
    for (int i = 0; i < g_sfield_count; i++) {
        if (strcmp(g_sfield_struct[i], sname) == 0 && strcmp(g_sfield_name[i], fname) == 0)
            return g_sfield_type[i];
    }
    return NULL;
}

static void collect_structs(ASTNode* ast) {
    g_struct_count = 0;
    g_sfield_count = 0;
    if (!ast || ast->type != AST_PROGRAM) return;
    for (int i = 0; i < ast->child_count && g_struct_count < 64; i++) {
        ASTNode* ch = ast->children[i];
        if (ch && ch->type == AST_STRUCT_DECL) {
            int fields = 0;
            for (int j = 0; j < ch->child_count; j++) {
                if (ch->children[j]->type == AST_VAR_DECL) fields++;
            }
            snprintf(g_struct_names[g_struct_count], 128, "%.*s", 127, ch->text);
            g_struct_fields[g_struct_count] = fields;
            for (int j = 0; j < ch->child_count && g_sfield_count < 512; j++) {
                ASTNode* fd = ch->children[j];
                if (fd && fd->type == AST_VAR_DECL && fd->child_count >= 2 && fd->children[1]) {
                    snprintf(g_sfield_struct[g_sfield_count], 128, "%.*s", 127, ch->text);
                    snprintf(g_sfield_name[g_sfield_count], 128, "%.*s", 127, fd->text);
                    snprintf(g_sfield_type[g_sfield_count], 160, "%.*s", 159, fd->children[1]->text);
                    g_sfield_count++;
                }
            }
            g_struct_count++;
        }
    }
}

// ---- COIL-136: struct references ----
// A reference type is spelled &(struct Name): a non-owning
// pointer to a struct value, declared on parameters and local
// variables only. &x takes a reference at a call site; member
// access uses . through values and references alike.
static int is_ref_type(const char* t) {
    if (!t) return 0;
    size_t l = strlen(t);
    return l >= 4 && strncmp(t, "&(", 2) == 0 && t[l - 1] == ')';
}
static void ref_inner_type(const char* t, char* out, size_t n) {
    if (!is_ref_type(t)) { snprintf(out, n, "%s", t ? t : ""); return; }
    snprintf(out, n, "%.*s", (int)strlen(t) - 3, t + 2);
}
// The type a value question should see: a reference answers
// as the struct it refers to.
static const char* deref_ref_type(const char* t, char* buf, size_t n) {
    if (!t) return NULL;
    if (is_ref_type(t)) { ref_inner_type(t, buf, n); return buf; }
    return t;
}

static int struct_field_count(const char* name) {
    for (int i = 0; i < g_struct_count; i++) {
        if (strcmp(g_struct_names[i], name) == 0) return g_struct_fields[i];
    }
    return 0;
}

// The struct name when an expression is a struct-typed local,
// else NULL.
static const char* struct_receiver_name(ASTNode* receiver) {
    if (!receiver || receiver->type != AST_IDENTIFIER) return NULL;
    char rbuf[300];
    const char* rt = deref_ref_type(get_local_variable_type(receiver->text),
                                    rbuf, sizeof(rbuf));
    if (rt && strncmp(rt, "struct ", 7) == 0 && struct_field_count(rt + 7) >= 0 &&
        rt[7] != 0) {
        for (int i = 0; i < g_struct_count; i++) {
            // Return the table entry, not rt + 7: rt may
            // point into the caller's stack buffer (ref
            // types), so returning it would hand back a
            // dead stack address. The strings are equal
            // by the strcmp above, so this is identical.
            if (strcmp(g_struct_names[i], rt + 7) == 0) return g_struct_names[i];
        }
    }
    return NULL;
}
static void collect_module_fns(ASTNode* ast) {
    g_module_fn_count = 0;
    if (!ast || ast->type != AST_PROGRAM) return;
    for (int i = 0; i < ast->child_count && g_module_fn_count < 256; i++) {
        ASTNode* ch = ast->children[i];
        if (ch && ch->type == AST_FUNCTION) {
            if (isupper((unsigned char)ch->text[0]) && strchr(ch->text, '_'))
                continue; // struct method (Rect_area): needs a receiver
            FnSig* e = &g_module_fns[g_module_fn_count++];
            snprintf(e->name, sizeof(e->name), "%.*s", (int)sizeof(e->name) - 1, ch->text);
            e->is_extern = ch->is_extern;
            build_fn_sig(ch, e->sig, sizeof(e->sig));
        }
    }
}
// Coroutines declared in the file being generated (COIL-120):
// spawn sites look the declaration up here for its parameters.
static ASTNode* g_coroutines[64];
static int g_coroutine_count = 0;

static void collect_coroutines(ASTNode* ast) {
    g_coroutine_count = 0;
    if (!ast || ast->type != AST_PROGRAM) return;
    for (int i = 0; i < ast->child_count && g_coroutine_count < 64; i++) {
        ASTNode* ch = ast->children[i];
        if (ch && ch->type == AST_FUNCTION && ch->is_coroutine) {
            g_coroutines[g_coroutine_count++] = ch;
        }
    }
}

static ASTNode* find_coroutine(const char* name) {
    for (int i = 0; i < g_coroutine_count; i++) {
        if (strcmp(g_coroutines[i]->text, name) == 0) return g_coroutines[i];
    }
    return NULL;
}

// Module-level variables of the file being generated
// (COIL-122): coroutines may not touch them (spec 9.1).
static char g_module_vars[128][128];
static int g_module_var_count = 0;

static void collect_module_vars(ASTNode* ast) {
    g_module_var_count = 0;
    if (!ast || ast->type != AST_PROGRAM) return;
    for (int i = 0; i < ast->child_count && g_module_var_count < 128; i++) {
        ASTNode* ch = ast->children[i];
        if (ch && ch->type == AST_VAR_DECL) {
            snprintf(g_module_vars[g_module_var_count++], 128, "%.*s", 127, ch->text);
        }
    }
}

static int is_module_var(const char* name) {
    for (int i = 0; i < g_module_var_count; i++) {
        if (strcmp(g_module_vars[i], name) == 0) return 1;
    }
    // The driver's registry also knows the module's
    // variables declared in another file of the group
    // (COIL-140).
    return codegen_module_var_index(current_module, name, NULL) >= 0;
}

// Module-level constants and enum names of the file being
// generated (COIL-129): in expressions they are plain
// identifiers, and assigning to one is an error. Collected
// in the same pre-pass style as the module variables.
static char g_const_names[256][128];
static int g_const_name_count = 0;

static void collect_const_names(ASTNode* ast) {
    g_const_name_count = 0;
    if (!ast || ast->type != AST_PROGRAM) return;
    for (int i = 0; i < ast->child_count; i++) {
        ASTNode* ch = ast->children[i];
        if (!ch) continue;
        if (ch->type == AST_CONST_DECL && g_const_name_count < 256) {
            snprintf(g_const_names[g_const_name_count++], 128, "%.*s", 127, ch->text);
        } else if (ch->type == AST_CONST_GROUP) {
            for (int j = 0; j < ch->child_count && g_const_name_count < 256; j++) {
                if (ch->children[j] && ch->children[j]->text[0])
                    snprintf(g_const_names[g_const_name_count++], 128, "%.*s", 127, ch->children[j]->text);
            }
        }
    }
}

static int is_const_name(const char* name) {
    for (int i = 0; i < g_const_name_count; i++) {
        if (strcmp(g_const_names[i], name) == 0) return 1;
    }
    return 0;
}

// Constants declared inside the function being generated:
// registered as their declarations are generated (source
// order), reset per function.
static char g_fn_const_names[128][128];
static int g_fn_const_count = 0;

static int is_fn_const_name(const char* name) {
    for (int i = 0; i < g_fn_const_count; i++) {
        if (strcmp(g_fn_const_names[i], name) == 0) return 1;
    }
    return 0;
}

// Symbols of a directory module's OTHER files (COIL-129):
// the driver registers every function, global, and
// constant name of the group before generating any file,
// so a cross-file reference is not mistaken for an
// undefined name. Keyed by module; a fresh registration
// replaces the old one.
typedef struct { char module[128]; char names[512][128]; int count; } GroupSymbols;
static GroupSymbols g_group_symbols[16];
static int g_group_symbol_modules = 0;

void codegen_register_group_symbols(const char* module, const char** names, int count) {
    GroupSymbols* slot = NULL;
    for (int i = 0; i < g_group_symbol_modules; i++) {
        if (strcmp(g_group_symbols[i].module, module) == 0) slot = &g_group_symbols[i];
    }
    if (!slot) {
        if (g_group_symbol_modules >= 16) return;
        slot = &g_group_symbols[g_group_symbol_modules++];
        snprintf(slot->module, sizeof(slot->module), "%s", module);
    }
    slot->count = 0;
    for (int i = 0; i < count && slot->count < 512; i++)
        snprintf(slot->names[slot->count++], 128, "%s", names[i]);
}


// === Module state registry (COIL-140) ===
// Filled by the driver at parse time for every user module:
// the module-level variables (name + Come type) and the set
// of stateful functions with a witness chain each ("bump ->
// count", "tick -> bump -> count"). Code generated for a
// module WITH variables splits into an instance surface
// (stateful functions, bound to an instance) and a namespace
// surface (the rest).
#define MAX_MODULE_STATES 32
typedef struct {
    char module[64];
    char var_names[64][64];
    char var_types[64][160];
    int var_count;
    char stateful[192][64];
    char chains[192][320];
    int stateful_count;
    int has_main;
    int can_instance;
    char fn_names[192][64];
    char fn_rets[192][64];
    char fn_params[192][512];
    int fn_count;
} ModuleState;
static ModuleState g_module_states[MAX_MODULE_STATES];
static int g_module_state_count = 0;

static int cur_module_has_state(void);

void codegen_register_module_state(const char* module,
                                   const char** var_names,
                                   const char** var_types, int var_count,
                                   const char** stateful,
                                   const char** chains, int stateful_count,
                                   int has_main, int can_instance,
                                   const char** fn_names, const char** fn_rets,
                                   const char** fn_params, int fn_count) {
    ModuleState* slot = NULL;
    for (int i = 0; i < g_module_state_count; i++)
        if (strcmp(g_module_states[i].module, module) == 0) slot = &g_module_states[i];
    if (!slot) {
        if (g_module_state_count >= MAX_MODULE_STATES) return;
        slot = &g_module_states[g_module_state_count++];
        memset(slot, 0, sizeof(*slot));
        snprintf(slot->module, sizeof(slot->module), "%s", module);
    }
    slot->var_count = 0;
    for (int i = 0; i < var_count && slot->var_count < 64; i++) {
        snprintf(slot->var_names[slot->var_count], 64, "%s", var_names[i]);
        snprintf(slot->var_types[slot->var_count], 160, "%s", var_types[i]);
        slot->var_count++;
    }
    slot->has_main = has_main;
    slot->can_instance = can_instance;
    slot->fn_count = 0;
    for (int i = 0; i < fn_count && slot->fn_count < 192; i++) {
        snprintf(slot->fn_names[slot->fn_count], 64, "%s", fn_names[i]);
        snprintf(slot->fn_rets[slot->fn_count], 64, "%s", fn_rets[i]);
        snprintf(slot->fn_params[slot->fn_count], 512, "%s", fn_params[i]);
        slot->fn_count++;
    }
    slot->stateful_count = 0;
    for (int i = 0; i < stateful_count && slot->stateful_count < 192; i++) {
        snprintf(slot->stateful[slot->stateful_count], 64, "%s", stateful[i]);
        snprintf(slot->chains[slot->stateful_count], 320, "%s", chains[i]);
        slot->stateful_count++;
    }
}

static ModuleState* module_state_of(const char* module) {
    for (int i = 0; i < g_module_state_count; i++)
        if (strcmp(g_module_states[i].module, module) == 0) return &g_module_states[i];
    return NULL;
}

int codegen_module_has_state(const char* module) {
    ModuleState* ms = module_state_of(module);
    // A module that defines main() is a program: its state
    // is the program's own singleton, not an instance
    // surface (COIL-140).
    return ms && ms->var_count > 0 && !ms->has_main;
}

// Instantiability is the lifecycle pair (COIL-156): a
// module that defines both create and destroy hooks can
// be instantiated; any other module is namespace-only.
// The driver enforces the pair, so can_instance is never
// a half-pair. A module defining main() is a program -
// never an instance surface, pair or not.
int codegen_module_can_instance(const char* module) {
    ModuleState* ms = module_state_of(module);
    return ms && ms->can_instance && !ms->has_main;
}

static int cur_module_can_instance(void) {
    return codegen_module_can_instance(current_module);
}

// Per-instance state exists when a module can be
// instantiated AND has variables to hold per instance:
// stateful functions and hooks then take the instance
// form, and the hooks run per instance rather than in
// the program-level chain.
static int cur_module_per_instance(void) {
    return cur_module_can_instance() && cur_module_has_state();
}

int codegen_fn_stateful(const char* module, const char* fn, const char** chain_out) {
    ModuleState* ms = module_state_of(module);
    if (!ms) return 0;
    for (int i = 0; i < ms->stateful_count; i++)
        if (strcmp(ms->stateful[i], fn) == 0) {
            if (chain_out) *chain_out = ms->chains[i];
            return 1;
        }
    return 0;
}

// === Module struct registry (COIL-146) ===
// Each user module's struct definitions, registered by the
// driver at parse time alongside the state registry: an
// importer cannot use a struct another module returns or
// takes until its own translation unit sees the definition
// (the types half of spec 3.4). Field lists are serialized
// as "name\x1ftype" pairs joined by ';'.
#define MAX_MODULE_STRUCTS 32
typedef struct {
    char module[64];
    char names[64][128];
    char defs[64][2048];
    int packeds[64];
    int aligns[64];
    int count;
} ModuleStructs;
static ModuleStructs g_module_structs[MAX_MODULE_STRUCTS];
static int g_module_struct_modules = 0;

void codegen_register_module_structs(const char* module,
                                     const char** names, const char** defs,
                                     const int* packeds, const int* aligns,
                                     int count) {
    ModuleStructs* slot = NULL;
    for (int i = 0; i < g_module_struct_modules; i++)
        if (strcmp(g_module_structs[i].module, module) == 0) slot = &g_module_structs[i];
    if (!slot) {
        if (g_module_struct_modules >= MAX_MODULE_STRUCTS) return;
        slot = &g_module_structs[g_module_struct_modules++];
        memset(slot, 0, sizeof(*slot));
        snprintf(slot->module, sizeof(slot->module), "%s", module);
    }
    slot->count = 0;
    for (int i = 0; i < count && slot->count < 64; i++) {
        snprintf(slot->names[slot->count], sizeof(slot->names[0]), "%s", names[i]);
        snprintf(slot->defs[slot->count], sizeof(slot->defs[0]), "%s", defs[i]);
        slot->packeds[slot->count] = packeds[i];
        slot->aligns[slot->count] = aligns[i];
        slot->count++;
    }
}

static ModuleStructs* module_structs_of(const char* module) {
    for (int i = 0; i < g_module_struct_modules; i++)
        if (strcmp(g_module_structs[i].module, module) == 0) return &g_module_structs[i];
    return NULL;
}

int codegen_module_struct_count(const char* module) {
    ModuleStructs* ms = module_structs_of(module);
    return ms ? ms->count : 0;
}

// === Module const registry (COIL-148) ===
#define MAX_MODULE_CONSTS 32
typedef struct {
    char module[64];
    char names[256][128];
    char ctypes[256][32];
    int count;
} ModuleConsts;
static ModuleConsts g_module_consts[MAX_MODULE_CONSTS];
static int g_module_const_modules = 0;

void codegen_register_module_consts(const char* module,
                                    const char** names, const char** ctypes,
                                    int count) {
    ModuleConsts* slot = NULL;
    for (int i = 0; i < g_module_const_modules; i++)
        if (strcmp(g_module_consts[i].module, module) == 0) slot = &g_module_consts[i];
    if (!slot) {
        if (g_module_const_modules >= MAX_MODULE_CONSTS) return;
        slot = &g_module_consts[g_module_const_modules++];
        memset(slot, 0, sizeof(*slot));
        snprintf(slot->module, sizeof(slot->module), "%s", module);
    }
    slot->count = 0;
    for (int i = 0; i < count && slot->count < 256; i++) {
        snprintf(slot->names[slot->count], sizeof(slot->names[0]), "%s", names[i]);
        snprintf(slot->ctypes[slot->count], sizeof(slot->ctypes[0]), "%s", ctypes[i]);
        slot->count++;
    }
}

int codegen_module_const_count(const char* module) {
    for (int i = 0; i < g_module_const_modules; i++)
        if (strcmp(g_module_consts[i].module, module) == 0) return g_module_consts[i].count;
    return 0;
}

int codegen_module_const_at(const char* module, int i, const char** name,
                            const char** ctype) {
    for (int m = 0; m < g_module_const_modules; m++) {
        if (strcmp(g_module_consts[m].module, module) != 0) continue;
        if (i < 0 || i >= g_module_consts[m].count) return 0;
        *name = g_module_consts[m].names[i];
        *ctype = g_module_consts[m].ctypes[i];
        return 1;
    }
    return 0;
}

// The C type of a module-level value constant, or NULL
// when the module declares no constant with that name.
const char* codegen_module_const_type(const char* module, const char* name) {
    for (int i = 0; i < g_module_const_modules; i++) {
        if (strcmp(g_module_consts[i].module, module) != 0) continue;
        for (int j = 0; j < g_module_consts[i].count; j++)
            if (strcmp(g_module_consts[i].names[j], name) == 0)
                return g_module_consts[i].ctypes[j];
        return NULL;
    }
    return NULL;
}

// ---- A5 (perf-experiments): frameless functions ----
// A function is frameless when its body provably allocates
// nothing and touches no scope-frame machinery: no buffer
// values created, no calls except to other frameless
// functions of the same module (plus the non-allocating
// .length()/.bytesize()/.owner() readers), no break or
// continue (they pop to a lexical depth), no string or
// aggregate literals, no buffer-typed locals, and string
// concatenation only on provably scalar operands. The
// driver computes the set per module as a fixpoint over
// the call graph before generating any file, and registers
// the names here; emission then skips the marker push at
// entry and the pop-to-marker on every exit path.
// Soundness: a frameless function's values, if any reach
// it through a callee's return, are adopted to the
// caller's live frame instead of a frame of its own - a
// lifetime extension, never a shortening, because every
// frame live during the call outlives the call. Returns
// adopt to come_scope_caller_ctx(), which for a frameless
// function is the caller's caller scope: again an outward
// (safe) move, and crash-safe when no marker exists at
// all (it returns the root). Loop and block frames inside
// a frameless function are still emitted; only the
// function marker is skipped.
#define FRAME_MAX_FNS 256
#define FRAME_MAX_VARS 512

typedef struct { char module[128]; char names[FRAME_MAX_FNS][128]; int count; } FramelessModule;
static FramelessModule g_frameless[64];
static int g_frameless_count = 0;

void codegen_register_module_frameless(const char* module, const char** names, int count) {
    FramelessModule* slot = NULL;
    for (int i = 0; i < g_frameless_count; i++)
        if (strcmp(g_frameless[i].module, module) == 0) slot = &g_frameless[i];
    if (!slot) {
        if (g_frameless_count >= 64) return;
        slot = &g_frameless[g_frameless_count++];
        memset(slot, 0, sizeof(*slot));
        snprintf(slot->module, sizeof(slot->module), "%s", module);
    }
    slot->count = 0;
    for (int i = 0; i < count && slot->count < FRAME_MAX_FNS; i++) {
        snprintf(slot->names[slot->count], sizeof(slot->names[0]), "%s", names[i]);
        slot->count++;
    }
}

int codegen_is_frameless(const char* module, const char* fn) {
    for (int i = 0; i < g_frameless_count; i++) {
        if (strcmp(g_frameless[i].module, module) != 0) continue;
        for (int j = 0; j < g_frameless[i].count; j++)
            if (strcmp(g_frameless[i].names[j], fn) == 0) return 1;
        return 0;
    }
    return 0;
}

typedef struct { char name[128]; ASTNode* node; char ret[128]; } FrameFn;
static FrameFn g_frame_fns[FRAME_MAX_FNS];
static int g_frame_fn_count = 0;
static int g_frame_frameless[FRAME_MAX_FNS];
static char g_frame_module[128];

typedef struct { char name[128]; char type[128]; } FrameVar;
static FrameVar g_frame_vars[FRAME_MAX_VARS];
static int g_frame_var_count = 0;

static int frame_scalar_type(const char* t) {
    return t && (strcmp(t, "int") == 0 || strcmp(t, "bool") == 0 ||
                 strcmp(t, "byte") == 0 || strcmp(t, "long") == 0 ||
                 strcmp(t, "float") == 0 || strcmp(t, "double") == 0);
}

static void frame_var_set(const char* name, const char* type) {
    if (g_frame_var_count >= FRAME_MAX_VARS) return;
    snprintf(g_frame_vars[g_frame_var_count].name, sizeof(g_frame_vars[0].name), "%s", name);
    snprintf(g_frame_vars[g_frame_var_count].type, sizeof(g_frame_vars[0].type), "%s", type);
    g_frame_var_count++;
}

static const char* frame_var_get(const char* name) {
    for (int i = g_frame_var_count - 1; i >= 0; i--)
        if (strcmp(g_frame_vars[i].name, name) == 0) return g_frame_vars[i].type;
    return NULL;
}

static int frame_fn_index(const char* name) {
    for (int i = 0; i < g_frame_fn_count; i++)
        if (strcmp(g_frame_fns[i].name, name) == 0) return i;
    return -1;
}

// Whether an expression used as a `+` operand is provably
// scalar, so the `+` cannot be a string concatenation
// (the one allocating binary operator). Anything unknown
// answers 0: the function is then not frameless.
static int frame_operand_scalar(ASTNode* n) {
    if (!n) return 0;
    switch (n->type) {
        case AST_NUMBER: case AST_BOOL_LITERAL: return 1;
        case AST_IDENTIFIER: return frame_scalar_type(frame_var_get(n->text));
        case AST_ARRAY_ACCESS: {
            if (n->child_count < 1 || !n->children[0] ||
                n->children[0]->type != AST_IDENTIFIER) return 0;
            const char* t = frame_var_get(n->children[0]->text);
            if (!t) return 0;
            if (strcmp(t, "string") == 0) return 1; // s[i] is wchar
            size_t L = strlen(t);
            if (L > 2 && strcmp(t + L - 2, "[]") == 0) {
                char base[128];
                snprintf(base, sizeof(base), "%.*s", (int)(L - 2), t);
                return frame_scalar_type(base);
            }
            return 0;
        }
        case AST_CAST:
            return n->child_count >= 1 && n->children[0] &&
                   frame_scalar_type(n->children[0]->text);
        case AST_METHOD_CALL:
            return strcmp(n->text, "length") == 0 || strcmp(n->text, "bytesize") == 0;
        case AST_CALL: {
            int idx = frame_fn_index(n->text);
            if (idx < 0 || !g_frame_frameless[idx]) return 0;
            return frame_scalar_type(g_frame_fns[idx].ret);
        }
        case AST_TERNARY:
            return n->child_count >= 3 && frame_operand_scalar(n->children[1]) &&
                   frame_operand_scalar(n->children[2]);
        default: return 0;
    }
}

// 1 = this subtree allocates nothing and needs no frame,
// 0 = a disqualifier was found. The tracker holds the
// local types visible at the current point; block-like
// nodes save and restore it so a declaration never leaks
// past its own scope (a leaked scalar type could mask a
// buffer-typed use in a sibling branch).
static int frame_walk_clean(ASTNode* n) {
    if (!n) return 1;
    switch (n->type) {
        case AST_BLOCK:
        case AST_IF:
        case AST_ELSE:
        case AST_WHILE:
        case AST_FOR:
        case AST_DO_WHILE: {
            int save = g_frame_var_count;
            int ok = 1;
            for (int i = 0; i < n->child_count && ok; i++)
                ok = frame_walk_clean(n->children[i]);
            g_frame_var_count = save;
            return ok;
        }
        case AST_VAR_DECL: {
            if (n->child_count < 2 || !n->children[1]) return 0;
            const char* t = n->children[1]->text;
            ASTNode* init = n->children[0];
            if (frame_scalar_type(t)) {
                frame_var_set(n->text, t);
                return frame_walk_clean(init);
            }
            if (strcmp(t, "var") == 0) {
                if (!init) return 0;
                if (init->type == AST_NUMBER) { frame_var_set(n->text, "int"); return 1; }
                if (init->type == AST_BOOL_LITERAL) { frame_var_set(n->text, "bool"); return 1; }
                if (init->type == AST_IDENTIFIER) {
                    const char* it = frame_var_get(init->text);
                    if (frame_scalar_type(it)) { frame_var_set(n->text, it); return 1; }
                }
                return 0;
            }
            return 0; // buffer, struct, ref, instance, unknown: needs a frame
        }
        case AST_ASSIGN: {
            if (strcmp(n->text, "=") != 0) return 0; // compound: string += allocates
            for (int i = 0; i < n->child_count; i++)
                if (!frame_walk_clean(n->children[i])) return 0;
            return 1;
        }
        case AST_CALL: {
            int idx = frame_fn_index(n->text);
            if (idx < 0 || !g_frame_frameless[idx]) return 0;
            for (int i = 0; i < n->child_count; i++)
                if (!frame_walk_clean(n->children[i])) return 0;
            return 1;
        }
        case AST_METHOD_CALL: {
            // A same-module call written module.fn(...).
            if (n->child_count > 0 && n->children[0] &&
                n->children[0]->type == AST_IDENTIFIER &&
                strcmp(n->children[0]->text, g_frame_module) == 0) {
                int idx = frame_fn_index(n->text);
                if (idx < 0 || !g_frame_frameless[idx]) return 0;
                for (int i = 0; i < n->child_count; i++)
                    if (!frame_walk_clean(n->children[i])) return 0;
                return 1;
            }
            if (strcmp(n->text, "length") == 0 || strcmp(n->text, "bytesize") == 0 ||
                strcmp(n->text, "owner") == 0 || strcmp(n->text, "view") == 0 ||
                strcmp(n->text, "byte_view") == 0) {
                for (int i = 0; i < n->child_count; i++)
                    if (!frame_walk_clean(n->children[i])) return 0;
                return 1;
            }
            return 0;
        }
        case AST_BINARY_OP: {
            if (n->child_count >= 2 && strcmp(n->text, "+") == 0) {
                if (!frame_operand_scalar(n->children[0]) ||
                    !frame_operand_scalar(n->children[1]))
                    return 0;
            }
            for (int i = 0; i < n->child_count; i++)
                if (!frame_walk_clean(n->children[i])) return 0;
            return 1;
        }
        case AST_CAST: {
            if (n->child_count < 1 || !n->children[0]) return 0;
            if (!frame_scalar_type(n->children[0]->text)) return 0;
            for (int i = 1; i < n->child_count; i++)
                if (!frame_walk_clean(n->children[i])) return 0;
            return 1;
        }
        case AST_RETURN:
        case AST_UNARY_OP:
        case AST_POST_INC:
        case AST_POST_DEC:
        case AST_TERNARY:
        case AST_ARRAY_ACCESS:
        case AST_MEMBER_ACCESS:
        case AST_IDENTIFIER:
        case AST_NUMBER:
        case AST_BOOL_LITERAL:
            for (int i = 0; i < n->child_count; i++)
                if (!frame_walk_clean(n->children[i])) return 0;
            return 1;
        default:
            return 0; // string/aggregate literals, break, continue,
                      // switch, spawn, yield, printf, net, ... : needs a frame
    }
}

static int frame_fn_clean(int idx) {
    ASTNode* fn = g_frame_fns[idx].node;
    if (!fn || fn->child_count == 0) return 0;
    ASTNode* body = fn->children[fn->child_count - 1];
    if (!body || body->type != AST_BLOCK) return 0;
    g_frame_var_count = 0;
    for (int i = 1; i < fn->child_count; i++) {
        ASTNode* p = fn->children[i];
        if (!p || p->type == AST_BLOCK) break;
        if (p->type == AST_VAR_DECL && p->child_count >= 2 && p->children[1])
            frame_var_set(p->text, p->children[1]->text);
    }
    return frame_walk_clean(body);
}

void codegen_analyze_frameless(const char* module, ASTNode** asts, int nast) {
    g_frame_fn_count = 0;
    memset(g_frame_frameless, 0, sizeof(g_frame_frameless));
    snprintf(g_frame_module, sizeof(g_frame_module), "%s", module);
    for (int a = 0; a < nast; a++) {
        ASTNode* ast = asts[a];
        if (!ast || ast->type != AST_PROGRAM) continue;
        for (int i = 0; i < ast->child_count; i++) {
            ASTNode* ch = ast->children[i];
            if (!ch || ch->type != AST_FUNCTION || !ch->text[0]) continue;
            if (ch->is_coroutine || ch->is_extern) continue;
            if (strcmp(ch->text, "main") == 0) continue;
            if (strcmp(ch->text, "create") == 0 || strcmp(ch->text, "module_create") == 0 ||
                strcmp(ch->text, "destroy") == 0 || strcmp(ch->text, "module_destroy") == 0) continue;
            if (frame_fn_index(ch->text) >= 0) continue; // duplicate: first wins
            if (g_frame_fn_count >= FRAME_MAX_FNS) continue;
            FrameFn* f = &g_frame_fns[g_frame_fn_count++];
            snprintf(f->name, sizeof(f->name), "%.*s", (int)sizeof(f->name) - 1, ch->text);
            f->node = ch;
            if (ch->child_count > 0 && ch->children[0] && ch->children[0]->text[0])
                snprintf(f->ret, sizeof(f->ret), "%.*s", (int)sizeof(f->ret) - 1, ch->children[0]->text);
            else
                snprintf(f->ret, sizeof(f->ret), "void");
        }
    }
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < g_frame_fn_count; i++) {
            if (g_frame_frameless[i]) continue;
            if (frame_fn_clean(i)) { g_frame_frameless[i] = 1; changed = 1; }
        }
    }
    const char* names[FRAME_MAX_FNS];
    int count = 0;
    for (int i = 0; i < g_frame_fn_count; i++)
        if (g_frame_frameless[i]) names[count++] = g_frame_fns[i].name;
    codegen_register_module_frameless(module, names, count);
}

// ---- A4 (redesign): may-fail functions ----
// A function may fail when its body can reach a
// come_ERR_set call: a typed map read (come_map_get_e
// sets ENOENT on a missing key), a string index
// (come_string_wchar_at_e sets ERANGE out of bounds), a
// failing runtime method (regex/at/tol/tod on strings,
// insert/remove on arrays, FILE operations, getenv), an
// instance-method call (the destroyed-instance guard sets
// EBADF before the body runs), a division/modulo, a
// spawn/yield, or a call to a may-fail function. Array
// index reads do NOT count: they use the OOB slot macro,
// which records its own location via #line, never the arm.
// The driver computes the set per module as a fixpoint
// over the call graph before generating any file, and
// registers the names here; emission then arms the ERR
// pending location only before statements that may fail,
// instead of before every statement. Arms elsewhere are
// pure overhead: no error can be set during those
// statements, so the location they would record is never
// consumed. Over-approximation is safe (an extra arm);
// under-approximation would record a stale location, so
// every rule below arms on doubt, and the statement
// predicate in codegen mode re-derives receiver types
// from dispatch (locals plus module variables), so a
// gap in the analysis only loses the caller-arm safety
// net, never the inner arm at the failing statement
// itself: the callee's own statements arm per the same
// predicate, and an inner arm always overwrites an
// outer one before any error is set.
#define MAYFAIL_MAX_FNS 256

typedef struct { char module[128]; char names[MAYFAIL_MAX_FNS][128]; int count; } MayfailModule;
static MayfailModule g_mayfail[64];
static int g_mayfail_count = 0;

void codegen_register_module_mayfail(const char* module, const char** names, int count) {
    MayfailModule* slot = NULL;
    for (int i = 0; i < g_mayfail_count; i++)
        if (strcmp(g_mayfail[i].module, module) == 0) slot = &g_mayfail[i];
    if (!slot) {
        if (g_mayfail_count >= 64) return;
        slot = &g_mayfail[g_mayfail_count++];
        memset(slot, 0, sizeof(*slot));
        snprintf(slot->module, sizeof(slot->module), "%s", module);
    }
    slot->count = 0;
    for (int i = 0; i < count && slot->count < MAYFAIL_MAX_FNS; i++) {
        snprintf(slot->names[slot->count], sizeof(slot->names[0]), "%s", names[i]);
        slot->count++;
    }
}

int codegen_mayfail_known(const char* module) {
    for (int i = 0; i < g_mayfail_count; i++)
        if (strcmp(g_mayfail[i].module, module) == 0) return 1;
    return 0;
}

int codegen_is_mayfail(const char* module, const char* fn) {
    for (int i = 0; i < g_mayfail_count; i++) {
        if (strcmp(g_mayfail[i].module, module) != 0) continue;
        for (int j = 0; j < g_mayfail[i].count; j++)
            if (strcmp(g_mayfail[i].names[j], fn) == 0) return 1;
        return 0;
    }
    return 0;
}

typedef struct { char name[128]; ASTNode* node; char ret[128]; } MayfailFn;
static MayfailFn g_mayfail_fns[MAYFAIL_MAX_FNS];
static int g_mayfail_fn_count = 0;
static int g_mayfail_flags[MAYFAIL_MAX_FNS];
static char g_mayfail_module[128];
static int g_mayfail_analysis = 0;

static FrameVar g_mayfail_vars[FRAME_MAX_VARS];
static int g_mayfail_var_count = 0;

static const char* dispatch_type_of(const char* name);

static int mayfail_fn_index(const char* name) {
    for (int i = 0; i < g_mayfail_fn_count; i++)
        if (strcmp(g_mayfail_fns[i].name, name) == 0) return i;
    return -1;
}

static void mayfail_var_set(const char* name, const char* type) {
    if (g_mayfail_var_count >= FRAME_MAX_VARS) return;
    snprintf(g_mayfail_vars[g_mayfail_var_count].name, sizeof(g_mayfail_vars[0].name), "%s", name);
    snprintf(g_mayfail_vars[g_mayfail_var_count].type, sizeof(g_mayfail_vars[0].type), "%s", type ? type : "");
    g_mayfail_var_count++;
}

static const char* mayfail_var_get(const char* name) {
    for (int i = g_mayfail_var_count - 1; i >= 0; i--)
        if (strcmp(g_mayfail_vars[i].name, name) == 0) return g_mayfail_vars[i].type;
    return NULL;
}

// The type of a name for may-fail dispatch: the analysis
// tracker in analysis mode, the codegen dispatch (locals
// plus module variables) in codegen mode.
static const char* mayfail_lookup_var(const char* name) {
    if (g_mayfail_analysis) return mayfail_var_get(name);
    return dispatch_type_of(name);
}

// 1 = may fail, 0 = proven not, -1 = unknown (arms on doubt).
static int mayfail_lookup_fn(const char* module, const char* fn) {
    if (g_mayfail_analysis && strcmp(module, g_mayfail_module) == 0) {
        int idx = mayfail_fn_index(fn);
        if (idx < 0) return -1;
        return g_mayfail_flags[idx];
    }
    if (!codegen_mayfail_known(module)) return -1;
    return codegen_is_mayfail(module, fn);
}

// Methods that never set ERR, on any receiver (verified
// against the come_ERR_set inventory: only the failing
// methods below, the map/string/array index helpers, the
// FILE operations, and the instance guard set errors).
static int mayfail_method_pure(const char* m) {
    if (!m) return 0;
    static const char* pure[] = {
        "length", "bytesize", "owner", "type", "dup", "chown",
        "lower", "upper", "substr", "find", "split", "split_n",
        "replace", "repeat", "trim", "ltrim", "rtrim", "join",
        "byte_array", "append", "clear", "resize", "free", "slice",
        "view", "byte_view",
        "put", "str", NULL };
    for (int i = 0; pure[i]; i++)
        if (strcmp(m, pure[i]) == 0) return 1;
    return 0;
}

// Methods that can set ERR, on any receiver: the failing
// string methods (regex family, at, tol, tod), array
// insert/remove, and the FILE/Proc operations.
static int mayfail_method_fails(const char* m) {
    if (!m) return 0;
    if (strcmp(m, "regex") == 0 || strncmp(m, "regex_", 6) == 0) return 1;
    static const char* fails[] = {
        "at", "tol", "tod", "insert", "remove", "get",
        "open", "close", "fdopen", "reopen", "fileno",
        "read", "write", "getc", "putc", "gets", "puts",
        "fname", "ungetc", "getenv", "init", "exit",
        "lookup_lower", NULL };
    for (int i = 0; fails[i]; i++)
        if (strcmp(m, fails[i]) == 0) return 1;
    return 0;
}

static int mayfail_library_type(const char* t) {
    if (!t) return 0;
    if (strcmp(t, "string") == 0 || strncmp(t, "map{", 4) == 0 ||
        strcmp(t, "map") == 0 || strcmp(t, "FILE") == 0) return 1;
    if (strchr(t, '[') != NULL) return 1;
    return frame_scalar_type(t);
}

// 1 = this subtree may set an error. The tracker holds
// the local types visible at the current point in
// analysis mode; block-like nodes save and restore it so
// a declaration never leaks past its own scope.
static int mayfail_walk(ASTNode* n) {
    if (!n) return 0;
    switch (n->type) {
        case AST_BLOCK:
        case AST_IF:
        case AST_ELSE:
        case AST_WHILE:
        case AST_FOR:
        case AST_DO_WHILE: {
            int save = g_mayfail_var_count;
            for (int i = 0; i < n->child_count; i++)
                if (mayfail_walk(n->children[i])) {
                    if (g_mayfail_analysis) g_mayfail_var_count = save;
                    return 1;
                }
            if (g_mayfail_analysis) g_mayfail_var_count = save;
            return 0;
        }
        case AST_VAR_DECL: {
            if (g_mayfail_analysis && n->child_count >= 2 && n->children[1]) {
                const char* t = n->children[1]->text;
                if (strcmp(t, "var") == 0) {
                    ASTNode* init = (n->child_count >= 1) ? n->children[0] : NULL;
                    if (init && init->type == AST_STRING_LITERAL) t = "string";
                    else if (init && init->type == AST_IDENTIFIER) {
                        const char* it = mayfail_var_get(init->text);
                        if (it) t = it;
                    } else if (init && init->type == AST_CALL) {
                        int idx = mayfail_fn_index(init->text);
                        if (idx >= 0) t = g_mayfail_fns[idx].ret;
                    }
                }
                mayfail_var_set(n->text, t);
            }
            if (n->child_count >= 1 && n->children[0])
                return mayfail_walk(n->children[0]);
            return 0;
        }
        case AST_CALL: {
            const char* curmod = g_mayfail_analysis ? g_mayfail_module : current_module;
            int r = mayfail_lookup_fn(curmod, n->text);
            if (r != 0) return 1;
            for (int i = 0; i < n->child_count; i++)
                if (mayfail_walk(n->children[i])) return 1;
            return 0;
        }
        case AST_METHOD_CALL: {
            for (int i = 0; i < n->child_count; i++)
                if (mayfail_walk(n->children[i])) return 1;
            const char* m = n->text;
            ASTNode* recv = (n->child_count > 0) ? n->children[0] : NULL;
            if (!recv || recv->type != AST_IDENTIFIER) {
                if (mayfail_method_pure(m)) return 0;
                if (mayfail_method_fails(m)) return 1;
                return 1;
            }
            if (strcmp(recv->text, "ERR") == 0) return 0;
            const char* curmod = g_mayfail_analysis ? g_mayfail_module : current_module;
            if (strcmp(recv->text, curmod) == 0) {
                int r = mayfail_lookup_fn(curmod, m);
                return r != 0;
            }
            const char* t = mayfail_lookup_var(recv->text);
            if (t) {
                if (mayfail_method_pure(m)) return 0;
                if (mayfail_method_fails(m)) return 1;
                if (mayfail_library_type(t)) return 0;
                return 1;
            }
            // Not a value: a module name (import or system)
            // or an unknown name; the registry decides, and
            // an unregistered module arms on doubt.
            int r = mayfail_lookup_fn(recv->text, m);
            return r != 0;
        }
        case AST_ARRAY_ACCESS: {
            // Mirror the emitter: string index and map read
            // set ERR; array index uses the OOB slot macro
            // (explicit location, never the arm).
            if (n->child_count >= 1 && n->children[0] &&
                n->children[0]->type == AST_IDENTIFIER) {
                const char* t = mayfail_lookup_var(n->children[0]->text);
                if (t && strcmp(t, "string") == 0) return 1;
                if (t && (strncmp(t, "map{", 4) == 0 || strcmp(t, "map") == 0)) return 1;
            }
            for (int i = 0; i < n->child_count; i++)
                if (mayfail_walk(n->children[i])) return 1;
            return 0;
        }
        case AST_ASSIGN: {
            // A plain map-index write (m[k] = v) is a put,
            // which never sets ERR; only the key and value
            // expressions can fail. Compound assignment
            // still reads first, so it walks normally (the
            // map-read arm fires). A string-index write
            // still sets ERR through the index helper.
            if (n->child_count >= 2 &&
                strcmp(n->text, "=") == 0 && n->children[0] &&
                n->children[0]->type == AST_ARRAY_ACCESS) {
                ASTNode* lhs = n->children[0];
                const char* t = NULL;
                if (lhs->child_count >= 1 && lhs->children[0] &&
                    lhs->children[0]->type == AST_IDENTIFIER)
                    t = mayfail_lookup_var(lhs->children[0]->text);
                if (t && (strncmp(t, "map{", 4) == 0 || strcmp(t, "map") == 0)) {
                    for (int i = 1; i < lhs->child_count; i++)
                        if (mayfail_walk(lhs->children[i])) return 1;
                    for (int i = 1; i < n->child_count; i++)
                        if (mayfail_walk(n->children[i])) return 1;
                    return 0;
                }
            }
            for (int i = 0; i < n->child_count; i++)
                if (mayfail_walk(n->children[i])) return 1;
            return 0;
        }
        case AST_BINARY_OP: {
            if (n->child_count >= 2 &&
                (strcmp(n->text, "/") == 0 || strcmp(n->text, "%") == 0)) return 1;
            for (int i = 0; i < n->child_count; i++)
                if (mayfail_walk(n->children[i])) return 1;
            return 0;
        }
        case AST_SPAWN:
        case AST_YIELD:
            return 1;
        default: {
            for (int i = 0; i < n->child_count; i++)
                if (mayfail_walk(n->children[i])) return 1;
            return 0;
        }
    }
}

static int mayfail_fn_body_may_fail(int idx) {
    ASTNode* fn = g_mayfail_fns[idx].node;
    if (!fn || fn->child_count == 0) return 1;
    ASTNode* body = fn->children[fn->child_count - 1];
    if (!body || body->type != AST_BLOCK) return 1;
    g_mayfail_var_count = 0;
    for (int i = 1; i < fn->child_count; i++) {
        ASTNode* p = fn->children[i];
        if (!p || p->type == AST_BLOCK) break;
        if (p->type == AST_VAR_DECL && p->child_count >= 2 && p->children[1])
            mayfail_var_set(p->text, p->children[1]->text);
    }
    return mayfail_walk(body);
}

void codegen_analyze_mayfail(const char* module, ASTNode** asts, int nast) {
    g_mayfail_fn_count = 0;
    memset(g_mayfail_flags, 0, sizeof(g_mayfail_flags));
    snprintf(g_mayfail_module, sizeof(g_mayfail_module), "%s", module);
    g_mayfail_analysis = 1;
    for (int a = 0; a < nast; a++) {
        ASTNode* ast = asts[a];
        if (!ast || ast->type != AST_PROGRAM) continue;
        for (int i = 0; i < ast->child_count; i++) {
            ASTNode* ch = ast->children[i];
            if (!ch || ch->type != AST_FUNCTION || !ch->text[0]) continue;
            if (ch->is_extern) continue;
            if (ch->child_count == 0) continue;
            ASTNode* body = ch->children[ch->child_count - 1];
            if (!body || body->type != AST_BLOCK) continue;
            if (mayfail_fn_index(ch->text) >= 0) continue;
            if (g_mayfail_fn_count >= MAYFAIL_MAX_FNS) continue;
            MayfailFn* f = &g_mayfail_fns[g_mayfail_fn_count++];
            snprintf(f->name, sizeof(f->name), "%.*s", (int)sizeof(f->name) - 1, ch->text);
            f->node = ch;
            if (ch->child_count > 0 && ch->children[0] && ch->children[0]->text[0])
                snprintf(f->ret, sizeof(f->ret), "%.*s", (int)sizeof(f->ret) - 1, ch->children[0]->text);
            else
                snprintf(f->ret, sizeof(f->ret), "void");
        }
    }
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < g_mayfail_fn_count; i++) {
            if (g_mayfail_flags[i]) continue;
            if (mayfail_fn_body_may_fail(i)) { g_mayfail_flags[i] = 1; changed = 1; }
        }
    }
    const char* names[MAYFAIL_MAX_FNS];
    int count = 0;
    for (int i = 0; i < g_mayfail_fn_count; i++)
        if (g_mayfail_flags[i]) names[count++] = g_mayfail_fns[i].name;
    codegen_register_module_mayfail(module, names, count);
    g_mayfail_analysis = 0;
}

// The codegen-mode predicate: arms only before statements
// that may set an error. The #line directive stays
// unconditional (OOB slots read their location from it).
int codegen_stmt_may_fail(ASTNode* node) {
    g_mayfail_analysis = 0;
    g_mayfail_var_count = 0;
    return mayfail_walk(node);
}

int codegen_module_struct_at(const char* module, int i, const char** name,
                             const char** def, int* packed, int* align) {
    ModuleStructs* ms = module_structs_of(module);
    if (!ms || i < 0 || i >= ms->count) return 0;
    *name = ms->names[i];
    *def = ms->defs[i];
    if (packed) *packed = ms->packeds[i];
    if (align) *align = ms->aligns[i];
    return 1;
}

// The type a name has for dispatch purposes: a local's
// recorded type, or - inside an instance-form body - a
// module variable's declared type from the registry
// (COIL-140). Module variables are not locals, so without
// this the method/array dispatch cannot see their types.
static const char* dispatch_type_of(const char* name) {
    const char* t = get_local_variable_type(name);
    if (!t) {
        // Not a local: a module variable of the current
        // module has its declared type (COIL-140). This
        // also gives singleton modules typed module
        // variables for dispatch - indexing one used to
        // fail with "element type is not known".
        const char* rt = NULL;
        if (codegen_module_var_index(current_module, name, &rt) >= 0) t = rt;
    }
    return t;
}

// Header-typed values (COIL-135) carry a context pointer and
// are built by runtime calls - come_map_new, COME_HDR_INIT,
// come_string_new - so a variable of one of these types
// cannot take an initializer at C file scope, where
// initializers must be constant (COIL-142).
static int is_header_type(const char* t) {
    if (!t) return 0;
    return strncmp(t, "map{", 4) == 0 || strcmp(t, "map") == 0 ||
           strcmp(t, "string") == 0 || strchr(t, '[') != NULL;
}

// The C type of a header-typed variable's bare file-scope
// declaration, mirroring the declaration branches below:
// a zeroed header is a live empty value (the map runtime
// grows lazily from a NULL data block, arrays likewise),
// so the bare global needs no file-scope initializer.
static void header_c_type(const char* t, char* out, size_t n) {
    if (strncmp(t, "map{", 4) == 0 || strcmp(t, "map") == 0) {
        snprintf(out, n, "come_map_t");
    } else if (strcmp(t, "string") == 0) {
        snprintf(out, n, "come_string_t");
    } else if (strcmp(t, "string[]") == 0) {
        snprintf(out, n, "come_string_list_t");
    } else {
        char raw[64];
        const char* lb = strchr(t, '[');
        size_t rl = lb ? (size_t)(lb - t) : strlen(t);
        if (rl >= sizeof(raw)) rl = sizeof(raw) - 1;
        memcpy(raw, t, rl);
        raw[rl] = 0;
        if (strcmp(raw, "int") == 0 || strcmp(raw, "var") == 0) snprintf(out, n, "come_int_array_t");
        else if (strcmp(raw, "byte") == 0) snprintf(out, n, "come_byte_array_t");
        else snprintf(out, n, "come_array_%s_t", raw);
    }
}

const char* codegen_module_fn_ret(const char* module, const char* fn) {
    ModuleState* ms = module_state_of(module);
    if (!ms) return NULL;
    for (int i = 0; i < ms->fn_count; i++)
        if (strcmp(ms->fn_names[i], fn) == 0) return ms->fn_rets[i];
    return NULL;
}

// If `call` (AST_METHOD_CALL) calls a registered module
// function - through the namespace (receiver is the module
// name) or through an instance variable - return that
// function's registered return type, else NULL (COIL-140).
static const char* method_call_module_ret(ASTNode* call) {
    if (!call || call->type != AST_METHOD_CALL || call->child_count < 1 ||
        !call->children[0] || call->children[0]->type != AST_IDENTIFIER)
        return NULL;
    const char* rn = call->children[0]->text;
    const char* lt = get_local_variable_type(rn);
    if (lt && strncmp(lt, "instance:", 9) == 0)
        return codegen_module_fn_ret(lt + 9, call->text);
    if (lt && codegen_module_can_instance(lt))
        return codegen_module_fn_ret(lt, call->text);
    return codegen_module_fn_ret(rn, call->text);
}

int codegen_module_fn_count(const char* module) {
    ModuleState* ms = module_state_of(module);
    return ms ? ms->fn_count : 0;
}

int codegen_module_fn_at(const char* module, int i, const char** name,
                         const char** ret, const char** params) {
    ModuleState* ms = module_state_of(module);
    if (!ms || i < 0 || i >= ms->fn_count) return 0;
    if (name) *name = ms->fn_names[i];
    if (ret) *ret = ms->fn_rets[i];
    if (params) *params = ms->fn_params[i];
    return 1;
}

int codegen_module_var_index(const char* module, const char* name, const char** type_out) {
    ModuleState* ms = module_state_of(module);
    if (!ms) return -1;
    for (int i = 0; i < ms->var_count; i++)
        if (strcmp(ms->var_names[i], name) == 0) {
            if (type_out) *type_out = ms->var_types[i];
            return i;
        }
    return -1;
}

static int is_group_symbol(const char* name) {
    for (int m = 0; m < g_group_symbol_modules; m++) {
        if (strcmp(g_group_symbols[m].module, current_module) != 0) continue;
        for (int i = 0; i < g_group_symbols[m].count; i++) {
            if (strcmp(g_group_symbols[m].names[i], name) == 0) return 1;
        }
    }
    return 0;
}

// 1 while generating a coroutine body (COIL-122).
static int g_in_coroutine = 0;

// Loop/switch nesting while generating a function body
// (COIL-129): break and continue outside any loop used to
// surface as C compiler errors. Reset per function.
static int g_break_depth = 0;
static int g_continue_depth = 0;

// COIL-157: lexical scope depth within the current
// function (0 = the function body, i.e. the marker
// frame). Nested blocks and loop bodies count up; the
// depth is recorded with every local at declaration, so
// an assignment can adopt a value to its destination's
// scope. Loop bodies push their enclosing depth here so
// break/continue can unwind to it.
static int g_scope_depth = 0;
static int g_loop_depths[64];
static int g_loop_depth_count = 0;
static void scope_depth_push(void) { g_scope_depth++; g_local_add_depth = g_scope_depth; }
static void scope_depth_pop(void) { g_scope_depth--; g_local_add_depth = g_scope_depth; }

// Buffer types transfer ownership on spawn (spec 9.1/7);
// value types are simply copied into the args struct.
static int is_buffer_type(const char* t) {
    return t && (strcmp(t, "string") == 0 || strstr(t, "[]") != NULL ||
                 strncmp(t, "map{", 4) == 0 || strcmp(t, "link") == 0);
}

// Parameter count of a function/coroutine node: children are
// [ret, params..., body].
static int fn_param_count(ASTNode* fn) {
    int n = 0;
    for (int i = 1; i < fn->child_count; i++) {
        if (fn->children[i]->type == AST_BLOCK) break;
        n++;
    }
    return n;
}

static int is_fn_type(const char* t) { return t && strncmp(t, "fn:", 3) == 0; }

// The C type of a coroutine parameter, as stored in the args
// struct (COIL-120). Mirrors the function prototype mapping:
// strings and collections travel as pointers.
static void emit_coro_field_type(FILE* f, const char* type_text) {
    if (is_fn_type(type_text)) {
        codegen_error("coroutine parameters cannot be functions in v1");
        fprintf(f, "void*");
    } else if (strstr(type_text, "[]")) {
        char raw[64];
        size_t n = strlen(type_text);
        if (n >= 2 && n - 2 < sizeof(raw)) {
            memcpy(raw, type_text, n - 2);
            raw[n - 2] = 0;
            if (strcmp(raw, "int") == 0) fprintf(f, "come_int_array_t");
            else if (strcmp(raw, "byte") == 0) fprintf(f, "come_byte_array_t");
            else if (strcmp(raw, "string") == 0) fprintf(f, "come_string_list_t");
            else fprintf(f, "come_array_t");
        } else {
            fprintf(f, "come_array_t");
        }
    } else if (strcmp(type_text, "string") == 0) {
        fprintf(f, "come_string_t");
    } else if (strncmp(type_text, "map{", 4) == 0) {
        fprintf(f, "come_map_t");
    } else if (strcmp(type_text, "link") == 0) {
        fprintf(f, "come_link_t*");
    } else {
        fprintf(f, "%s", type_text);
    }
}
// Split a comma-separated type list at nesting depth 0.
static int split_types(const char* s, char parts[][160], int max_parts) {
    int n = 0, depth = 0;
    const char* start = s;
    for (const char* p = s; ; p++) {
        char c = *p;
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
        if ((c == ',' && depth == 0) || c == 0) {
            if (n < max_parts) {
                size_t len = (size_t)(p - start);
                if (len > 159) len = 159;
                memcpy(parts[n], start, len);
                parts[n][len] = 0;
                n++;
            }
            start = p + 1;
            if (c == 0) break;
        }
    }
    return n;
}
// ---- Design 1 (redesign): may-escape analysis ----
// For each function, which buffer-typed parameters may
// escape the call: be returned, stored in a module
// variable, an instance field, or a container, passed
// to a function whose corresponding parameter escapes,
// captured by a coroutine, or re-parented by chown.
// Over-approximation is safe (an escaping value is
// treated conservatively); under-approximation would
// let an arena or a view free a live value, so every
// rule escapes on doubt: unknown callees, cross-module
// calls, and container mutators all count as escaping.
// The analysis is a monotone fixpoint over functions
// and locals: escaping sets only grow, so it
// terminates. It runs in analyze_module alongside
// frameless/mayfail, and the registry is what future
// codegen (arenas, views) will consult. `come escapes`
// dumps the classification: "escapes" vs "transient"
// (provably non-escaping, safe for an arena or view).
// Precision gaps, all in the safe direction: values
// flowing through a container index (m[k]) or a
// non-mutator method result are not tracked back to
// the container; cross-module calls escape their
// identifier arguments; mutator arguments escape even
// when the container is local; names are tracked flat
// (shadowing over-approximates by name).
#define ESC_MAX_FNS 256
#define ESC_MAX_NAMES 320
#define ESC_MAX_PARAMS 64

typedef struct {
    char name[128];
    ASTNode* node;      // AST_FUNCTION
    ASTNode* body;      // BLOCK child, or NULL
    char ret[128];
    char names[ESC_MAX_NAMES][128]; // params first, then locals
    int is_param[ESC_MAX_NAMES];
    int is_buffer[ESC_MAX_NAMES];   // params only
    int esc[ESC_MAX_NAMES];         // fixpoint state
    int count;
    int param_count;
} EscFn;

static EscFn g_esc_fns[ESC_MAX_FNS];
static int g_esc_fn_count = 0;
static char g_esc_module[128];

// Registry of escaping parameters, for cross-module
// use by future codegen passes.
#define ESC_REG_MAX 4096
typedef struct { char module[128]; char fn[128]; char param[128]; } EscReg;
static EscReg g_esc_reg[ESC_REG_MAX];
static int g_esc_reg_count = 0;
static char g_esc_reg_modules[64][128];
static int g_esc_reg_module_count = 0;

static void esc_reg_add(const char* module, const char* fn, const char* param) {
    for (int i = 0; i < g_esc_reg_count; i++)
        if (strcmp(g_esc_reg[i].module, module) == 0 &&
            strcmp(g_esc_reg[i].fn, fn) == 0 &&
            strcmp(g_esc_reg[i].param, param) == 0) return;
    if (g_esc_reg_count >= ESC_REG_MAX) return;
    EscReg* r = &g_esc_reg[g_esc_reg_count++];
    snprintf(r->module, sizeof(r->module), "%s", module);
    snprintf(r->fn, sizeof(r->fn), "%s", fn);
    snprintf(r->param, sizeof(r->param), "%s", param);
}

int codegen_esc_module_known(const char* module) {
    if (!module) return 0;
    for (int i = 0; i < g_esc_reg_module_count; i++)
        if (strcmp(g_esc_reg_modules[i], module) == 0) return 1;
    return 0;
}

int codegen_esc_param_escapes(const char* module, const char* fn, const char* param) {
    if (!module || !fn || !param) return 0;
    for (int i = 0; i < g_esc_reg_count; i++)
        if (strcmp(g_esc_reg[i].module, module) == 0 &&
            strcmp(g_esc_reg[i].fn, fn) == 0 &&
            strcmp(g_esc_reg[i].param, param) == 0) return 1;
    return 0;
}

static int esc_name_index(EscFn* fn, const char* name) {
    if (!fn || !name) return -1;
    for (int i = 0; i < fn->count; i++)
        if (strcmp(fn->names[i], name) == 0) return i;
    return -1;
}

static void esc_mark(EscFn* fn, const char* name, int* changed) {
    int idx = esc_name_index(fn, name);
    if (idx < 0) return;
    if (!fn->esc[idx]) { fn->esc[idx] = 1; if (changed) *changed = 1; }
}

static void esc_add_name(EscFn* fn, const char* name, int is_param, int is_buffer) {
    if (!fn || !name || !name[0]) return;
    if (esc_name_index(fn, name) >= 0) return; // shadowing: first wins, by name
    if (fn->count >= ESC_MAX_NAMES) return;
    int idx = fn->count++;
    snprintf(fn->names[idx], sizeof(fn->names[idx]), "%s", name);
    fn->is_param[idx] = is_param;
    fn->is_buffer[idx] = is_buffer;
    fn->esc[idx] = 0;
    if (is_param) fn->param_count++;
}

// The names[] index of a function's p-th parameter
// (params are added first, in declared order), or -1.
static int esc_param_index(EscFn* fn, int p) {
    if (!fn || p < 0) return -1;
    int seen = 0;
    for (int i = 0; i < fn->count; i++) {
        if (!fn->is_param[i]) continue;
        if (seen == p) return i;
        seen++;
    }
    return -1;
}

static void esc_collect_names(ASTNode* n, EscFn* fn) {
    if (!n || !fn) return;
    if (n->type == AST_VAR_DECL) {
        esc_add_name(fn, n->text, 0, 0);
        // The type node (children[1]) names a type, not
        // a variable: only the init may hold decls.
        if (n->child_count > 0 && n->children[0]) esc_collect_names(n->children[0], fn);
        return;
    }
    for (int i = 0; i < n->child_count; i++)
        if (n->children[i]) esc_collect_names(n->children[i], fn);
}

// A dup(0) ref creation: method "dup" with an argument,
// receiver an identifier. Returns the receiver name or NULL.
static const char* esc_dup0_receiver(ASTNode* n) {
    if (!n || n->type != AST_METHOD_CALL || strcmp(n->text, "dup") != 0) return NULL;
    if (n->child_count < 2) return NULL; // dup() with no arg is a deep copy
    if (!n->children[0] || n->children[0]->type != AST_IDENTIFIER) return NULL;
    return n->children[0]->text;
}

// Mark receivers of dup(0) refs anywhere inside an
// expression (a ref stored or returned escapes its
// source). Plain identifiers inside are NOT marked:
// f(x) keeps x unless f retains it (the call rule).
static void esc_mark_dup0_in(ASTNode* n, EscFn* fn, int* changed) {
    if (!n || !fn) return;
    const char* recv = esc_dup0_receiver(n);
    if (recv) { esc_mark(fn, recv, changed); return; }
    for (int i = 0; i < n->child_count; i++)
        if (n->children[i]) esc_mark_dup0_in(n->children[i], fn, changed);
}

// A value in a return/store position: the value itself
// escapes (an identifier, or the elements of an
// aggregate literal), and any dup(0) ref inside it
// escapes its source. Anything else (a call, an index,
// a concatenation) is a new value: its operands do
// not escape here (calls are covered by the call rule).
static void esc_mark_value_expr(ASTNode* e, EscFn* fn, int* changed) {
    if (!e || !fn) return;
    if (e->type == AST_IDENTIFIER) { esc_mark(fn, e->text, changed); return; }
    const char* recv = esc_dup0_receiver(e);
    if (recv) { esc_mark(fn, recv, changed); return; }
    if (e->type == AST_AGGREGATE_INIT || e->type == AST_MAP_PAIR) {
        for (int i = 0; i < e->child_count; i++)
            if (e->children[i]) esc_mark_value_expr(e->children[i], fn, changed);
        return;
    }
    esc_mark_dup0_in(e, fn, changed);
}

// Every identifier in a subtree (spawn capture, yield):
// over-approximate, the coroutine outlives the call.
static void esc_mark_all_identifiers(ASTNode* n, EscFn* fn, int* changed) {
    if (!n || !fn) return;
    if (n->type == AST_IDENTIFIER) { esc_mark(fn, n->text, changed); return; }
    for (int i = 0; i < n->child_count; i++)
        if (n->children[i]) esc_mark_all_identifiers(n->children[i], fn, changed);
}

// The identifier (or dup(0) source) passed as a call
// argument, or NULL if the argument is a new value.
static const char* esc_arg_value(ASTNode* arg) {
    if (!arg) return NULL;
    if (arg->type == AST_IDENTIFIER) return arg->text;
    return esc_dup0_receiver(arg);
}

// Apply the call rule for one call site: each
// identifier argument escapes if the callee's
// corresponding parameter escapes, or (for unknown
// or cross-module callees) unconditionally.
static void esc_apply_call(EscFn* fn, const char* callee_module, const char* callee,
                           ASTNode* call, int arg_start, int* changed) {
    if (!fn || !call || !callee) return;
    int callee_idx = -1;
    if (!callee_module || strcmp(callee_module, g_esc_module) == 0) {
        for (int i = 0; i < g_esc_fn_count; i++)
            if (strcmp(g_esc_fns[i].name, callee) == 0) { callee_idx = i; break; }
    }
    for (int a = arg_start; a < call->child_count; a++) {
        const char* v = esc_arg_value(call->children[a]);
        if (!v) continue;
        if (callee_idx >= 0) {
            EscFn* cf = &g_esc_fns[callee_idx];
            int pidx = esc_param_index(cf, a - arg_start);
            if (pidx >= 0 && cf->esc[pidx]) esc_mark(fn, v, changed);
        } else {
            // Unknown or cross-module callee: escapes on doubt.
            esc_mark(fn, v, changed);
        }
    }
}

static void esc_walk(ASTNode* n, EscFn* fn, int* changed) {
    if (!n || !fn) return;
    switch (n->type) {
        case AST_VAR_DECL: {
            // Edge: the initializer flows into the declared name.
            if (n->child_count > 0 && n->children[0]) {
                ASTNode* init = n->children[0];
                int idx = esc_name_index(fn, n->text);
                if (idx >= 0 && fn->esc[idx]) {
                    const char* v = esc_arg_value(init);
                    if (v) esc_mark(fn, v, changed);
                }
                esc_walk(init, fn, changed);
            }
            return; // never walk the type node
        }
        case AST_ASSIGN: {
            if (n->child_count >= 2 && strcmp(n->text, "=") == 0) {
                ASTNode* lhs = n->children[0];
                ASTNode* rhs = n->children[1];
                if (lhs && lhs->type == AST_IDENTIFIER) {
                    int idx = esc_name_index(fn, lhs->text);
                    if (idx >= 0) {
                        // A local: the value flows into it (edge).
                        if (fn->esc[idx]) {
                            const char* v = esc_arg_value(rhs);
                            if (v) esc_mark(fn, v, changed);
                        }
                    } else {
                        // A module variable: the value escapes.
                        esc_mark_value_expr(rhs, fn, changed);
                    }
                } else if (lhs && (lhs->type == AST_MEMBER_ACCESS || lhs->type == AST_ARRAY_ACCESS)) {
                    // An instance field or a container element: escapes.
                    esc_mark_value_expr(rhs, fn, changed);
                }
                // Other LHS shapes (destructuring): no sink; walk below.
            }
            for (int i = 0; i < n->child_count; i++)
                if (n->children[i]) esc_walk(n->children[i], fn, changed);
            return;
        }
        case AST_CALL: {
            esc_apply_call(fn, NULL, n->text, n, 0, changed);
            for (int i = 0; i < n->child_count; i++)
                if (n->children[i]) esc_walk(n->children[i], fn, changed);
            return;
        }
        case AST_METHOD_CALL: {
            // A same-module call written module.fn(...).
            if (n->child_count > 0 && n->children[0] &&
                n->children[0]->type == AST_IDENTIFIER &&
                strcmp(n->children[0]->text, g_esc_module) == 0) {
                esc_apply_call(fn, g_esc_module, n->text, n, 1, changed);
            } else if (strcmp(n->text, "chown") == 0) {
                if (n->child_count > 0 && n->children[0] &&
                    n->children[0]->type == AST_IDENTIFIER)
                    esc_mark(fn, n->children[0]->text, changed);
            } else if (strcmp(n->text, "put") == 0 || strcmp(n->text, "append") == 0 ||
                       strcmp(n->text, "insert") == 0 || strcmp(n->text, "set") == 0 ||
                       strcmp(n->text, "cpy") == 0) {
                // A container mutator stores its arguments.
                for (int i = 1; i < n->child_count; i++) {
                    const char* v = esc_arg_value(n->children[i]);
                    if (v) esc_mark(fn, v, changed);
                }
            }
            // Pure methods (length/bytesize/owner) and other
            // runtime methods borrow their arguments.
            for (int i = 0; i < n->child_count; i++)
                if (n->children[i]) esc_walk(n->children[i], fn, changed);
            return;
        }
        case AST_RETURN: {
            if (fn->ret[0] && strcmp(fn->ret, "void") != 0 && !frame_scalar_type(fn->ret)) {
                for (int i = 0; i < n->child_count; i++)
                    if (n->children[i]) esc_mark_value_expr(n->children[i], fn, changed);
            }
            for (int i = 0; i < n->child_count; i++)
                if (n->children[i]) esc_walk(n->children[i], fn, changed);
            return;
        }
        case AST_SPAWN:
        case AST_YIELD: {
            esc_mark_all_identifiers(n, fn, changed);
            for (int i = 0; i < n->child_count; i++)
                if (n->children[i]) esc_walk(n->children[i], fn, changed);
            return;
        }
        default: {
            for (int i = 0; i < n->child_count; i++)
                if (n->children[i]) esc_walk(n->children[i], fn, changed);
            return;
        }
    }
}

static void esc_collect_function(ASTNode* fn_node, EscFn* f) {
    if (fn_node->child_count > 0 && fn_node->children[0] &&
        fn_node->children[0]->type != AST_VAR_DECL &&
        fn_node->children[0]->type != AST_BLOCK)
        snprintf(f->ret, sizeof(f->ret), "%.*s", (int)sizeof(f->ret) - 1, fn_node->children[0]->text);
    else
        snprintf(f->ret, sizeof(f->ret), "void");
    ASTNode* body = NULL;
    for (int i = 0; i < fn_node->child_count; i++) {
        ASTNode* ch = fn_node->children[i];
        if (!ch) continue;
        if (ch->type == AST_VAR_DECL) {
            const char* t = (ch->child_count >= 2 && ch->children[1]) ? ch->children[1]->text : "var";
            int is_buffer = !frame_scalar_type(t) && strcmp(t, "void") != 0;
            esc_add_name(f, ch->text, 1, is_buffer);
        } else if (ch->type == AST_BLOCK) {
            body = ch;
        }
    }
    f->body = body;
    if (body) esc_collect_names(body, f);
}

void codegen_analyze_escapes(const char* module, ASTNode** asts, int nast) {
    if (!module) return;
    snprintf(g_esc_module, sizeof(g_esc_module), "%s", module);
    g_esc_fn_count = 0;
    for (int a = 0; a < nast; a++) {
        ASTNode* ast = asts[a];
        if (!ast || ast->type != AST_PROGRAM) continue;
        for (int i = 0; i < ast->child_count; i++) {
            ASTNode* ch = ast->children[i];
            if (!ch || ch->type != AST_FUNCTION || !ch->text[0]) continue;
            if (g_esc_fn_count >= ESC_MAX_FNS) continue;
            EscFn* f = &g_esc_fns[g_esc_fn_count];
            memset(f, 0, sizeof(*f));
            snprintf(f->name, sizeof(f->name), "%.*s", (int)sizeof(f->name) - 1, ch->text);
            f->node = ch;
            esc_collect_function(ch, f);
            g_esc_fn_count++;
        }
    }
    int changed = 1, iter = 0;
    while (changed && iter < 10000) {
        changed = 0; iter++;
        for (int i = 0; i < g_esc_fn_count; i++) {
            EscFn* f = &g_esc_fns[i];
            if (f->body) esc_walk(f->body, f, &changed);
        }
    }
    int known = 0;
    for (int i = 0; i < g_esc_reg_module_count; i++)
        if (strcmp(g_esc_reg_modules[i], module) == 0) known = 1;
    if (!known && g_esc_reg_module_count < 64) {
        snprintf(g_esc_reg_modules[g_esc_reg_module_count], sizeof(g_esc_reg_modules[0]), "%s", module);
        g_esc_reg_module_count++;
    }
    for (int i = 0; i < g_esc_fn_count; i++) {
        EscFn* f = &g_esc_fns[i];
        for (int j = 0; j < f->count; j++) {
            if (f->is_param[j] && f->is_buffer[j] && f->esc[j])
                esc_reg_add(module, f->name, f->names[j]);
        }
    }
}

void codegen_dump_escapes(FILE* out) {
    if (!out) return;
    for (int i = 0; i < g_esc_fn_count; i++) {
        EscFn* f = &g_esc_fns[i];
        for (int j = 0; j < f->count; j++) {
            if (!f->is_param[j] || !f->is_buffer[j]) continue;
            fprintf(out, "%s.%s(%s): %s\n", g_esc_module, f->name, f->names[j],
                    f->esc[j] ? "escapes" : "transient");
        }
    }
}

// ---- Tuple returns (spec 8.2, COIL-127) ----
// A function declared `(int, string) f(...)` returns a
// generated C struct by value; call sites destructure it
// with (a, b) = f(...). Element types are recorded here so
// returns and destructures can be checked and emitted.

// Map a Come type text to its C declarator type.
static void come_type_to_c(const char* t, char* out, size_t n) {
    if (is_ref_type(t)) {
        // &(struct Vec3) -> struct Vec3* (COIL-136)
        char inner[256], ic[256];
        ref_inner_type(t, inner, sizeof(inner));
        come_type_to_c(inner, ic, sizeof(ic));
        snprintf(out, n, "%s*", ic);
        return;
    }
    size_t len = strlen(t);
    if (len >= 2 && strcmp(t + len - 2, "[]") == 0) {
        char raw[128];
        snprintf(raw, sizeof(raw), "%.*s", (int)(len - 2), t);
        if (strcmp(raw, "int") == 0) snprintf(out, n, "come_int_array_t");
        else if (strcmp(raw, "byte") == 0) snprintf(out, n, "come_byte_array_t");
        else if (strcmp(raw, "string") == 0) snprintf(out, n, "come_string_list_t");
        else snprintf(out, n, "come_array_%s_t", raw);
    } else if (strcmp(t, "string") == 0) {
        snprintf(out, n, "come_string_t");
    } else if (strcmp(t, "link") == 0) {
        snprintf(out, n, "come_link_t*");
    } else if (strncmp(t, "map{", 4) == 0) {
        snprintf(out, n, "come_map_t");
    } else {
        // Scalars, struct X, and typedef'd names pass through:
        // the runtime headers define byte/short/... and user
        // aliases are emitted as typedefs.
        snprintf(out, n, "%s", t);
    }
}

typedef struct {
    char name[128];
    int count;
    char elems[8][160];
} TupleFn;
static TupleFn g_tuple_fns[64];
static int g_tuple_fn_count = 0;
static const TupleFn* g_cur_tuple = NULL; // function being generated

static void collect_tuple_fns(ASTNode* ast) {
    g_tuple_fn_count = 0;
    if (!ast || ast->type != AST_PROGRAM) return;
    for (int i = 0; i < ast->child_count && g_tuple_fn_count < 64; i++) {
        ASTNode* ch = ast->children[i];
        if (!ch || ch->type != AST_FUNCTION || ch->child_count == 0) continue;
        const char* ret = ch->children[0]->text;
        if (!ret || ret[0] != '(') continue;
        char inner[600];
        snprintf(inner, sizeof(inner), "%.*s", (int)strlen(ret) - 2, ret + 1);
        TupleFn* tf = &g_tuple_fns[g_tuple_fn_count];
        snprintf(tf->name, sizeof(tf->name), "%.*s", (int)sizeof(tf->name) - 1, ch->text);
        tf->count = split_types(inner, tf->elems, 8);
        for (int j = 0; j < tf->count; j++) {
            // Trim surrounding spaces from each element type.
            char* s = tf->elems[j];
            while (*s == ' ') memmove(s, s + 1, strlen(s));
            size_t l = strlen(s);
            while (l > 0 && s[l - 1] == ' ') s[--l] = 0;
        }
        g_tuple_fn_count++;
    }
}

static const TupleFn* find_tuple_fn(const char* name) {
    for (int i = 0; i < g_tuple_fn_count; i++) {
        if (strcmp(g_tuple_fns[i].name, name) == 0) return &g_tuple_fns[i];
    }
    return NULL;
}

// Is parameter `idx` (0-based) of module function
// mod.fn declared as a string? Read from the registry the
// driver built from the defining module (COIL-153) - the
// fact a cross-module call site needs to materialize a
// string literal argument for that parameter.
static int registry_param_is_string(const char* mod, const char* fn, int idx) {
    if (idx < 0) return 0;
    int n = codegen_module_fn_count(mod);
    for (int i = 0; i < n; i++) {
        const char *nm, *ret, *params;
        if (!codegen_module_fn_at(mod, i, &nm, &ret, &params)) continue;
        if (strcmp(nm, fn) != 0) continue;
        if (!params || !params[0]) return 0;
        char parts[32][160];
        int np = split_types(params, parts, 32);
        if (idx >= np) return 0;
        char* s = parts[idx];
        while (*s == ' ') s++;
        size_t l = strlen(s);
        while (l > 0 && s[l - 1] == ' ') s[--l] = 0;
        return strcmp(s, "string") == 0;
    }
    return 0;
}

// A tuple-returning function resolved through the module
// registry (COIL-146): fills `out` from the registered
// return type of `name` in `module`. The table above only
// knows the file being generated; the registry knows every
// file of every module in the build, which is what a
// cross-file or cross-module destructure needs.
static int registry_tuple_fn(const char* module, const char* name, TupleFn* out) {
    const char* ret = codegen_module_fn_ret(module, name);
    if (!ret || ret[0] != '(') return 0;
    char inner[600];
    snprintf(inner, sizeof(inner), "%.*s", (int)strlen(ret) - 2, ret + 1);
    snprintf(out->name, sizeof(out->name), "%s", name);
    out->count = split_types(inner, out->elems, 8);
    for (int j = 0; j < out->count; j++) {
        char* s = out->elems[j];
        while (*s == ' ') memmove(s, s + 1, strlen(s));
        size_t l = strlen(s);
        while (l > 0 && s[l - 1] == ' ') s[--l] = 0;
    }
    return out->count > 0;
}

// Resolve a call node to a tuple function and its defining
// module (written to modbuf): a plain call names a function
// of this module (this file's table first, then the
// registry for sibling files); a method call names one of
// the receiver module's functions, through the namespace or
// an instance variable alike.
static const TupleFn* resolve_tuple_call(ASTNode* call, char* modbuf, size_t modn,
                                         TupleFn* scratch) {
    if (!call) return NULL;
    if (call->type == AST_CALL) {
        const TupleFn* tf = find_tuple_fn(call->text);
        if (tf) {
            snprintf(modbuf, modn, "%s", current_module);
            return tf;
        }
        if (registry_tuple_fn(current_module, call->text, scratch)) {
            snprintf(modbuf, modn, "%s", current_module);
            return scratch;
        }
        return NULL;
    }
    if (call->type == AST_METHOD_CALL && call->child_count >= 1 &&
        call->children[0] && call->children[0]->type == AST_IDENTIFIER) {
        const char* rn = call->children[0]->text;
        const char* lt = get_local_variable_type(rn);
        const char* mod = rn;
        if (lt && strncmp(lt, "instance:", 9) == 0) mod = lt + 9;
        else if (lt && codegen_module_can_instance(lt)) mod = lt;
        if (registry_tuple_fn(mod, call->text, scratch)) {
            snprintf(modbuf, modn, "%s", mod);
            return scratch;
        }
    }
    return NULL;
}

// Emit a C function-pointer parameter: RET (*name)(ARGS) from a
// fn: type text. `name` may be NULL for prototypes. Array argument
// types map to the runtime array types, as for ordinary parameters.
static void emit_fn_param(FILE* f, const char* fn_text, const char* name) {
    const char* lp = strchr(fn_text, '(');
    const char* rp = strrchr(fn_text, ')');
    if (!lp || !rp) { fprintf(f, "void* %s", name ? name : ""); return; }
    char ret[96];
    snprintf(ret, sizeof(ret), "%.*s", (int)(lp - (fn_text + 3)), fn_text + 3);
    char retc[300];
    come_type_to_c(ret, retc, sizeof(retc));
    char inner[512];
    snprintf(inner, sizeof(inner), "%.*s", (int)(rp - lp - 1), lp + 1);
    fprintf(f, "%s (*%s)(", retc, name ? name : "");
    if (inner[0] == 0) {
        fprintf(f, "void");
    } else {
        char parts[32][160];
        int np = split_types(inner, parts, 32);
        for (int i = 0; i < np; i++) {
            if (i) fprintf(f, ", ");
            size_t L = strlen(parts[i]);
            if (L >= 2 && strcmp(parts[i] + L - 2, "[]") == 0) {
                char raw[128];
                snprintf(raw, sizeof(raw), "%.*s", (int)(L - 2), parts[i]);
                if (strcmp(raw, "int") == 0) fprintf(f, "come_int_array_t");
                else if (strcmp(raw, "byte") == 0) fprintf(f, "come_byte_array_t");
                else if (strcmp(raw, "string") == 0) fprintf(f, "come_string_list_t");
                else fprintf(f, "come_array_t");
            } else {
                char ct[300];
                come_type_to_c(parts[i], ct, sizeof(ct));
                fprintf(f, "%s", ct);
            }
        }
    }
    fprintf(f, ")");
}
// The '(' opening a fn: signature's parameter list. The list
// is the LAST balanced group in the signature: a tuple return
// type and reference parameters (&(struct T), COIL-136) put
// parentheses of their own earlier in the text.
static const char* sig_params_open(const char* csig) {
    if (!csig || strncmp(csig, "fn:", 3) != 0) return NULL;
    size_t len = strlen(csig);
    if (len == 0 || csig[len - 1] != ')') return NULL;
    int depth = 0;
    for (long i = (long)len - 1; i >= 3; i--) {
        if (csig[i] == ')') depth++;
        else if (csig[i] == '(' && --depth == 0) return csig + i;
    }
    return NULL;
}

// The declared parameter types of a same-module callee, split
// from its fn: signature. Returns the count (0 when unknown).
static int call_param_types(ASTNode* call, char parts[][160], int max_parts) {
    const char* csig = find_module_fn_sig(call->text);
    const char* q = csig ? sig_params_open(csig) : NULL;
    if (!q) return 0;
    const char* rp = csig + strlen(csig) - 1;
    char inner[512];
    snprintf(inner, sizeof(inner), "%.*s", (int)(rp - q - 1), q + 1);
    if (!inner[0]) return 0;
    return split_types(inner, parts, max_parts);
}
// Check a call's arguments against 8.3: a function name may appear
// only where the callee declares a function parameter, and then
// its signature must match the declarator exactly. Callees the
// compiler does not know (other modules, methods) are unchecked.
static void check_call_args(ASTNode* call) {
    char parts[32][160];
    int np = call_param_types(call, parts, 32);
    if (np == 0 && !find_module_fn_sig(call->text)) return;
    for (int i = 0; i < call->child_count && i < np; i++) {
        ASTNode* arg = call->children[i];
        int wants_fn = is_fn_type(parts[i]);
        const char* arg_sig = NULL;
        if (arg->type == AST_IDENTIFIER) {
            arg_sig = find_module_fn_sig(arg->text);
            if (arg_sig && get_local_variable_type(arg->text)) arg_sig = NULL; // a local shadows the function
        }
        if (wants_fn) {
            if (!arg_sig)
                codegen_error("argument %d of '%s' must be a named function matching %s",
                              i + 1, call->text, parts[i]);
            else if (strcmp(arg_sig, parts[i]) != 0)
                codegen_error("function '%s' (%s) does not match the declared signature %s for argument %d of '%s'",
                              arg->text, arg_sig, parts[i], i + 1, call->text);
        } else if (arg_sig) {
            codegen_error("function '%s' can only be passed where a function parameter is declared (spec 8.3)",
                          arg->text);
        }
    }
}
// An expression that IS a bare function name, in a value position.
static int expr_is_fn_value(ASTNode* e) {
    return e && e->type == AST_IDENTIFIER && find_module_fn_sig(e->text) &&
           !get_local_variable_type(e->text);
}

// The store check for an assignment node (identifier or array
// element target).
static void check_assign_store(ASTNode* assign) {
    if (!assign || assign->child_count < 2) return;
    ASTNode* lhs = assign->children[0];
    ASTNode* rhs = assign->children[1];
    if (expr_is_fn_value(rhs))
        codegen_error("functions can only be passed as arguments (spec 8.3): '%s' cannot be stored", rhs->text);
    // Strings are immutable (spec 6.5, COIL-110): s[i] = ... is an
    // error - s[i] is a character value, not a memory slot.
    if (lhs->type == AST_ARRAY_ACCESS && lhs->child_count >= 1 &&
        lhs->children[0]->type == AST_IDENTIFIER) {
        const char* bt = dispatch_type_of(lhs->children[0]->text);
        if (bt && strcmp(bt, "string") == 0)
            codegen_error("strings are immutable: '%s[i] = ...' is not allowed (spec 6.5) - build a new string instead",
                          lhs->children[0]->text);
    }
    if (lhs->type == AST_IDENTIFIER) {
        const char* lt = dispatch_type_of(lhs->text);
        if (lt) check_numeric_store(lt, rhs, lhs->text);
    } else if (lhs->type == AST_ARRAY_ACCESS && lhs->child_count >= 1 &&
               lhs->children[0]->type == AST_IDENTIFIER) {
        const char* bt = dispatch_type_of(lhs->children[0]->text);
        if (bt) {
            char elem[64];
            snprintf(elem, sizeof(elem), "%s", bt);
            char* br = strchr(elem, '[');
            if (br) *br = '\0';
            check_numeric_store(elem, rhs, lhs->children[0]->text);
        }
    }
}

static void expr_coarse_type(ASTNode* e, char* out, size_t n) {
    out[0] = 0;
    if (!e) return;
    switch (e->type) {
        case AST_STRING_LITERAL: snprintf(out, n, "string"); break;
        case AST_NUMBER: snprintf(out, n, "int"); break;
        case AST_BOOL_LITERAL: snprintf(out, n, "bool"); break;
        case AST_IDENTIFIER: {
            const char* t = dispatch_type_of(e->text);
            if (t) snprintf(out, n, "%s", t);
            break;
        }
        default: break;
    }
}

static void check_map_expr_type(const char* want, ASTNode* expr, const char* what) {
    char got[300];
    expr_coarse_type(expr, got, sizeof(got));
    if (got[0] == 0) return;
    if (strcmp(got, want) != 0) {
        codegen_error("map %s type mismatch: expected %s, got %s", what, want, got);
    }
}

// Is this method call one of the typed-map methods (Come_map.md
// method table) on a map{K, V} receiver? (COIL-103)
static int is_typed_map_method(ASTNode* receiver, const char* method) {
    if (!receiver || receiver->type != AST_IDENTIFIER) return 0;
    const char* t = dispatch_type_of(receiver->text);
    if (!t || strncmp(t, "map{", 4) != 0) return 0;
    return strcmp(method, "length") == 0 || strcmp(method, "has") == 0 ||
           strcmp(method, "delete") == 0 || strcmp(method, "clear") == 0 ||
           strcmp(method, "keys") == 0 || strcmp(method, "type") == 0 ||
           strcmp(method, "owner") == 0 || strcmp(method, "chown") == 0 ||
           strcmp(method, "lookup_lower") == 0 || strcmp(method, "dup") == 0;
}

// Emit an expression in a position that needs a come_string_t:
// string literals need an explicit come_string_new wrap there.
static void emit_string_expr(FILE* f, ASTNode* e) {
    if (e && e->type == AST_STRING_LITERAL) {
        fprintf(f, "come_string_new(COME_CTX, ");
        generate_expression(f, e);
        fprintf(f, ")");
    } else {
        generate_expression(f, e);
    }
}

// Is this expression statically a string value? Routes `+`
// to concatenation (spec 6.5) and string ternaries to
// materialized branches (spec 14). Deliberately conservative:
// literals, string variables, string-returning calls, and
// combinations of those.
static int expr_is_stringish(ASTNode* e) {
    if (!e) return 0;
    if (e->type == AST_STRING_LITERAL) return 1;
    if (e->type == AST_IDENTIFIER) {
        const char* t = dispatch_type_of(e->text);
        return t && strcmp(t, "string") == 0;
    }
    if (e->type == AST_BINARY_OP && strcmp(e->text, "+") == 0 && e->child_count >= 2) {
        return expr_is_stringish(e->children[0]) || expr_is_stringish(e->children[1]);
    }
    if (e->type == AST_TERNARY && e->child_count >= 3) {
        return expr_is_stringish(e->children[1]) || expr_is_stringish(e->children[2]);
    }
    if (e->type == AST_CALL) {
        const char* sig = find_module_fn_sig(e->text);
        return sig && strncmp(sig, "come_string_t", 14) == 0;
    }
    if (e->type == AST_METHOD_CALL) {
        const char* rr = method_call_module_ret(e);
        if (rr) return strcmp(rr, "string") == 0;
    }
    return 0;
}

// Emit a FILE method call (COIL-114, Come_std.md section 2):
// recv_c is the C lvalue of the FILE object; its address goes to
// the runtime. String parameters are emitted as string objects
// (the runtime reads their bytes). Returns 1 when the method is a
// FILE method, 0 otherwise.
static int emit_file_method_call(FILE* f, const char* recv_c, ASTNode* node) {
    static const char* methods[] = {
        "init", "exit", "open", "close", "fdopen", "reopen", "fileno",
        "printf", "scanf", "vprintf", "vscanf",
        "read", "write", "getc", "putc", "gets", "puts", "fname", "ungetc",
        "seek", "tell", "rewind",
        "isopen", "eof", "error", "flush", "clearerr",
        "setbuf", "setvbuf", "setlinebuf", NULL
    };
    const char* m = node->text;
    int found = 0;
    for (int i = 0; methods[i]; i++) if (strcmp(m, methods[i]) == 0) { found = 1; break; }
    if (!found) return 0;
    fprintf(f, "come_std__FILE__%s(&%s", m, recv_c);
    for (int i = 1; i < node->child_count; i++) {
        fprintf(f, ", ");
        int str_arg =
            (strcmp(m, "open") == 0 || strcmp(m, "reopen") == 0) ||
            (strcmp(m, "fdopen") == 0 && i == 2) ||
            (i == 1 && (strcmp(m, "printf") == 0 || strcmp(m, "scanf") == 0 ||
                        strcmp(m, "vprintf") == 0 || strcmp(m, "vscanf") == 0 ||
                        strcmp(m, "puts") == 0));
        if (str_arg) emit_string_expr(f, node->children[i]);
        else generate_expression(f, node->children[i]);
    }
    fprintf(f, ")");
    return 1;
}

// Auto-close for FILE variables (Come_std.md: .exit() runs
// automatically at scope end). emit_file_closes_for emits close
// calls, in reverse declaration order, for the FILE variables
// among a block's first `upto` children. generate_block_children
// is the block generator used wherever a braced block is emitted:
// it closes a block's FILE variables before any return among its
// statements and again at the block's end, so no path out of the
// block leaves a stream it opened still open.
static void emit_file_closes_for(FILE* f, ASTNode* block, int upto, int indent) {
    int std_imported = 0;
    for (int i = 0; i < current_import_count; i++)
        if (strcmp(current_imports[i], "std") == 0) { std_imported = 1; break; }
    if (!std_imported || !block) return;
    if (upto > block->child_count) upto = block->child_count;
    for (int i = upto - 1; i >= 0; i--) {
        ASTNode* ch = block->children[i];
        if (ch && ch->type == AST_VAR_DECL && ch->child_count > 1 &&
            ch->children[1] && strcmp(ch->children[1]->text, "FILE") == 0) {
            emit_indent(f, indent);
            fprintf(f, "come_std__FILE__close(&%s);\n", ch->text);
        }
    }
}

static void generate_node(FILE* f, ASTNode* node, int indent);
// ---- Clean blocks (redesign, Design 5 no-frame tier) ----
// A block whose frame would be empty may skip its push/pop:
// the frame's only job is freeing what the block creates,
// and a clean block creates nothing. The predicate mirrors
// frame_walk_clean, but runs at codegen time against the
// live local tables (the analysis tracker is stale by
// then), with one extra conservatism: a call used as a
// `+` operand is never scalar, since its return type is
// not visible here. Soundness notes:
// - Lexical depths are unchanged: scope_depth_push/pop
//   still run, so break/continue targets and variable
//   scopes resolve exactly as before; only frame
//   presence changes, and pop_to_depth pops "everything
//   above an index", which stays correct when the
//   missing frames are empty.
// - Buffer variables referenced inside the block are
//   declared outside it (a buffer declaration anywhere
//   in the subtree disqualifies), so their adoption
//   targets, at depths shallower than the block, are
//   unaffected by the missing frame.
// - A returned buffer value adopts to caller_ctx, a
//   marker-based lookup, never to a skipped block.
// - Break/continue fall in the default case and
//   disqualify, so the loop-target math never sees a
//   skipped frame on its path.
typedef struct { char name[128]; char type[128]; } CleanVar;
static CleanVar g_clean_vars[512];
static int g_clean_var_count = 0;

static const char* clean_var_get(const char* name) {
    for (int i = g_clean_var_count - 1; i >= 0; i--)
        if (strcmp(g_clean_vars[i].name, name) == 0) return g_clean_vars[i].type;
    return NULL;
}

static void clean_var_set(const char* name, const char* type) {
    if (g_clean_var_count >= 512) return;
    snprintf(g_clean_vars[g_clean_var_count].name, sizeof(g_clean_vars[0].name), "%s", name);
    snprintf(g_clean_vars[g_clean_var_count].type, sizeof(g_clean_vars[0].type), "%s", type);
    g_clean_var_count++;
}

// Identifier type: the walk's own tracker first (a
// declaration inside the block), then codegen's tables
// (enclosing locals and module variables).
static const char* clean_ident_type(const char* name) {
    const char* t = clean_var_get(name);
    if (t) return t;
    return dispatch_type_of(name);
}

static int clean_operand_scalar(ASTNode* n) {
    if (!n) return 0;
    switch (n->type) {
        case AST_NUMBER: case AST_BOOL_LITERAL: return 1;
        case AST_IDENTIFIER: return frame_scalar_type(clean_ident_type(n->text));
        case AST_ARRAY_ACCESS: {
            if (n->child_count < 1 || !n->children[0] ||
                n->children[0]->type != AST_IDENTIFIER) return 0;
            const char* t = clean_ident_type(n->children[0]->text);
            if (!t) return 0;
            if (strcmp(t, "string") == 0) return 1; // s[i] is wchar
            size_t L = strlen(t);
            if (L > 2 && strcmp(t + L - 2, "[]") == 0) {
                char base[128];
                snprintf(base, sizeof(base), "%.*s", (int)(L - 2), t);
                return frame_scalar_type(base);
            }
            return 0;
        }
        case AST_CAST:
            return n->child_count >= 1 && n->children[0] &&
                   frame_scalar_type(n->children[0]->text);
        case AST_METHOD_CALL:
            return strcmp(n->text, "length") == 0 || strcmp(n->text, "bytesize") == 0;
        case AST_TERNARY:
            return n->child_count >= 3 && clean_operand_scalar(n->children[1]) &&
                   clean_operand_scalar(n->children[2]);
        default: return 0; // calls: return type not visible here
    }
}

static int clean_walk(ASTNode* n) {
    if (!n) return 1;
    switch (n->type) {
        case AST_BLOCK:
        case AST_IF:
        case AST_ELSE:
        case AST_WHILE:
        case AST_FOR:
        case AST_DO_WHILE: {
            int save = g_clean_var_count;
            int ok = 1;
            for (int i = 0; i < n->child_count && ok; i++)
                ok = clean_walk(n->children[i]);
            g_clean_var_count = save;
            return ok;
        }
        case AST_VAR_DECL: {
            if (n->child_count < 2 || !n->children[1]) return 0;
            const char* t = n->children[1]->text;
            ASTNode* init = n->children[0];
            if (frame_scalar_type(t)) {
                clean_var_set(n->text, t);
                return clean_walk(init);
            }
            if (strcmp(t, "var") == 0) {
                if (!init) return 0;
                if (init->type == AST_NUMBER) { clean_var_set(n->text, "int"); return 1; }
                if (init->type == AST_BOOL_LITERAL) { clean_var_set(n->text, "bool"); return 1; }
                if (init->type == AST_IDENTIFIER) {
                    const char* it = clean_ident_type(init->text);
                    if (frame_scalar_type(it)) { clean_var_set(n->text, it); return 1; }
                }
                return 0;
            }
            return 0; // buffer, struct, ref, instance, unknown: needs a frame
        }
        case AST_ASSIGN: {
            if (strcmp(n->text, "=") != 0) return 0; // compound: string += allocates
            for (int i = 0; i < n->child_count; i++)
                if (!clean_walk(n->children[i])) return 0;
            return 1;
        }
        case AST_CALL: {
            if (!codegen_is_frameless(current_module, n->text)) return 0;
            for (int i = 0; i < n->child_count; i++)
                if (!clean_walk(n->children[i])) return 0;
            return 1;
        }
        case AST_METHOD_CALL: {
            // A same-module call written module.fn(...).
            if (n->child_count > 0 && n->children[0] &&
                n->children[0]->type == AST_IDENTIFIER &&
                strcmp(n->children[0]->text, current_module) == 0) {
                if (!codegen_is_frameless(current_module, n->text)) return 0;
                for (int i = 0; i < n->child_count; i++)
                    if (!clean_walk(n->children[i])) return 0;
                return 1;
            }
            if (strcmp(n->text, "length") == 0 || strcmp(n->text, "bytesize") == 0 ||
                strcmp(n->text, "owner") == 0 || strcmp(n->text, "view") == 0 ||
                strcmp(n->text, "byte_view") == 0) {
                for (int i = 0; i < n->child_count; i++)
                    if (!clean_walk(n->children[i])) return 0;
                return 1;
            }
            return 0;
        }
        case AST_BINARY_OP: {
            if (n->child_count >= 2 && strcmp(n->text, "+") == 0) {
                if (!clean_operand_scalar(n->children[0]) ||
                    !clean_operand_scalar(n->children[1]))
                    return 0;
            }
            for (int i = 0; i < n->child_count; i++)
                if (!clean_walk(n->children[i])) return 0;
            return 1;
        }
        case AST_CAST: {
            if (n->child_count < 1 || !n->children[0]) return 0;
            if (!frame_scalar_type(n->children[0]->text)) return 0;
            for (int i = 1; i < n->child_count; i++)
                if (!clean_walk(n->children[i])) return 0;
            return 1;
        }
        case AST_RETURN:
        case AST_UNARY_OP:
        case AST_POST_INC:
        case AST_POST_DEC:
        case AST_TERNARY:
        case AST_ARRAY_ACCESS:
        case AST_MEMBER_ACCESS:
        case AST_IDENTIFIER:
        case AST_NUMBER:
        case AST_BOOL_LITERAL:
            for (int i = 0; i < n->child_count; i++)
                if (!clean_walk(n->children[i])) return 0;
            return 1;
        default:
            return 0; // string/aggregate literals, break, continue,
                      // switch, spawn, yield, printf, net, ... : needs a frame
    }
}

// ---- Checkpoint blocks (parity step 4d, Design 5 checkpoint tier) ----
// A block whose allocations are all arena may use a
// checkpoint frame (bump rewind only, no talloc
// frame to allocate or free) instead of a full frame.
// Conservative predicate, mirroring clean_walk:
// scalars and transient-string concats are allowed;
// anything that could allocate talloc in the block
// (string/aggregate literals, non-transient concats,
// container stores, non-pure method calls,
// non-frameless calls, printf/spawn/yield/switch,
// break/continue) disqualifies. A transient concat
// is arena only as a VAR_DECL init or an ASSIGN RHS
// of a transient local; any other `+` disqualifies,
// except nested `+` inside an approved transient
// concat (chained concats, all arena), tracked by
// g_checkpoint_in_concat. Stores into containers
// disqualify (they may allocate in the block's
// scope); stores of arena values to outer scopes
// are already excluded by the scope-depth rule in
// the transient analysis, so a checkpoint block's
// arena values never outlive it.
static int g_checkpoint_in_concat = 0;

// 1 = an operand provably scalar (no allocation):
// number/bool literal, a scalar-typed identifier
// (via the live local table; an unregistered name
// is not scalar, conservatively), a length/bytesize
// call, or a nested scalar `+`.
static int ckpt_operand_scalar(ASTNode* n) {
    if (!n) return 0;
    if (n->type == AST_NUMBER || n->type == AST_BOOL_LITERAL) return 1;
    if (n->type == AST_IDENTIFIER) {
        const char* t = get_local_variable_type(n->text);
        return t && frame_scalar_type(t);
    }
    if (n->type == AST_METHOD_CALL &&
        (strcmp(n->text, "length") == 0 || strcmp(n->text, "bytesize") == 0)) return 1;
    if (n->type == AST_BINARY_OP && strcmp(n->text, "+") == 0 &&
        n->child_count >= 2)
        return ckpt_operand_scalar(n->children[0]) &&
               ckpt_operand_scalar(n->children[1]);
    return 0;
}

static int checkpoint_walk(ASTNode* n) {
    if (!n) return 1;
    switch (n->type) {
        case AST_BLOCK:
        case AST_IF:
        case AST_ELSE:
        case AST_WHILE:
        case AST_FOR:
        case AST_DO_WHILE: {
            for (int i = 0; i < n->child_count; i++)
                if (!checkpoint_walk(n->children[i])) return 0;
            return 1;
        }
        case AST_VAR_DECL: {
            if (n->child_count < 2 || !n->children[1]) return 0;
            const char* t = n->children[1]->text;
            ASTNode* init = n->children[0];
            if (frame_scalar_type(t)) return checkpoint_walk(init);
            if (strcmp(t, "string") == 0) {
                if (!init) return 1;
                if (init->type == AST_IDENTIFIER) return 1;
                if (init->type == AST_BINARY_OP && strcmp(init->text, "+") == 0 &&
                    codegen_transient_local(current_module, current_function_name, n->text)) {
                    int save = g_checkpoint_in_concat;
                    g_checkpoint_in_concat = 1;
                    int ok = 1;
                    for (int i = 0; i < init->child_count && ok; i++)
                        ok = checkpoint_walk(init->children[i]);
                    g_checkpoint_in_concat = save;
                    return ok;
                }
                if (init->type == AST_CALL &&
                    codegen_is_frameless(current_module, init->text)) {
                    for (int i = 0; i < init->child_count; i++)
                        if (!checkpoint_walk(init->children[i])) return 0;
                    return 1;
                }
                return 0;
            }
            if (strcmp(t, "var") == 0) {
                if (init && init->type == AST_BINARY_OP && strcmp(init->text, "+") == 0 &&
                    codegen_transient_local(current_module, current_function_name, n->text)) {
                    int save = g_checkpoint_in_concat;
                    g_checkpoint_in_concat = 1;
                    int ok = 1;
                    for (int i = 0; i < init->child_count && ok; i++)
                        ok = checkpoint_walk(init->children[i]);
                    g_checkpoint_in_concat = save;
                    return ok;
                }
                return 0;
            }
            return 0;
        }
        case AST_ASSIGN: {
            if (strcmp(n->text, "=") != 0) return 0;
            if (n->child_count >= 1 && n->children[0]->type == AST_ARRAY_ACCESS) return 0;
            if (n->child_count >= 2 && n->children[1]->type == AST_BINARY_OP &&
                strcmp(n->children[1]->text, "+") == 0 &&
                n->children[1]->child_count >= 2 &&
                !(ckpt_operand_scalar(n->children[1]->children[0]) &&
                  ckpt_operand_scalar(n->children[1]->children[1]))) {
                if (n->children[0]->type != AST_IDENTIFIER) return 0;
                if (!codegen_transient_first_assign(current_module, current_function_name,
                                                   n->children[0]->text)) return 0;
                ASTNode* rhs = n->children[1];
                int save = g_checkpoint_in_concat;
                g_checkpoint_in_concat = 1;
                int ok = 1;
                for (int i = 0; i < rhs->child_count && ok; i++)
                    ok = checkpoint_walk(rhs->children[i]);
                g_checkpoint_in_concat = save;
                return ok;
            }
            for (int i = 0; i < n->child_count; i++)
                if (!checkpoint_walk(n->children[i])) return 0;
            return 1;
        }
        case AST_CALL: {
            if (!codegen_is_frameless(current_module, n->text)) return 0;
            for (int i = 0; i < n->child_count; i++)
                if (!checkpoint_walk(n->children[i])) return 0;
            return 1;
        }
        case AST_METHOD_CALL: {
            if (n->child_count > 0 && n->children[0] &&
                n->children[0]->type == AST_IDENTIFIER &&
                strcmp(n->children[0]->text, current_module) == 0) {
                if (!codegen_is_frameless(current_module, n->text)) return 0;
                for (int i = 0; i < n->child_count; i++)
                    if (!checkpoint_walk(n->children[i])) return 0;
                return 1;
            }
            if (strcmp(n->text, "length") == 0 || strcmp(n->text, "bytesize") == 0 ||
                strcmp(n->text, "owner") == 0 || strcmp(n->text, "view") == 0 ||
                strcmp(n->text, "byte_view") == 0) {
                for (int i = 0; i < n->child_count; i++)
                    if (!checkpoint_walk(n->children[i])) return 0;
                return 1;
            }
            return 0;
        }
        case AST_BINARY_OP: {
            if (n->child_count >= 2 && strcmp(n->text, "+") == 0) {
                if (!g_checkpoint_in_concat &&
                    !(ckpt_operand_scalar(n->children[0]) &&
                      ckpt_operand_scalar(n->children[1]))) return 0;
            }
            for (int i = 0; i < n->child_count; i++)
                if (!checkpoint_walk(n->children[i])) return 0;
            return 1;
        }
        case AST_CAST: {
            if (n->child_count < 1 || !n->children[0]) return 0;
            if (!frame_scalar_type(n->children[0]->text)) return 0;
            for (int i = 1; i < n->child_count; i++)
                if (!checkpoint_walk(n->children[i])) return 0;
            return 1;
        }
        case AST_RETURN:
        case AST_UNARY_OP:
        case AST_POST_INC:
        case AST_POST_DEC:
        case AST_TERNARY:
        case AST_ARRAY_ACCESS:
        case AST_MEMBER_ACCESS:
        case AST_IDENTIFIER:
        case AST_NUMBER:
        case AST_BOOL_LITERAL:
            for (int i = 0; i < n->child_count; i++)
                if (!checkpoint_walk(n->children[i])) return 0;
            return 1;
        default:
            return 0;
    }
}

static int block_is_checkpoint(ASTNode* block) {
    if (!block || block->type != AST_BLOCK) return 0;
    g_checkpoint_in_concat = 0;
    return checkpoint_walk(block);
}

// 1 = the block's frame would be empty: skip its push/pop.
static int block_is_clean(ASTNode* block) {
    if (!block || block->type != AST_BLOCK) return 0;
    g_clean_var_count = 0;
    return clean_walk(block);
}

// Parity step 1 (parity branch): guard-dominated bounds-check
// elimination, narrow slice. A proven set of (buffer,
// index) identifier pairs threads through statement
// generation: an early-exit guard adds its pair after
// the guard statement, a positive guard adds its pair
// inside its then-branch only, and any statement
// containing an assignment or a call voids all pairs
// (conservative: it may have changed a proven buffer
// or index). Block entry saves the set and block exit
// restores it, so proofs never leak out of a block or
// into a loop body's next iteration. Function entry
// clears the set. If a proof fails for any reason,
// the access keeps its check -- a missed proof can
// only miss an optimization, never remove a
// load-bearing check.
// Unchecked region (parity exhaust): build-level
// flag (from --unchecked, extern) plus the lexical
// depth flag (set by unchecked blocks, restored
// on exit; once set inside a region it stays set
// for nested blocks). Array reads (and stores)
// emit raw while either is set -- opt-in only,
// never the default.
int g_codegen_unchecked = 0;
static int g_unchecked = 0;

typedef struct { char buf[128]; char idx[128]; } BoundProof;
static BoundProof g_bound_proofs[64];
static int g_bound_proof_count = 0;
static void bound_proof_clear(void) { g_bound_proof_count = 0; }
static void bound_proof_add(const char* buf, const char* idx) {
    for (int i = 0; i < g_bound_proof_count; i++)
        if (strcmp(g_bound_proofs[i].buf, buf) == 0 &&
            strcmp(g_bound_proofs[i].idx, idx) == 0) return;
    if (g_bound_proof_count >= 64) return;
    snprintf(g_bound_proofs[g_bound_proof_count].buf, 128, "%s", buf);
    snprintf(g_bound_proofs[g_bound_proof_count].idx, 128, "%s", idx);
    g_bound_proof_count++;
}
static int bound_proof_has(const char* buf, const char* idx) {
    for (int i = 0; i < g_bound_proof_count; i++)
        if (strcmp(g_bound_proofs[i].buf, buf) == 0 &&
            strcmp(g_bound_proofs[i].idx, idx) == 0) return 1;
    return 0;
}
static int bound_proof_save(BoundProof* dst) {
    int n = g_bound_proof_count;
    for (int i = 0; i < n; i++) dst[i] = g_bound_proofs[i];
    return n;
}
static void bound_proof_restore(BoundProof* src, int n) {
    for (int i = 0; i < n; i++) g_bound_proofs[i] = src[i];
    g_bound_proof_count = n;
}
// Is node `<buf>.length()` with an identifier receiver?
// If so, copy the receiver name to buf_out and return 1.
static int bound_len_call(ASTNode* node, char* buf_out) {
    if (!node || node->type != AST_METHOD_CALL ||
        strcmp(node->text, "length") != 0) return 0;
    if (node->child_count < 1 || !node->children[0] ||
        node->children[0]->type != AST_IDENTIFIER) return 0;
    snprintf(buf_out, 128, "%.127s", node->children[0]->text);
    return 1;
}
static int bound_is_ident(ASTNode* node, const char** name_out) {
    if (!node || node->type != AST_IDENTIFIER) return 0;
    if (name_out) *name_out = node->text;
    return 1;
}
static int bound_is_zero(ASTNode* node) {
    return node && node->type == AST_NUMBER && strcmp(node->text, "0") == 0;
}
#define BOUND_NEG 1
#define BOUND_GE_LEN 2
#define BOUND_GE_ZERO 3
#define BOUND_LT_LEN 4
// If node is a comparison proving a bound, fill idx_out
// (and buf_out for the length kinds) and return the
// kind; else 0. Operand order is normalized (both
// `i < len` and `len > i` count, and so on).
static int bound_cmp_kind(ASTNode* node, char* idx_out, char* buf_out) {
    if (!node || node->type != AST_BINARY_OP || node->child_count < 2) return 0;
    ASTNode* l = node->children[0];
    ASTNode* r = node->children[1];
    if (!l || !r) return 0;
    const char* op = node->text;
    const char* iname = NULL;
    char lbuf[128] = "", rbuf[128] = "";
    int llen = bound_len_call(l, lbuf);
    int rlen = bound_len_call(r, rbuf);
    if (strcmp(op, "<") == 0 && bound_is_ident(l, &iname) && bound_is_zero(r)) {
        snprintf(idx_out, 128, "%s", iname); return BOUND_NEG;
    }
    if (strcmp(op, ">") == 0 && bound_is_zero(l) && bound_is_ident(r, &iname)) {
        snprintf(idx_out, 128, "%s", iname); return BOUND_NEG;
    }
    if (strcmp(op, ">=") == 0 && bound_is_ident(l, &iname) && rlen) {
        snprintf(idx_out, 128, "%s", iname); snprintf(buf_out, 128, "%s", rbuf); return BOUND_GE_LEN;
    }
    if (strcmp(op, "<=") == 0 && llen && bound_is_ident(r, &iname)) {
        snprintf(idx_out, 128, "%s", iname); snprintf(buf_out, 128, "%s", lbuf); return BOUND_GE_LEN;
    }
    if (strcmp(op, ">=") == 0 && bound_is_ident(l, &iname) && bound_is_zero(r)) {
        snprintf(idx_out, 128, "%s", iname); return BOUND_GE_ZERO;
    }
    if (strcmp(op, "<=") == 0 && bound_is_zero(l) && bound_is_ident(r, &iname)) {
        snprintf(idx_out, 128, "%s", iname); return BOUND_GE_ZERO;
    }
    if (strcmp(op, "<") == 0 && bound_is_ident(l, &iname) && rlen) {
        snprintf(idx_out, 128, "%s", iname); snprintf(buf_out, 128, "%s", rbuf); return BOUND_LT_LEN;
    }
    if (strcmp(op, ">") == 0 && llen && bound_is_ident(r, &iname)) {
        snprintf(idx_out, 128, "%s", iname); snprintf(buf_out, 128, "%s", lbuf); return BOUND_LT_LEN;
    }
    return 0;
}
// Early-exit guard: `i < 0 || i >= len` (either order),
// same index, length over one buffer.
static int bound_early_exit_guard(ASTNode* cond, char* buf_out, char* idx_out) {
    if (!cond || cond->type != AST_BINARY_OP ||
        strcmp(cond->text, "||") != 0 || cond->child_count < 2) return 0;
    char idx0[128] = "", buf0[128] = "", idx1[128] = "", buf1[128] = "";
    int k0 = bound_cmp_kind(cond->children[0], idx0, buf0);
    int k1 = bound_cmp_kind(cond->children[1], idx1, buf1);
    if (k0 == 0 || k1 == 0) return 0;
    if (!((k0 == BOUND_NEG && k1 == BOUND_GE_LEN) ||
          (k0 == BOUND_GE_LEN && k1 == BOUND_NEG))) return 0;
    if (strcmp(idx0, idx1) != 0) return 0;
    snprintf(idx_out, 128, "%s", idx0);
    snprintf(buf_out, 128, "%s", k0 == BOUND_GE_LEN ? buf0 : buf1);
    return 1;
}
// Positive guard: `i >= 0 && i < len` (either order),
// same index and buffer (the length side's buffer).
static int bound_positive_guard(ASTNode* cond, char* buf_out, char* idx_out) {
    if (!cond || cond->type != AST_BINARY_OP ||
        strcmp(cond->text, "&&") != 0 || cond->child_count < 2) return 0;
    char idx0[128] = "", buf0[128] = "", idx1[128] = "", buf1[128] = "";
    int k0 = bound_cmp_kind(cond->children[0], idx0, buf0);
    int k1 = bound_cmp_kind(cond->children[1], idx1, buf1);
    if (k0 == 0 || k1 == 0) return 0;
    if (!((k0 == BOUND_GE_ZERO && k1 == BOUND_LT_LEN) ||
          (k0 == BOUND_LT_LEN && k1 == BOUND_GE_ZERO))) return 0;
    if (strcmp(idx0, idx1) != 0) return 0;
    snprintf(idx_out, 128, "%s", idx0);
    snprintf(buf_out, 128, "%s", k0 == BOUND_LT_LEN ? buf0 : buf1);
    return 1;
}
// Does this if-body unconditionally exit (a single
// return / break / continue, bare or in a block)?
static int bound_body_exits(ASTNode* body) {
    if (!body) return 0;
    if (body->type == AST_RETURN || body->type == AST_BREAK ||
        body->type == AST_CONTINUE) return 1;
    if (body->type == AST_BLOCK && body->child_count == 1) {
        ASTNode* s = body->children[0];
        if (s && (s->type == AST_RETURN || s->type == AST_BREAK ||
                  s->type == AST_CONTINUE)) return 1;
    }
    return 0;
}
// Does this subtree contain an assignment or a call?
// (Conservative void trigger: any of them may have
// changed a proven buffer or index.) POST_INC and
// POST_DEC are included: `i++` in a loop body after
// a proof was added at body top would otherwise
// leave a later read of b[i] raw past the end
// (interval-analysis soundness fix; step 2's bodies
// had the same hole for i++ inside the body).
static int bound_subtree_has_assign_or_call(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_ASSIGN || node->type == AST_CALL ||
        node->type == AST_METHOD_CALL || node->type == AST_POST_INC ||
        node->type == AST_POST_DEC) return 1;
    for (int i = 0; i < node->child_count; i++)
        if (bound_subtree_has_assign_or_call(node->children[i])) return 1;
    return 0;
}
// Is node a decimal integer literal with value >= 0?
// (Only plain digits count: hex or suffixed literals
// are refused, conservatively.)
static int bound_nonneg_number(ASTNode* node, long* val_out) {
    if (!node || node->type != AST_NUMBER) return 0;
    const char* s = node->text;
    if (!*s) return 0;
    for (const char* p = s; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    long v = 0;
    for (const char* p = s; *p; p++) {
        v = v * 10 + (*p - '0');
        if (v > 1000000000L) return 0;
    }
    if (val_out) *val_out = v;
    return 1;
}
// Counted-for pattern: init `int i = <nonneg>`
// (VAR_DECL int; the parser rejects assignment in
// init position), cond `i < len` (either order),
// update `i++` (the parser rejects assignment in
// update position too; `i--` is refused because it
// can make i negative). If all match with the same
// index, fill buf/idx and return 1.
static int bound_for_counted(ASTNode* init, ASTNode* cond, ASTNode* update, char* buf_out, char* idx_out) {
    if (!init || !cond || !update) return 0;
    char idx[128] = "";
    if (init->type == AST_VAR_DECL) {
        if (init->child_count < 2 || !init->children[1] ||
            strcmp(init->children[1]->text, "int") != 0) return 0;
        if (!init->children[0] || !bound_nonneg_number(init->children[0], NULL)) return 0;
        snprintf(idx, 128, "%.127s", init->text);
    } else {
        return 0;
    }
    char cidx[128] = "", cbuf[128] = "";
    if (bound_cmp_kind(cond, cidx, cbuf) != BOUND_LT_LEN) return 0;
    if (strcmp(cidx, idx) != 0) return 0;
    if (update->type == AST_POST_INC) {
        if (update->child_count < 1 || !update->children[0] ||
            update->children[0]->type != AST_IDENTIFIER ||
            strcmp(update->children[0]->text, idx) != 0) return 0;
    } else {
        return 0;
    }
    snprintf(idx_out, 128, "%s", idx);
    snprintf(buf_out, 128, "%s", cbuf);
    return 1;
}
// Parity step 1+2: emit one statement, then update
// the proof set (void on assign/call, add on
// early-exit guard). Shared by block children and
// loop-body children so both thread proofs the
// same way (loop bodies used to bypass block
// children entirely, letting pre-loop proofs leak
// into loop bodies -- fixed in step 2).
// Interval analysis (parity exhaust, safe slice):
// identifiers provably >= 0 at this point. A
// nonneg identifier stays nonneg through i++ and
// through i = i + <nonneg>, is gained by i =
// <nonneg constant / length / nonneg identifier>,
// and is lost by any other assignment to it and by
// i--. Calls do not touch it (Come scalars are
// values; a call cannot modify a caller's local).
// Threaded like the proof set, but NOT cleared at
// loop boundaries: nonneg is a variable property,
// and a loop's single-bound condition needs it at
// entry. Saved/restored per block so block-local
// nonneg does not leak out.
typedef struct { char name[128]; } NonnegId;
static NonnegId g_nonneg[64];
static int g_nonneg_count = 0;
static int nonneg_contains(const char* name) {
    for (int i = 0; i < g_nonneg_count; i++)
        if (strcmp(g_nonneg[i].name, name) == 0) return 1;
    return 0;
}
static void nonneg_add(const char* name) {
    if (nonneg_contains(name)) return;
    if (g_nonneg_count >= 64) return;
    snprintf(g_nonneg[g_nonneg_count].name, 128, "%s", name);
    g_nonneg_count++;
}
static void nonneg_remove(const char* name) {
    for (int i = 0; i < g_nonneg_count; i++) {
        if (strcmp(g_nonneg[i].name, name) == 0) {
            g_nonneg[i] = g_nonneg[--g_nonneg_count];
            return;
        }
    }
}
static int nonneg_save(NonnegId* dst) {
    for (int i = 0; i < g_nonneg_count; i++) dst[i] = g_nonneg[i];
    return g_nonneg_count;
}
static void nonneg_restore(NonnegId* src, int count) {
    for (int i = 0; i < count; i++) g_nonneg[i] = src[i];
    g_nonneg_count = count;
}
static int nonneg_expr(ASTNode* n) {
    if (!n) return 0;
    if (bound_nonneg_number(n, NULL)) return 1;
    if (n->type == AST_METHOD_CALL &&
        (strcmp(n->text, "length") == 0 || strcmp(n->text, "bytesize") == 0)) return 1;
    if (n->type == AST_IDENTIFIER) return nonneg_contains(n->text);
    return 0;
}
static void nonneg_update_for_stmt(ASTNode* ch) {
    if (!ch) return;
    if (ch->type == AST_VAR_DECL) {
        if (ch->child_count >= 2 && ch->children[1] &&
            strcmp(ch->children[1]->text, "int") == 0) {
            if (nonneg_expr(ch->children[0])) nonneg_add(ch->text);
            else nonneg_remove(ch->text);
        }
        return;
    }
    if (ch->type == AST_POST_DEC) {
        if (ch->child_count >= 1 && ch->children[0]->type == AST_IDENTIFIER)
            nonneg_remove(ch->children[0]->text);
        return;
    }
    if (ch->type == AST_POST_INC) return;
    if (ch->type == AST_ASSIGN && ch->child_count >= 2 &&
        ch->children[0]->type == AST_IDENTIFIER) {
        const char* tgt = ch->children[0]->text;
        ASTNode* rhs = ch->children[1];
        if (strcmp(ch->text, "=") == 0) {
            if (rhs->type == AST_BINARY_OP && strcmp(rhs->text, "+") == 0 &&
                rhs->child_count >= 2 &&
                ((rhs->children[0]->type == AST_IDENTIFIER &&
                  strcmp(rhs->children[0]->text, tgt) == 0 && nonneg_expr(rhs->children[1])) ||
                 (rhs->children[1]->type == AST_IDENTIFIER &&
                  strcmp(rhs->children[1]->text, tgt) == 0 && nonneg_expr(rhs->children[0]))))
                return;
            if (nonneg_expr(rhs)) nonneg_add(tgt);
            else nonneg_remove(tgt);
        } else if (strcmp(ch->text, "+=") == 0) {
            if (!bound_nonneg_number(rhs, NULL)) nonneg_remove(tgt);
        } else {
            nonneg_remove(tgt);
        }
        return;
    }
}
static void generate_guarded_stmt(FILE* f, ASTNode* ch, int indent) {
    generate_node(f, ch, indent);
    if (bound_subtree_has_assign_or_call(ch)) bound_proof_clear();
    nonneg_update_for_stmt(ch);
    if (ch && ch->type == AST_IF && ch->child_count == 2) {
        char buf[128], idx[128];
        if (bound_early_exit_guard(ch->children[0], buf, idx) &&
            bound_body_exits(ch->children[1]))
            bound_proof_add(buf, idx);
    }
}
static void generate_block_children(FILE* f, ASTNode* block, int indent) {
    // Parity step 1: proofs are scoped to this block.
    BoundProof saved[64];
    int saved_count = bound_proof_save(saved);
    NonnegId saved_nonneg[64];
    int saved_nonneg_count = nonneg_save(saved_nonneg);
    for (int i = 0; i < block->child_count; i++) {
        ASTNode* ch = block->children[i];
        if (ch && ch->type == AST_RETURN) emit_file_closes_for(f, block, i, indent);
        generate_guarded_stmt(f, ch, indent);
    }
    bound_proof_restore(saved, saved_count);
    nonneg_restore(saved_nonneg, saved_nonneg_count);
    // At the block's end (unless the last statement is a return:
    // the closes already ran before it).
    if (block->child_count == 0 || block->children[block->child_count - 1]->type != AST_RETURN)
        emit_file_closes_for(f, block, block->child_count, indent);
}

// Array toolbox dispatch (COIL-116, Come_array.md section 4).
static int is_array_toolbox_method(const char* m) {
    return strcmp(m, "contains") == 0 || strcmp(m, "index") == 0 ||
           strcmp(m, "append") == 0 || strcmp(m, "insert") == 0 ||
           strcmp(m, "remove") == 0 || strcmp(m, "clear") == 0 ||
           strcmp(m, "sort") == 0 || strcmp(m, "reverse") == 0 ||
           strcmp(m, "set") == 0 || strcmp(m, "cpy") == 0 ||
           strcmp(m, "dup") == 0 || strcmp(m, "type") == 0 ||
           strcmp(m, "owner") == 0 || strcmp(m, "chown") == 0;
}

static int is_array_receiver(ASTNode* receiver) {
    if (!receiver) return 0;
    char tb[160];
    const char* rt = expr_come_type(receiver, tb, sizeof(tb));
    return rt && strchr(rt, '[') != NULL;
}

// Emit one array toolbox call. The receiver is an array variable;
// COIL-135: the header lives inline in the variable, mutating
// operations take the address of the header slot, and growth never
// moves a shared data block, so no assignment back is needed - the
// expression still yields the header value where the old code did.
// COIL-157: adopt helpers defined below (before
// emit_map_put); the array toolbox uses them.
static int type_needs_adopt(const char* t);
static void emit_adopt_value(FILE* f, int indent, const char* lv,
                             const char* type, const char* dst);
static void elem_c_type(const char* elem, char* out, size_t n);

// COIL-157: an identifier or member chain re-emits
// without side effects, so it can be read again for a
// destination context. Anything else (an index, a call)
// may evaluate again, so storing methods skip adoption
// for those receivers (a documented corner).
static int expr_is_lvalue_chain(ASTNode* n) {
    if (!n) return 0;
    if (n->type == AST_IDENTIFIER) return 1;
    if (n->type == AST_MEMBER_ACCESS && n->child_count >= 1)
        return expr_is_lvalue_chain(n->children[0]);
    return 0;
}

static void emit_array_method(FILE* f, ASTNode* node, ASTNode* receiver, const char* method) {
    char rtb[160];
    const char* rt = expr_come_type(receiver, rtb, sizeof(rtb));
    char elem[64];
    const char* lb = rt ? strchr(rt, '[') : NULL;
    size_t el = lb ? (size_t)(lb - rt) : 0;
    if (el >= sizeof(elem)) el = sizeof(elem) - 1;
    memcpy(elem, rt ? rt : "", el);
    elem[el] = 0;
    // The element's C type: scalar typedefs in come_types.h make the
    // Come spelling valid C for every scalar kind.
    char et[96];
    if (strcmp(elem, "int") == 0) strcpy(et, "int");
    else if (strcmp(elem, "byte") == 0) strcpy(et, "uint8_t");
    else if (strcmp(elem, "string") == 0) strcpy(et, "come_string_t");
    else if (strcmp(elem, "bool") == 0) strcpy(et, "bool");
    else snprintf(et, sizeof(et), "%s", elem);
    ASTNode* a1 = node->child_count > 1 ? node->children[1] : NULL;
    ASTNode* a2 = node->child_count > 2 ? node->children[2] : NULL;

    if (type_is_fixed_array(rt) &&
        (strcmp(method, "append") == 0 || strcmp(method, "insert") == 0 ||
         strcmp(method, "remove") == 0 || strcmp(method, "clear") == 0 ||
         strcmp(method, "cpy") == 0)) {
        codegen_error("'%s' would change the length of fixed array '%s' (%s): fixed arrays keep their length - declare a dynamic array (T[]) instead",
                      method, receiver->text, rt);
    }

    if (strcmp(method, "type") == 0) {
        fprintf(f, "come_string_new(COME_CTX, \"array\")");
        return;
    }
    if (strcmp(method, "owner") == 0) {
        fprintf(f, "(");
        generate_expression(f, receiver);
        fprintf(f, ").ctx");
        return;
    }
    if (strcmp(method, "chown") == 0) {
        // chown(owner) re-parents to a context; chown(buffer) makes
        // the buffer a child of that buffer (an owner can be a
        // variable - the allocation tree is the model).
        const char* at = (a1 && a1->type == AST_IDENTIFIER) ? get_local_variable_type(a1->text) : NULL;
        int arg_is_buffer = at && (strchr(at, '[') != NULL || strcmp(at, "string") == 0 ||
                                   strncmp(at, "map{", 4) == 0);
        if (arg_is_buffer) {
            fprintf(f, "come_hdr_chown_to((come_hdr_t*)&(");
            generate_expression(f, receiver);
            fprintf(f, "), (const come_hdr_t*)&(");
            generate_expression(f, a1);
            fprintf(f, "))");
        } else {
            fprintf(f, "come_hdr_chown_ctx((come_hdr_t*)&(");
            generate_expression(f, receiver);
            fprintf(f, "), ");
            if (a1) generate_expression(f, a1);
            else fprintf(f, "COME_CTX");
            fprintf(f, ")");
        }
        return;
    }
    if (strcmp(method, "dup") == 0) {
        if (a1 && a1->type == AST_NUMBER && atoi(a1->text) == 0) {
            generate_expression(f, receiver);   // dup(0) shares the buffer
            return;
        }
        fprintf(f, "come_array_dup_g(COME_CTX, ");
        generate_expression(f, receiver);
        fprintf(f, ", sizeof(%s))", et);
        return;
    }
    if (strcmp(method, "clear") == 0) {
        fprintf(f, "come_array_clear_g((come_hdr_t*)&(");
        generate_expression(f, receiver);
        fprintf(f, "))");
        return;
    }
    if (strcmp(method, "reverse") == 0) {
        fprintf(f, "come_array_reverse_g((come_hdr_t*)&(");
        generate_expression(f, receiver);
        fprintf(f, "), sizeof(%s))", et);
        return;
    }
    if (strcmp(method, "remove") == 0) {
        fprintf(f, "({ come_array_remove_g((come_hdr_t*)&(");
        generate_expression(f, receiver);
        fprintf(f, "), sizeof(%s), ", et);
        if (a1) generate_expression(f, a1);
        fprintf(f, "); ");
        generate_expression(f, receiver);
        fprintf(f, "; })");
        return;
    }
    if (strcmp(method, "cpy") == 0) {
        fprintf(f, "({ come_array_cpy_g((come_hdr_t*)&(");
        generate_expression(f, receiver);
        fprintf(f, "), sizeof(%s), ", et);
        if (a1) generate_expression(f, a1);
        fprintf(f, "); ");
        generate_expression(f, receiver);
        fprintf(f, "; })");
        return;
    }
    if (strcmp(method, "sort") == 0) {
        const char* sortfn = NULL;
        if (strcmp(elem, "int") == 0) sortfn = "come_int_array_sort";
        else if (strcmp(elem, "byte") == 0) sortfn = "come_byte_array_sort";
        else if (strcmp(elem, "string") == 0) sortfn = "come_string_list_sort";
        if (!sortfn) {
            codegen_error("sort(cmp) is implemented for int, byte and string arrays, not '%s'", rt);
            fprintf(f, "0");
            return;
        }
        if (!a1 || a1->type != AST_IDENTIFIER) {
            codegen_error("sort expects a comparator function name (spec 8.3)");
            fprintf(f, "0");
            return;
        }
        const char* lt = get_local_variable_type(a1->text);
        fprintf(f, "%s((come_hdr_t*)&(", sortfn);
        generate_expression(f, receiver);
        fprintf(f, "), ");
        if (lt && is_fn_type(lt)) generate_expression(f, a1);
        else if (find_module_fn_sig(a1->text)) fprintf(f, "come_%s__%s", current_module, a1->text);
        else codegen_error("sort: '%s' is not a function of this module", a1->text);
        fprintf(f, ")");
        return;
    }
    // append / insert / set / contains / index carry an element,
    // materialized into a temp so its address can be taken.
    ASTNode* elem_expr = (strcmp(method, "insert") == 0) ? a2 : a1;
    fprintf(f, "({ %s _ae = (", et);
    if (elem_expr) {
        if (strcmp(elem, "string") == 0) emit_string_expr(f, elem_expr);
        else generate_expression(f, elem_expr);
    } else {
        fprintf(f, "0");
    }
    fprintf(f, "); ");
    // COIL-157: a stored element moves to the receiver's
    // context, or it dies when the storing scope exits
    // while the array lives on. Reads (contains/index)
    // never store, so they never adopt.
    int adopt_elem = (strcmp(method, "append") == 0 || strcmp(method, "insert") == 0 ||
                      strcmp(method, "set") == 0) &&
                     type_needs_adopt(elem) && expr_is_lvalue_chain(receiver) &&
                     // Step 4c-arrays: a transient container
                     // never escapes, so its elements never
                     // need eager promotion on store.
                     !(receiver->type == AST_IDENTIFIER &&
                       codegen_transient_container(current_module, current_function_name,
                                                   receiver->text));
    if (adopt_elem) {
        fprintf(f, "void* _actx = (");
        generate_expression(f, receiver);
        fprintf(f, ").ctx; ");
        emit_adopt_value(f, 0, "_ae", elem, "_actx");
        fprintf(f, " ");
    }
    if (strcmp(method, "append") == 0) {
        fprintf(f, "come_array_append_g((come_hdr_t*)&(");
        generate_expression(f, receiver);
        fprintf(f, "), sizeof(%s), &_ae); ", et);
        generate_expression(f, receiver);
        fprintf(f, "; })");
    } else if (strcmp(method, "insert") == 0) {
        fprintf(f, "come_array_insert_g((come_hdr_t*)&(");
        generate_expression(f, receiver);
        fprintf(f, "), sizeof(%s), ", et);
        if (a1) generate_expression(f, a1);
        fprintf(f, ", &_ae); ");
        generate_expression(f, receiver);
        fprintf(f, "; })");
    } else if (strcmp(method, "set") == 0) {
        fprintf(f, "come_array_set_g((come_hdr_t*)&(");
        generate_expression(f, receiver);
        fprintf(f, "), sizeof(%s), &_ae); })", et);
    } else if (strcmp(elem, "string") == 0) {   // contains / index on strings compare contents
        fprintf(f, "come_array_%s_string(", method);
        generate_expression(f, receiver);
        fprintf(f, ", _ae); })");
    } else {   // contains / index: reads take the header by value
        fprintf(f, "come_array_%s_g(", method);
        generate_expression(f, receiver);
        fprintf(f, ", sizeof(%s), &_ae); })", et);
    }
}

// String methods whose runtime parameters are C strings
// (COIL-117): the runtime takes `const char*` for separators,
// patterns and cutsets, so a string argument must be emitted as
// its byte buffer, not as the string object. (Passing the object
// was the long-standing split/join-with-variable-separator
// crash.) These methods are emitted here in full.
static int is_cstr_param_string_method(const char* m) {
    return strcmp(m, "find") == 0 || strcmp(m, "rfind") == 0 ||
           strcmp(m, "count") == 0 || strcmp(m, "replace") == 0 ||
           strcmp(m, "trim") == 0 || strcmp(m, "ltrim") == 0 ||
           strcmp(m, "rtrim") == 0 || strcmp(m, "split") == 0 ||
           strcmp(m, "split_n") == 0 || strcmp(m, "regex") == 0 ||
           strcmp(m, "regex_split") == 0 || strcmp(m, "regex_groups") == 0 ||
           strcmp(m, "regex_replace") == 0;
}

static void emit_string_obj(FILE* f, ASTNode* e) {
    if (e && e->type == AST_STRING_LITERAL) {
        fprintf(f, "come_string_new(COME_CTX, ");
        generate_expression(f, e);
        fprintf(f, ")");
    } else {
        generate_expression(f, e);
    }
}

static void emit_cstr_arg(FILE* f, ASTNode* e) {
    if (!e) { fprintf(f, "NULL"); return; }
    if (e->type == AST_STRING_LITERAL) { generate_expression(f, e); return; }
    fprintf(f, "((");
    generate_expression(f, e);
    fprintf(f, ").data ? (");
    generate_expression(f, e);
    fprintf(f, ").data : \"\")");
}

static void emit_cstr_string_method(FILE* f, ASTNode* node, const char* method) {
    ASTNode* receiver = node->children[0];
    ASTNode* a1 = node->child_count > 1 ? node->children[1] : NULL;
    ASTNode* a2 = node->child_count > 2 ? node->children[2] : NULL;
    ASTNode* a3 = node->child_count > 3 ? node->children[3] : NULL;
    fprintf(f, "come_string_%s(", method);
    emit_string_obj(f, receiver);
    if (strcmp(method, "trim") == 0 || strcmp(method, "ltrim") == 0 || strcmp(method, "rtrim") == 0) {
        fprintf(f, ", ");
        if (a1) emit_cstr_arg(f, a1);
        else fprintf(f, "NULL");
    } else if (strcmp(method, "find") == 0 || strcmp(method, "rfind") == 0 ||
               strcmp(method, "count") == 0 || strcmp(method, "split") == 0 ||
               strcmp(method, "regex") == 0 || strcmp(method, "regex_groups") == 0) {
        fprintf(f, ", ");
        emit_cstr_arg(f, a1);
    } else if (strcmp(method, "split_n") == 0 || strcmp(method, "regex_split") == 0) {
        fprintf(f, ", ");
        emit_cstr_arg(f, a1);
        fprintf(f, ", ");
        if (a2) generate_expression(f, a2);
        else fprintf(f, "0");
    } else if (strcmp(method, "replace") == 0) {
        fprintf(f, ", ");
        emit_cstr_arg(f, a1);
        fprintf(f, ", ");
        emit_cstr_arg(f, a2);
        fprintf(f, ", ");
        if (a3) generate_expression(f, a3);
        else fprintf(f, "0");
    } else if (strcmp(method, "regex_replace") == 0) {
        fprintf(f, ", ");
        emit_cstr_arg(f, a1);
        fprintf(f, ", ");
        emit_cstr_arg(f, a2);
        fprintf(f, ", ");
        if (a3) generate_expression(f, a3);
        else fprintf(f, "0");
    }
    fprintf(f, ")");
}

// Emit the map operand of a store as an address: through the
// base expression when one is given (a module variable in an
// instance-form body is (__inst->x), not a C global of that
// name), else the plain variable name (COIL-142).
static void emit_map_base_ref(FILE* f, ASTNode* base, const char* name) {
    if (base) {
        fprintf(f, "&(");
        generate_expression(f, base);
        fprintf(f, ")");
    } else {
        fprintf(f, "&%s", name);
    }
}

// Emit one map store statement (ends with a newline). Int values are
// boxed in a cell owned by the current context; string values are
// stored as the string object itself.
// ---- COIL-157: scope adoption ----
// Emit statements that move a value's buffers to a
// longer-lived scope (dst, a C expression for a context).
// lv is a pure C lvalue (identifier, member chain, or a
// temp); type is Come type text. Scalars, references
// (non-owning), and links need nothing. Arrays adopt
// their data block and, for buffer elements, each
// element's data. Maps adopt the entries block, every
// key's data, and every value (boxed ints move as
// chunks; boxed string headers move their data too).
// Structs recurse per field. A value whose buffers are
// already owned outside every scope (module or instance
// context, another buffer) is left alone by the runtime's
// ancestor check, so adoption is safe to emit wherever
// a value may cross into a longer-lived scope.
static int g_adopt_tmp = 0;

static int adopt_struct_def(const char* sname, char* def, size_t n) {
    // The driver registry: fields serialized as
    // "name\x1ftype" pairs joined by ';' (COIL-146).
    for (int m = 0; m < g_module_struct_modules; m++) {
        for (int i = 0; i < 64; i++) {
            const char* nm = NULL; const char* df = NULL;
            if (!codegen_module_struct_at(g_module_structs[m].module, i, &nm, &df, NULL, NULL)) break;
            if (nm && strcmp(nm, sname) == 0 && df) {
                snprintf(def, n, "%s", df);
                return 1;
            }
        }
    }
    return 0;
}

static void emit_adopt_value(FILE* f, int indent, const char* lv,
                             const char* type, const char* dst);

static void emit_adopt_struct(FILE* f, int indent, const char* lv,
                              const char* sname, const char* dst) {
    char def[2048];
    if (adopt_struct_def(sname, def, sizeof(def))) {
        char* p = def;
        while (*p) {
            char* semi = strchr(p, ';');
            if (semi) *semi = 0;
            char* sep = strchr(p, '\x1f');
            if (sep) {
                *sep = 0;
                // Sized past the longest field path the
                // compiler can construct (a 2048-byte
                // lvalue plus a field name).
                char flv[4100];
                snprintf(flv, sizeof(flv), "(%s).%s", lv, p);
                emit_adopt_value(f, indent, flv, sep + 1, dst);
            }
            if (!semi) break;
            p = semi + 1;
        }
        return;
    }
    // Same-module structs in this file (collect_structs):
    int found = 0;
    for (int i = 0; i < g_sfield_count; i++) {
        if (strcmp(g_sfield_struct[i], sname) == 0) {
            found = 1;
            char flv[512];
            snprintf(flv, sizeof(flv), "(%s).%s", lv, g_sfield_name[i]);
            emit_adopt_value(f, indent, flv, g_sfield_type[i], dst);
        }
    }
    if (!found) {
        codegen_error("cannot move struct '%s' across a scope boundary: its fields are not registered - "
                      "a buffer it holds would die with its scope", sname);
    }
}

static void emit_adopt_value(FILE* f, int indent, const char* lv,
                             const char* type, const char* dst) {
    if (!type || !*type || !lv || !*lv || !dst || !*dst) return;
    if (is_ref_type(type)) return; // non-owning: never adopt
    if (strcmp(type, "link") == 0) return;
    if (strcmp(type, "string") == 0) {
        emit_indent(f, indent);
        // Design 2 (redesign): an arena string reaching
        // adoption is promoted to a talloc copy instead
        // of re-parented (an arena pointer is not a
        // talloc chunk, so adopting it would corrupt).
        fprintf(f, "if (come_arena_contains((%s).data)) (%s) = come_string_promote((%s), %s); else if (come_scope_adopt(%s, (%s).data)) (%s).ctx = %s;\n", lv, lv, lv, dst, dst, lv, lv, dst);
        return;
    }
    if (type_is_array(type)) {
        char base[160];
        snprintf(base, sizeof(base), "%s", type);
        char* lb = strrchr(base, '[');
        if (lb) *lb = 0;
        if (type_is_fixed_array(type)) {
            if (!is_ref_type(base) && strcmp(base, "link") != 0 &&
                (strcmp(base, "string") == 0 || type_is_array(base) ||
                 strncmp(base, "map{", 4) == 0 || strncmp(base, "struct ", 7) == 0)) {
                int n = type_fixed_size(type);
                int t = g_adopt_tmp++;
                emit_indent(f, indent);
                fprintf(f, "for (uint32_t _ca%d = 0; _ca%d < %d; _ca%d++) {\n", t, t, n, t);
                char elv[512];
                snprintf(elv, sizeof(elv), "(%s)[_ca%d]", lv, t);
                emit_adopt_value(f, indent + 4, elv, base, dst);
                emit_indent(f, indent);
                fprintf(f, "}\n");
            }
            return;
        }
        // Dynamic array: the data block, then each buffer
        // element's own data (elements are headers inline
        // in the block, so they move with it; their data
        // does not).
        emit_indent(f, indent);
        fprintf(f, "if (come_scope_adopt(%s, (%s).data)) (%s).ctx = %s;\n", dst, lv, lv, dst);
        if (strcmp(base, "string") == 0 || type_is_array(base) ||
            strncmp(base, "map{", 4) == 0 || strncmp(base, "struct ", 7) == 0) {
            char ect[300];
            come_type_to_c(base, ect, sizeof(ect));
            int t = g_adopt_tmp++;
            emit_indent(f, indent);
            fprintf(f, "if ((%s).data) for (uint32_t _ca%d = 0; _ca%d < (%s).count; _ca%d++) {\n",
                    lv, t, t, lv, t);
            char elv[512];
            snprintf(elv, sizeof(elv), "((%s*)(%s).data)[_ca%d]", ect, lv, t);
            emit_adopt_value(f, indent + 4, elv, base, dst);
            emit_indent(f, indent);
            fprintf(f, "}\n");
        }
        return;
    }
    if (strncmp(type, "map{", 4) == 0) {
        char mk[160], mv[160];
        if (!map_type_parts(type, mk, sizeof(mk), mv, sizeof(mv))) return;
        int t = g_adopt_tmp++;
        emit_indent(f, indent);
        fprintf(f, "if ((%s).data) {\n", lv);
        emit_indent(f, indent + 4);
        fprintf(f, "if (come_scope_adopt(%s, (%s).data)) (%s).ctx = %s;\n", dst, lv, lv, dst);
        emit_indent(f, indent + 4);
        fprintf(f, "for (uint32_t _ca%d = 0; _ca%d < (%s).cap; _ca%d++) {\n", t, t, lv, t);
        emit_indent(f, indent + 8);
        fprintf(f, "if ((%s).data[_ca%d].occupied) {\n", lv, t);
        emit_indent(f, indent + 12);
        fprintf(f, "if (come_scope_adopt(%s, (%s).data[_ca%d].key.data)) (%s).data[_ca%d].key.ctx = %s;\n",
                dst, lv, t, lv, t, dst);
        if (strcmp(mv, "string") == 0) {
            emit_indent(f, indent + 12);
            // The value cell itself is a talloc chunk in
            // the source scope too: adopting only its
            // bytes leaves the cell to die with the
            // frame, and the entry still points at it
            // (use-after-free on a returned map -- found
            // by the 4c-maps fixture under ASan).
            fprintf(f, "if ((%s).data[_ca%d].value) { come_string_t* _cv = (come_string_t*)(%s).data[_ca%d].value; if (come_scope_adopt(%s, _cv->data)) _cv->ctx = %s; come_scope_adopt(%s, _cv); }\n",
                    lv, t, lv, t, dst, dst, dst);
        } else {
            emit_indent(f, indent + 12);
            fprintf(f, "come_scope_adopt(%s, (%s).data[_ca%d].value);\n", dst, lv, t);
        }
        emit_indent(f, indent + 8);
        fprintf(f, "}\n");
        emit_indent(f, indent + 4);
        fprintf(f, "}\n");
        emit_indent(f, indent);
        fprintf(f, "}\n");
        return;
    }
    if (strncmp(type, "struct ", 7) == 0) {
        emit_adopt_struct(f, indent, lv, type + 7, dst);
        return;
    }
    // Scalars, FILE handles, and anything else: no buffers.
}

// Does a value of this Come type own buffers that
// adoption must move? Mirrors emit_adopt_value's
// dispatch; scalars, references, and links own none.
static int type_needs_adopt(const char* t) {
    if (!t || !*t) return 0;
    if (is_ref_type(t) || strcmp(t, "link") == 0) return 0;
    if (strcmp(t, "string") == 0) return 1;
    if (strncmp(t, "map{", 4) == 0) return 1;
    if (strncmp(t, "struct ", 7) == 0) return 1;
    if (type_is_array(t)) {
        if (type_is_fixed_array(t)) {
            char base[160];
            snprintf(base, sizeof(base), "%s", t);
            char* lb = strrchr(base, '[');
            if (lb) *lb = 0;
            return type_needs_adopt(base);
        }
        return 1;
    }
    return 0;
}

// The scope context a variable lives in, as C text: a
// local's recorded depth, the module context for a
// module variable, the instance context for a field in
// an instance-form body.
static void dst_scope_text(const char* name, char* buf, size_t n) {
    if (g_fn_instance_form && is_module_var(name) &&
        !get_local_variable_type(name)) {
        snprintf(buf, n, "__inst->ctx");
        return;
    }
    if (get_local_variable_type(name)) {
        snprintf(buf, n, "come_scope_ctx_for_depth(%d)", get_local_variable_scope(name));
        return;
    }
    snprintf(buf, n, "come_%s__ctx", current_module);
}

// The array node an LHS is rooted at (itself for an
// index store, found down a member chain), or NULL.
static ASTNode* lhs_array_node(ASTNode* node) {
    while (node) {
        if (node->type == AST_ARRAY_ACCESS) return node;
        if (node->type == AST_MEMBER_ACCESS && node->child_count >= 1) {
            node = node->children[0];
            continue;
        }
        return NULL;
    }
    return NULL;
}

// Resolve an LHS's Come type: an identifier's recorded
// type (local, else module variable), a member chain
// resolved field by field, an index resolved to its
// element or map value type. NULL when unknowable.
static const char* resolve_lhs_type(ASTNode* lhs, char* buf, size_t n) {
    if (lhs->type == AST_IDENTIFIER) {
        const char* t = get_local_variable_type(lhs->text);
        if (!t) {
            const char* rt = NULL;
            if (codegen_module_var_index(current_module, lhs->text, &rt) >= 0) t = rt;
        }
        if (t) { snprintf(buf, n, "%s", t); return buf; }
        return NULL;
    }
    ASTNode* path[64];
    int depth = 0;
    ASTNode* cur = lhs;
    while (cur && depth < 64) {
        path[depth++] = cur;
        if (cur->type == AST_MEMBER_ACCESS && cur->child_count >= 1) { cur = cur->children[0]; continue; }
        if (cur->type == AST_ARRAY_ACCESS && cur->child_count >= 2) { cur = cur->children[0]; continue; }
        break;
    }
    if (!cur || cur->type != AST_IDENTIFIER) return NULL;
    const char* t = get_local_variable_type(cur->text);
    if (!t) {
        const char* rt = NULL;
        if (codegen_module_var_index(current_module, cur->text, &rt) >= 0) t = rt;
    }
    if (!t) return NULL;
    char cur_t[300];
    snprintf(cur_t, sizeof(cur_t), "%s", t);
    for (int i = depth - 2; i >= 0; i--) {
        ASTNode* step = path[i];
        char ib[300];
        const char* dt = deref_ref_type(cur_t, ib, sizeof(ib));
        if (step->type == AST_MEMBER_ACCESS) {
            if (strncmp(dt, "struct ", 7) != 0) return NULL;
            const char* ft = struct_field_type(dt + 7, step->text);
            if (!ft) return NULL;
            snprintf(cur_t, sizeof(cur_t), "%s", ft);
        } else if (step->type == AST_ARRAY_ACCESS) {
            if (strncmp(dt, "map{", 4) == 0) {
                char mk[160], mv[160];
                if (!map_type_parts(dt, mk, sizeof(mk), mv, sizeof(mv))) return NULL;
                snprintf(cur_t, sizeof(cur_t), "%s", mv);
            } else if (type_is_array(dt)) {
                char* lb = strrchr(cur_t, '[');
                if (!lb) return NULL;
                *lb = 0;
            } else return NULL;
        } else return NULL;
    }
    snprintf(buf, n, "%s", cur_t);
    return buf;
}

// Emit an LHS with one node replaced by text: for an
// array-rooted store, the array header becomes a temp
// so the index is evaluated once and the destination
// scope is the temp's ctx.
static void emit_lhs_rerooted(FILE* f, ASTNode* node, ASTNode* target, const char* repl) {
    if (node == target) { fprintf(f, "%s", repl); return; }
    if (node->type == AST_MEMBER_ACCESS && node->child_count >= 1) {
        emit_lhs_rerooted(f, node->children[0], target, repl);
        fprintf(f, ".%s", node->text);
        return;
    }
    if (node->type == AST_ARRAY_ACCESS && node->child_count >= 2) {
        // Unchecked store: raw lvalue (no bounds
        // check), mirroring the unchecked read.
        if (g_unchecked) {
            char bb2[300];
            const char* bt2 = resolve_lhs_type(node->children[0], bb2, sizeof(bb2));
            char et2[96] = "int";
            if (bt2) {
                const char* lb2 = strchr(bt2, '[');
                if (lb2 && (size_t)(lb2 - bt2) < 64) {
                    char elem2[64];
                    memcpy(elem2, bt2, (size_t)(lb2 - bt2));
                    elem2[lb2 - bt2] = 0;
                    elem_c_type(elem2, et2, sizeof(et2));
                }
            }
            fprintf(f, "((%s*)(%s.data))[", et2, repl);
            generate_expression(f, node->children[1]);
            fprintf(f, "]");
            return;
        }
        // Array element access is COME_ARR_GET_T, not a
        // subscript of the header (COIL-135): the macro
        // reads the header's data, so it stays a valid
        // lvalue with the temp header in place.
        char bb[300];
        const char* bt = resolve_lhs_type(node->children[0], bb, sizeof(bb));
        char et[96] = "int";
        if (bt) {
            const char* lb = strchr(bt, '[');
            if (lb && (size_t)(lb - bt) < 64) {
                char elem[64];
                memcpy(elem, bt, (size_t)(lb - bt));
                elem[lb - bt] = 0;
                elem_c_type(elem, et, sizeof(et));
            }
        }
        fprintf(f, "COME_ARR_GET_T(");
        emit_lhs_rerooted(f, node->children[0], target, repl);
        fprintf(f, ", ");
        generate_expression(f, node->children[1]);
        fprintf(f, ", %s)", et);
        return;
    }
    generate_expression(f, node);
}

// The context a map's entries live in: the map header's
// ctx. The base is always identifier-pure at the call
// sites (map index writes require an identifier base;
// literal paths pass NULL), so re-emitting it is safe.
static void emit_map_ctx(FILE* f, ASTNode* base, const char* map_var) {
    if (base) {
        fprintf(f, "(");
        generate_expression(f, base);
        fprintf(f, ").ctx");
    } else {
        fprintf(f, "%s.ctx", map_var);
    }
}

static void emit_map_put(FILE* f, ASTNode* base, const char* map_var, const char* vtype, ASTNode* key_expr, ASTNode* val_expr) {
    // Step 4c-maps: a transient map never escapes,
    // so its keys/values never need eager promotion
    // on put. Literal construction (base == NULL)
    // builds a temporary that is then adopted, so
    // it always promotes -- only named-local index
    // stores suppress.
    int transient_map = (base != NULL && map_var &&
        codegen_transient_container(current_module, current_function_name, map_var));
    // COIL-157: entries live in the map's own context.
    // A map built in a loop and kept (or returned) keeps
    // its entries; a map dropped with its scope frees
    // them with it. Keys are adopted to the map's ctx
    // too, or an entry's key would dangle when the
    // storing scope exits.
    if (strcmp(vtype, "int") == 0) {
        fprintf(f, "{ int* _vp = (int*)mem_talloc_alloc(");
        emit_map_ctx(f, base, map_var);
        fprintf(f, ", sizeof(int)); *_vp = (");
        generate_expression(f, val_expr);
        fprintf(f, "); { come_string_t _ck = ");
        emit_string_expr(f, key_expr);
        fprintf(f, ";");
        // Design 2 (redesign): promote an arena key
        // instead of adopting it (see emit_adopt_value).
        if (!transient_map) {
            fprintf(f, " if (come_arena_contains(_ck.data)) _ck = come_string_promote(_ck, ");
            emit_map_ctx(f, base, map_var);
            fprintf(f, "); else if (come_scope_adopt(");
            emit_map_ctx(f, base, map_var);
            fprintf(f, ", _ck.data)) _ck.ctx = ");
            emit_map_ctx(f, base, map_var);
            fprintf(f, ";");
        }
        fprintf(f, " come_map_put(");
        emit_map_base_ref(f, base, map_var);
        fprintf(f, ", _ck, _vp); } }\n");
    } else if (strcmp(vtype, "string") == 0) {
        // String values are boxed in a cell too (COIL-135): the
        // stored value is a header value, not a pointer.
        fprintf(f, "{ come_string_t _cv = ");
        emit_string_expr(f, val_expr);
        fprintf(f, ";");
        // Design 2 (redesign): promote an arena value
        // instead of adopting it (see emit_adopt_value).
        if (!transient_map) {
            fprintf(f, " if (come_arena_contains(_cv.data)) _cv = come_string_promote(_cv, ");
            emit_map_ctx(f, base, map_var);
            fprintf(f, "); else if (come_scope_adopt(");
            emit_map_ctx(f, base, map_var);
            fprintf(f, ", _cv.data)) _cv.ctx = ");
            emit_map_ctx(f, base, map_var);
            fprintf(f, ";");
        }
        fprintf(f, " come_string_t* _vp = (come_string_t*)mem_talloc_alloc(");
        emit_map_ctx(f, base, map_var);
        fprintf(f, ", sizeof(come_string_t)); *_vp = _cv; { come_string_t _ck = ");
        emit_string_expr(f, key_expr);
        fprintf(f, ";");
        if (!transient_map) {
            fprintf(f, " if (come_scope_adopt(");
            emit_map_ctx(f, base, map_var);
            fprintf(f, ", _ck.data)) _ck.ctx = ");
            emit_map_ctx(f, base, map_var);
            fprintf(f, ";");
        }
        fprintf(f, " come_map_put(");
        emit_map_base_ref(f, base, map_var);
        fprintf(f, ", _ck, _vp); } }\n");
    } else {
        fprintf(f, "come_map_put(");
        emit_map_base_ref(f, base, map_var);
        fprintf(f, ", ");
        emit_string_expr(f, key_expr);
        fprintf(f, ", ");
        emit_string_expr(f, val_expr);
        fprintf(f, ");\n");
    }
}

// The Come type of an expression, for the cases codegen can
// track (COIL-135): locals, struct fields, module function calls,
// and the string-list-producing methods. Returns NULL when the
// type is not knowable here.
static const char* expr_come_type(ASTNode* e, char* buf, size_t n) {
    if (!e) return NULL;
    if (e->type == AST_IDENTIFIER) return dispatch_type_of(e->text);
    if (e->type == AST_MEMBER_ACCESS && e->child_count >= 1 &&
        e->children[0]->type == AST_IDENTIFIER) {
        const char* ot = get_local_variable_type(e->children[0]->text);
        if (ot) {
            char ib[300];
            ot = deref_ref_type(ot, ib, sizeof(ib));
            if (strncmp(ot, "struct ", 7) == 0) ot += 7;
            const char* ft = struct_field_type(ot, e->text);
            if (ft) return ft;
        }
        return NULL;
    }
    if (e->type == AST_CALL) {
        const char* nm = strrchr(e->text, '.');
        nm = nm ? nm + 1 : e->text;
        const char* sig = find_module_fn_sig(nm);
        if (sig && strncmp(sig, "fn:", 3) == 0) {
            const char* lp = sig_params_open(sig);
            if (lp && lp > sig + 3) {
                size_t rl = (size_t)(lp - (sig + 3));
                if (rl >= n) rl = n - 1;
                memcpy(buf, sig + 3, rl);
                buf[rl] = 0;
                return buf;
            }
        }
        return NULL;
    }
    if (e->type == AST_AGGREGATE_INIT && strncmp(e->text, "struct ", 7) == 0) {
        // A typed struct literal (COIL-136).
        snprintf(buf, n, "%s", e->text);
        return buf;
    }
    if (e->type == AST_BINARY_OP && e->child_count >= 2 &&
        strcmp(e->text, "+") == 0 &&
        (expr_is_stringish(e->children[0]) ||
         expr_is_stringish(e->children[1]))) {
        // Design 2 (redesign): a string concat expression
        // has type string, so .length() and friends on a
        // concat temporary resolve to the string forms
        // instead of the list forms.
        snprintf(buf, n, "string");
        return buf;
    }
    if (e->type == AST_METHOD_CALL && e->child_count >= 1) {
        const char* m = e->text;
        if (strcmp(m, "get") == 0) {
            // A legacy map's get() yields a string value.
            char rb[160];
            const char* rct = expr_come_type(e->children[0], rb, sizeof(rb));
            if (rct && strcmp(rct, "map") == 0) { snprintf(buf, n, "string"); return buf; }
        }
        if (strcmp(m, "split") == 0 || strcmp(m, "split_n") == 0 ||
            strcmp(m, "regex_split") == 0 || strcmp(m, "regex_groups") == 0 ||
            strcmp(m, "keys") == 0) {
            snprintf(buf, n, "string[]");
            return buf;
        }
        if (strcmp(m, "view") == 0) {
            snprintf(buf, n, "string");
            return buf;
        }
        if (strcmp(m, "byte_view") == 0) {
            snprintf(buf, n, "byte[]");
            return buf;
        }
        if (strcmp(m, "slice") == 0 || strcmp(m, "dup") == 0)
            return expr_come_type(e->children[0], buf, n);
        return NULL;
    }
    if (e->type == AST_ARRAY_ACCESS && e->child_count >= 1) {
        const char* bt2 = expr_come_type(e->children[0], buf, n);
        if (bt2 && strncmp(bt2, "map{", 4) == 0) {
            // A typed map read yields the value type.
            char mk[160], mv[160];
            if (map_type_parts(bt2, mk, sizeof(mk), mv, sizeof(mv))) {
                snprintf(buf, n, "%s", mv);
                return buf;
            }
            return NULL;
        }
        if (bt2) {
            size_t bl = strlen(bt2);
            if (bl > 2 && strcmp(bt2 + bl - 2, "[]") == 0) {
                // Indexing strips exactly one []: T[] -> T,
                // T[][] -> T[].
                snprintf(buf, n, "%.*s", (int)(bl - 2), bt2);
                return buf;
            }
        }
        return NULL;
    }
    return NULL;
}

// Map a Come element type to its C type text.
static void elem_c_type(const char* elem, char* out, size_t n) {
    if (strcmp(elem, "int") == 0) snprintf(out, n, "int");
    else if (strcmp(elem, "byte") == 0) snprintf(out, n, "uint8_t");
    else if (strcmp(elem, "string") == 0) snprintf(out, n, "come_string_t");
    else if (strcmp(elem, "bool") == 0) snprintf(out, n, "bool");
    else snprintf(out, n, "%s", elem);
}

static const char* array_literal_elem_type(ASTNode* agg, char* buf, size_t n);

static int numeric_rank(const char* t) {
    return strcmp(t, "int") == 0 ? 1 : strcmp(t, "long") == 0 ? 2 :
           strcmp(t, "double") == 0 ? 3 : 0;
}

// The type of one array-literal element (COIL-145): literal
// kinds directly, arithmetic by promotion over its operands,
// anything else through expr_come_type. NULL when the type
// is not knowable here.
static const char* literal_expr_type(ASTNode* c, char* buf, size_t n) {
    if (c->type == AST_STRING_LITERAL) { snprintf(buf, n, "string"); return buf; }
    if (c->type == AST_BOOL_LITERAL) { snprintf(buf, n, "bool"); return buf; }
    if (c->type == AST_NUMBER) {
        if (strchr(c->text, '.') || strchr(c->text, 'e') || strchr(c->text, 'E') ||
            strchr(c->text, 'f') || strchr(c->text, 'F')) snprintf(buf, n, "double");
        else if (strchr(c->text, 'l') || strchr(c->text, 'L')) snprintf(buf, n, "long");
        else snprintf(buf, n, "int");
        return buf;
    }
    if (c->type == AST_AGGREGATE_INIT && strncmp(c->text, "struct ", 7) != 0 &&
        !(c->child_count > 0 && c->children[0]->type == AST_MAP_PAIR)) {
        // A nested array literal: T[] of the inner element.
        char ib[160];
        const char* inner = array_literal_elem_type(c, ib, sizeof(ib));
        if (!inner) return NULL;
        snprintf(buf, n, "%s[]", inner);
        return buf;
    }
    if (c->type == AST_BINARY_OP && c->child_count >= 2) {
        char lb[160], rb[160];
        const char* lt = literal_expr_type(c->children[0], lb, sizeof(lb));
        const char* rt = literal_expr_type(c->children[1], rb, sizeof(rb));
        if (!lt || !rt) return NULL;
        int rl = numeric_rank(lt), rr = numeric_rank(rt);
        if (!rl || !rr) return NULL;
        snprintf(buf, n, "%s", rr > rl ? rt : lt);
        return buf;
    }
    if ((c->type == AST_UNARY_OP || c->type == AST_POST_INC || c->type == AST_POST_DEC) &&
        c->child_count >= 1) {
        return literal_expr_type(c->children[0], buf, n);
    }
    return expr_come_type(c, buf, n);
}

// The element type of an array literal: every element must
// agree, with numeric promotion (int < long < double).
static const char* array_literal_elem_type(ASTNode* agg, char* buf, size_t n) {
    buf[0] = 0;
    for (int i = 0; i < agg->child_count; i++) {
        char cb[160];
        const char* ct = literal_expr_type(agg->children[i], cb, sizeof(cb));
        if (!ct) return NULL;
        if (!buf[0]) {
            snprintf(buf, n, "%s", ct);
        } else if (strcmp(buf, ct) != 0) {
            int rb = numeric_rank(buf), rc = numeric_rank(ct);
            if (!rb || !rc) return NULL;
            if (rc > rb) snprintf(buf, n, "%s", ct);
        }
    }
    return buf[0] ? buf : NULL;
}

// Emit an array literal as a value (COIL-145): a statement
// expression that builds the header - the same construction
// the declaration path performs, packaged as an expression
// so a literal can be passed to a call, assigned, or
// returned. String elements are appended as owned strings,
// like the string[] declaration.
static void emit_array_literal_expr(FILE* f, ASTNode* agg) {
    char ebuf[160];
    const char* elem = agg->child_count > 0 ? array_literal_elem_type(agg, ebuf, sizeof(ebuf)) : NULL;
    if (!elem) {
        codegen_error("this array literal's element type cannot be worked out here - declare it with a type first (T[] name = { ... })");
        fprintf(f, "COME_HDR_INIT(COME_CTX)");
        return;
    }
    char ft[200];
    snprintf(ft, sizeof(ft), "%s[]", elem);
    char arr_ct[160];
    header_c_type(ft, arr_ct, sizeof(arr_ct));
    fprintf(f, "({ %s _al = COME_HDR_INIT(COME_CTX); ", arr_ct);
    if (strcmp(elem, "string") == 0) {
        for (int i = 0; i < agg->child_count; i++) {
            fprintf(f, "{ come_string_t _e = ");
            emit_string_obj(f, agg->children[i]);
            fprintf(f, "; come_array_append_g((come_hdr_t*)&_al, sizeof(come_string_t), &_e); } ");
        }
    } else {
        char ect[96];
        elem_c_type(elem, ect, sizeof(ect));
        fprintf(f, "%s _vals[] = { ", ect);
        for (int i = 0; i < agg->child_count; i++) {
            if (i) fprintf(f, ", ");
            generate_expression(f, agg->children[i]);
        }
        fprintf(f, " }; come_array_ensure((come_hdr_t*)&_al, sizeof(%s), %d); memcpy(_al.data, _vals, sizeof(_vals)); _al.count = %d; ",
                ect, agg->child_count, agg->child_count);
    }
    fprintf(f, "_al; })");
}

// A link .read() call (COIL-135): buffers travel through a
// channel as boxed header cells; the default read emission
// dereferences the cell into a header value. When the value
// lands in a link-typed variable the payload is an end handle
// and the raw cell pointer is the value instead.
static int is_link_read_expr(ASTNode* e) {
    if (!e || e->type != AST_METHOD_CALL || strcmp(e->text, "read") != 0 ||
        e->child_count < 1 || !e->children[0] || e->children[0]->type != AST_IDENTIFIER)
        return 0;
    const char* rt = get_local_variable_type(e->children[0]->text);
    return rt && strcmp(rt, "link") == 0;
}

static void emit_link_read_raw(FILE* f, ASTNode* e) {
    fprintf(f, "come_link_read(");
    generate_expression(f, e->children[0]);
    fprintf(f, ", COME_CTX)");
}

// Emit an expression where a value of C type `ct` is needed.
// A link read naturally yields a come_hdr_t; when the landing
// type is a different header struct (come_string_t,
// come_map_t), copy the bits across (COIL-135: all header
// structs share one layout).
static void emit_expr_as(FILE* f, ASTNode* e, const char* ct) {
    if (is_link_read_expr(e) && strcmp(ct, "come_hdr_t") != 0) {
        fprintf(f, "({ %s _t; come_hdr_t _h = ", ct);
        generate_expression(f, e);
        fprintf(f, "; memcpy(&_t, &_h, sizeof(_t)); _t; })");
        return;
    }
    generate_expression(f, e);
}

// 1 when the expression is reference-typed (COIL-136).
static int expr_is_ref(ASTNode* e) {
    char buf[300];
    return is_ref_type(expr_come_type(e, buf, sizeof(buf)));
}

// Emit one call argument against its parameter's Come type
// (COIL-136). A reference parameter takes &lvalue (a struct
// variable, field, or element) or a reference variable of the
// same type, passed through as-is. A value parameter rejects
// a stray '&'. With no reference involved this is exactly the
// plain emission.
static void emit_ref_or_value_arg(FILE* f, ASTNode* arg, const char* ptype,
                                  const char* fname) {
    if (is_ref_type(ptype)) {
        char inner[256];
        ref_inner_type(ptype, inner, sizeof(inner));
        if (arg->type == AST_UNARY_OP && strcmp(arg->text, "&") == 0) {
            ASTNode* operand = arg->children[0];
            char ob[300];
            const char* ot = expr_come_type(operand, ob, sizeof(ob));
            if (is_ref_type(ot)) {
                codegen_error("the argument is already a reference: pass it to '%s' without '&'", fname);
                generate_expression(f, operand);
                return;
            }
            int lv = operand->type == AST_IDENTIFIER ||
                     operand->type == AST_MEMBER_ACCESS ||
                     operand->type == AST_ARRAY_ACCESS;
            if (!lv) {
                codegen_error("cannot take a reference to a temporary: '&' needs a variable, field, or element of type %s", inner);
            } else if (ot && strcmp(ot, inner) != 0) {
                codegen_error("reference type mismatch: the parameter is %s but the argument is %s", ptype, ot);
            }
            fprintf(f, "&(");
            generate_expression(f, operand);
            fprintf(f, ")");
            return;
        }
        char ab[300];
        const char* at = expr_come_type(arg, ab, sizeof(ab));
        if (is_ref_type(at) && strcmp(at, ptype) == 0) {
            generate_expression(f, arg);
            return;
        }
        codegen_error("a reference parameter (%s) needs a reference: pass &variable, or a %s variable", ptype, ptype);
        generate_expression(f, arg);
        return;
    }
    if (arg->type == AST_UNARY_OP && strcmp(arg->text, "&") == 0) {
        codegen_error("this parameter of '%s' is not a reference: remove the '&'", fname);
        generate_expression(f, arg->children[0]);
        return;
    }
    generate_expression(f, arg);
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
        if (g_in_coroutine && is_module_var(node->text) &&
            !get_local_variable_type(node->text)) {
            codegen_error("coroutine cannot access module variable '%s' (spec 9.1: a coroutine sees only its own locals and parameters)", node->text);
        }
        // An unknown name used to emit raw and fail in the C
        // compiler as an "undeclared" error against a mangled
        // function (COIL-129). Known names: locals and
        // parameters, module variables and constants, the
        // other files of a directory module, functions used
        // as values, and the legacy null.
        if (strcmp(node->text, "null") != 0 && !expr_is_fn_value(node) &&
            !get_local_variable_type(node->text) &&
            !is_module_var(node->text) && !is_const_name(node->text) &&
            !is_fn_const_name(node->text) &&
            !codegen_module_const_type(current_module, node->text) &&
            !is_group_symbol(node->text)) {
            codegen_error("'%s' is not defined: no variable, constant, or parameter has that name here - check the spelling, or declare it before use", node->text);
        }
        if (strcmp(node->text, "null") == 0) fprintf(f, "NULL");
        else if (!get_local_variable_type(node->text) &&
                 (g_module_level || !is_fn_const_name(node->text)) &&
                 codegen_module_const_type(current_module, node->text)) {
            // A module-level value constant of this module
            // (COIL-148): the one namespaced definition,
            // declared in this file or a sibling. Locals and
            // function constants of the same name shadow it.
            fprintf(f, "come_%s__const_%s", current_module, node->text);
        }
        else if (expr_is_fn_value(node)) {
            // A named function used as a value: its code reference
            // (spec 8.3 - legal as a call argument; the store and
            // call checks reject other positions). Extern "C"
            // functions keep their raw C name (COIL-111).
            if (is_extern_fn(node->text)) fprintf(f, "%s", node->text);
            else fprintf(f, "come_%s__%s", current_module, node->text);
        }
        else if (g_fn_instance_form && is_module_var(node->text) &&
                 !get_local_variable_type(node->text)) {
            // In an instance-form body a module variable is
            // a field of the instance (COIL-140); a local of
            // the same name still shadows it.
            fprintf(f, "(__inst->%s)", node->text);
        }
        else fprintf(f, "%s", node->text);
    } else if (node->type == AST_UNARY_OP) {
        if (strcmp(node->text, "&") == 0) {
            // Taking an address (COIL-136): at a typed call
            // site this builds a struct reference (checked by
            // emit_ref_or_value_arg); elsewhere it is the
            // C-interop form the std surface already uses
            // (scanf's &v out-parameters). Two shapes are
            // errors in every position: a reference to a
            // temporary, and '&' of something that already
            // is a reference.
            ASTNode* operand = node->children[0];
            char ob[300];
            const char* ot = expr_come_type(operand, ob, sizeof(ob));
            if (is_ref_type(ot)) {
                codegen_error("the operand is already a reference: use it directly, without '&'");
            } else if (operand->type != AST_IDENTIFIER &&
                       operand->type != AST_MEMBER_ACCESS &&
                       operand->type != AST_ARRAY_ACCESS) {
                codegen_error("cannot take a reference to a temporary: '&' needs a variable, field, or element");
            }
            fprintf(f, "&(");
            generate_expression(f, operand);
            fprintf(f, ")");
            return;
        }
        fprintf(f, "(%s", node->text);
        generate_expression(f, node->children[0]);
        fprintf(f, ")");
    } else if (node->type == AST_ARRAY_ACCESS) {
        ASTNode* base = node->children[0];
        char mkt[160], mvt[160];
        const char* bt = (base->type == AST_IDENTIFIER) ? dispatch_type_of(base->text) : NULL;
        // Indexing a scalar used to generate garbage C with
        // pointer-cast warnings (COIL-129).
        if (bt && (num_info(bt) || strcmp(bt, "bool") == 0)) {
            codegen_error("cannot index '%s' of type %s: only arrays, strings, and maps can be indexed", base->text, bt);
        }
        if ((bt && strcmp(bt, "string") == 0) || base->type == AST_STRING_LITERAL) {
            // String indexing (COIL-110, spec 6.5): s[i] is the i-th
            // character as a wchar. The runtime helper reports
            // out-of-bounds via ERR and yields 0. (Not COME_ARR_GET -
            // its _Generic cannot take strings.)
            fprintf(f, "come_string_wchar_at_e(");
            emit_string_expr(f, base);
            fprintf(f, ", ");
            generate_expression(f, node->children[1]);
            fprintf(f, ")");
        } else if (bt && map_type_parts(bt, mkt, sizeof(mkt), mvt, sizeof(mvt))) {
            // Typed map read (COIL-102): the value, or V's zero value
            // when the key is absent.
            if (!map_combo_supported(mkt, mvt)) map_combo_error(mkt, mvt);
            check_map_expr_type(mkt, node->children[1], "key");
            fprintf(f, "({ void* _p = come_map_get_e(");
            generate_expression(f, base);
            fprintf(f, ", ");
            emit_string_expr(f, node->children[1]);
            if (strcmp(mvt, "int") == 0) {
                fprintf(f, "); _p ? *(int*)_p : 0; })");
            } else {
                fprintf(f, "); _p ? *(come_string_t*)_p : come_string_new(COME_CTX, \"\"); })");
            }
        } else {
            // COME_ARR_GET_T(arr, index, ElemCType) (COIL-135: the
            // header's data is void*, so the element type travels
            // with the access).
            char ctbuf[160];
            const char* ct = expr_come_type(node->children[0], ctbuf, sizeof(ctbuf));
            char et[96] = "";
            if (ct) {
                const char* lb = strchr(ct, '[');
                if (lb && (size_t)(lb - ct) < 64) {
                    char elem[64];
                    memcpy(elem, ct, (size_t)(lb - ct));
                    elem[lb - ct] = 0;
                    elem_c_type(elem, et, sizeof(et));
                }
            }
            if (!et[0]) {
                codegen_error("cannot index here: the element type is not known - index a named array variable");
                fprintf(f, "(*(int*)come_oob_slot_bytes_at(\"");
                fprint_escaped_cstr(f, source_filename);
                fprintf(f, "\", %d, \"come_%s__%s\"))", node->source_line, current_module, current_function_name);
            } else if (g_unchecked ||
                       (base->type == AST_IDENTIFIER &&
                        node->children[1]->type == AST_IDENTIFIER &&
                        bound_proof_has(base->text, node->children[1]->text))) {
                // Parity step 1: guard-dominated access.
                // The guard already proved 0 <= idx < count
                // for these exact identifiers, unmodified
                // since, so the check is redundant. Raw form
                // is an lvalue like the macro, evaluating
                // each operand once (safe: identifiers have
                // no side effects, unlike the macro's two
                // evaluations).
                fprintf(f, "((%s*)(", et);
                generate_expression(f, base);
                fprintf(f, ".data))[");
                generate_expression(f, node->children[1]);
                fprintf(f, "]");
            } else {
                fprintf(f, "COME_ARR_GET_T(");
                generate_expression(f, node->children[0]);
                fprintf(f, ", ");
                generate_expression(f, node->children[1]);
                fprintf(f, ", %s)", et);
            }
        }
    } else if (node->type == AST_ASSIGN) {
        check_assign_store(node); // COIL-107
        if (node->children[0]->type == AST_IDENTIFIER && node->children[1]->type == AST_IDENTIFIER) {
            const char* lt = get_local_variable_type(node->children[0]->text);
            const char* rt = get_local_variable_type(node->children[1]->text);
            if (lt && rt) check_array_transfer("be assigned to", node->children[0]->text, lt, node->children[1]->text, rt);
        }
        if (node->children[0]->type == AST_IDENTIFIER && is_link_read_expr(node->children[1])) {
            const char* lt = get_local_variable_type(node->children[0]->text);
            if (lt && strcmp(lt, "link") == 0) {
                generate_expression(f, node->children[0]);
                fprintf(f, " %s ", node->text);
                emit_link_read_raw(f, node->children[1]);
                return;
            }
        }
        if (node->children[0]->type == AST_IDENTIFIER && is_link_read_expr(node->children[1])) {
            const char* lt = get_local_variable_type(node->children[0]->text);
            if (lt && strcmp(lt, "string") == 0) {
                generate_expression(f, node->children[0]);
                fprintf(f, " %s ", node->text);
                emit_expr_as(f, node->children[1], "come_string_t");
                return;
            }
        }
        if (node->children[0]->type == AST_IDENTIFIER &&
            strcmp(node->text, "=") == 0) {
            const char* lt = get_local_variable_type(node->children[0]->text);
            if (is_ref_type(lt)) {
                ASTNode* rhs = node->children[1];
                char rb[300];
                const char* rt2 = expr_come_type(rhs, rb, sizeof(rb));
                if (rhs->type == AST_UNARY_OP && strcmp(rhs->text, "&") == 0) {
                    // Rebind: the guarded '&' emission applies.
                    generate_expression(f, node->children[0]);
                    fprintf(f, " = ");
                    generate_expression(f, rhs);
                    return;
                }
                if (!is_ref_type(rt2)) {
                    // Assign through: the referent takes the value.
                    fprintf(f, "(*");
                    generate_expression(f, node->children[0]);
                    fprintf(f, ") = ");
                    generate_expression(f, rhs);
                    return;
                }
                // reference = reference: a plain rebind, below.
            }
        }
        // Step 4b remainder: a concat assignment to a
        // first-assignment-transient local bypasses
        // adoption entirely, like a declaration init
        // (which gets no adoption): the arena value is
        // created in the local's own frame, so it needs
        // no move, and later escaping uses are still
        // caught by the net at those sites. Clearing
        // g_view_promote makes the RHS concat arena.
        if (strcmp(node->text, "=") == 0 && node->child_count >= 2 &&
            node->children[0]->type == AST_IDENTIFIER &&
            node->children[1]->type == AST_BINARY_OP &&
            strcmp(node->children[1]->text, "+") == 0 &&
            codegen_transient_first_assign(current_module, current_function_name,
                                           node->children[0]->text)) {
            int saved_promote = g_view_promote;
            g_view_promote = 0;
            generate_expression(f, node->children[0]);
            fprintf(f, " = ");
            generate_expression(f, node->children[1]);
            g_view_promote = saved_promote;
            return;
        }
        // COIL-157: an assignment used as a value still
        // moves scopes when it stores a buffer: temp the
        // value, adopt it to the destination's scope,
        // store it, and yield it.
        if (strcmp(node->text, "=") == 0 && node->child_count >= 2 &&
            (node->children[0]->type == AST_IDENTIFIER ||
             node->children[0]->type == AST_MEMBER_ACCESS)) {
            char ltb[300];
            const char* ltype = resolve_lhs_type(node->children[0], ltb, sizeof(ltb));
            // A map INDEX write keeps its existing path: a
            // map index is not an array element, and map
            // writes have their own interception. But a
            // whole-map variable assignment DOES adopt -
            // narrowing the guard to index writes only, or
            // a table stored into a field (kw_table in
            // create()) would die with the storing scope.
            if (ltype && type_needs_adopt(ltype) &&
                !(strncmp(ltype, "map{", 4) == 0 &&
                  node->children[0]->type == AST_ARRAY_ACCESS)) {
                char ctype[300];
                come_type_to_c(ltype, ctype, sizeof(ctype));
                char dst[160];
                ASTNode* root = node->children[0];
                while (root->type == AST_MEMBER_ACCESS && root->child_count >= 1)
                    root = root->children[0];
                if (root->type == AST_IDENTIFIER) dst_scope_text(root->text, dst, sizeof(dst));
                else snprintf(dst, sizeof(dst), "COME_CTX");
                fprintf(f, "({ %s _come_v = ", ctype);
                generate_expression(f, node->children[1]);
                fprintf(f, "; ");
                emit_adopt_value(f, 0, "_come_v", ltype, dst);
                fprintf(f, " ");
                generate_expression(f, node->children[0]);
                fprintf(f, " = _come_v; _come_v; })");
                return;
            }
        }
        generate_expression(f, node->children[0]);
        fprintf(f, " %s ", node->text);
        generate_expression(f, node->children[1]);
    } else if (node->type == AST_MEMBER_ACCESS) {
        // A qualified constant (COIL-148): mod.NAME names a
        // module-level value constant of another module -
        // visible only when exported (spec 3.4), and bound
        // to the one namespaced definition.
        if (node->children[0]->type == AST_IDENTIFIER &&
            !get_local_variable_type(node->children[0]->text)) {
            const char* mod = node->children[0]->text;
            if (codegen_module_const_type(mod, node->text)) {
                if (strcmp(mod, current_module) != 0 &&
                    !module_symbol_visible(mod, node->text)) {
                    codegen_error("'%s.%s' is not exported by module '%s' (spec 3.4: only exported symbols are visible outside their module)", mod, node->text, mod);
                }
                fprintf(f, "come_%s__const_%s", mod, node->text);
                return;
            }
        }
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
        
        int is_ptr = is_pointer_expression(node->children[0]) ||
                     expr_is_ref(node->children[0]);
        if (is_ptr) {
            fprintf(f, ")->%s", node->text);
        } else {
            fprintf(f, ").%s", node->text);
        }
    } else if (node->type == AST_METHOD_CALL) {
        char* method = node->text;
        char c_func[16384];
        int skip_receiver = 0;
        ASTNode* receiver = node->children[0];

        // Module instances (COIL-140): a local whose type is
        // an instance of an imported module (recorded as
        // "instance:<mod>", or the module's name used as the
        // type) calls the module's functions bound to that
        // instance. c_func is set here and the shared tail
        // emits it with the receiver as the first argument,
        // like any method: stateful functions take the
        // instance directly, stateless ones go through the
        // generated __inst wrapper (which guards the call).
        const char* inst_mod = NULL;
        if (receiver->type == AST_IDENTIFIER) {
            const char* rt = get_local_variable_type(receiver->text);
            if (rt) {
                if (strncmp(rt, "instance:", 9) == 0) inst_mod = rt + 9;
                else if (codegen_module_can_instance(rt)) inst_mod = rt;
            }
            if (!inst_mod && g_fn_instance_form) {
                // A module variable of this module whose
                // type is another stateful module is an
                // instance held in this instance's state
                // (COIL-154): calls on it are instance
                // calls, with the field as the receiver.
                const char* vt = NULL;
                if (codegen_module_var_index(current_module, receiver->text, &vt) >= 0 &&
                    vt && codegen_module_can_instance(vt))
                    inst_mod = vt;
            }
            if (inst_mod) {
                // Emitted in full here: the generic dispatch
                // below would mistake the "instance:<mod>"
                // type for a struct type. The receiver goes
                // first (it is a pointer already); a string
                // literal argument is materialized, as for
                // same-module calls (COIL-133).
                if (strcmp(method, "create") == 0) {
                    codegen_error("'%s' is already an instance of module '%s': create() belongs on the module name (%s.create())", receiver->text, inst_mod, inst_mod);
                    return;
                }
                if (strcmp(method, "destroy") == 0) {
                    fprintf(f, "come_%s__destroy(", inst_mod);
                    generate_expression(f, receiver);
                    fprintf(f, ")");
                    return;
                }
                // The receiver expression, not just its
                // name: a local prints its name, a module
                // variable held as state prints __inst->x
                // (COIL-154).
                if (codegen_fn_stateful(inst_mod, method, NULL))
                    fprintf(f, "come_%s__%s(", inst_mod, method);
                else
                    fprintf(f, "come_%s__%s__inst(", inst_mod, method);
                generate_expression(f, receiver);
                for (int i = 1; i < node->child_count; i++) {
                    fprintf(f, ", ");
                    if (node->children[i]->type == AST_STRING_LITERAL)
                        emit_string_expr(f, node->children[i]);
                    else
                        generate_expression(f, node->children[i]);
                }
                fprintf(f, ")");
                return;
            }
        }

        // Detect module static calls
        int is_import = 0;
        if (receiver->type == AST_IDENTIFIER) {
            for (int i=0; i<current_import_count; i++) {
                if (strcmp(receiver->text, current_imports[i]) == 0) { is_import = 1; break; }
            }
        }
        
        if (receiver->type == AST_IDENTIFIER && (
            strcmp(receiver->text, "net")==0 || 
            strcmp(receiver->text, "conv")==0 || 
            strcmp(receiver->text, "mem")==0 ||
            strcmp(receiver->text, "std")==0 ||
            strcmp(receiver->text, "ERR")==0 ||
            is_import)) {
            
            skip_receiver = 1;

            // ERR.set() is a predicate, not a call: true when an error
            // is recorded (COIL-105). ERR.fatal(msg) halts via the
            // runtime; its argument needs a string-object wrap.
            if (strcmp(receiver->text, "ERR")==0 && strcmp(method, "set")==0) {
                 fprintf(f, "(come_ERR_no() != 0)");
                 return;
            }
            // COIL-139: indexed accessors and depth. The
            // index argument is optional (default 0, the
            // root cause); the call site rides along so an
            // out-of-range read can record its own error.
            if (strcmp(receiver->text, "ERR")==0 &&
                (strcmp(method, "no")==0 || strcmp(method, "str")==0 ||
                 strcmp(method, "where")==0)) {
                 if (node->child_count > 2) {
                     codegen_error("ERR.%s takes at most one argument, the chain index (spec 12)", method);
                     return;
                 }
                 fprintf(f, "come_ERR_%s_at(", method);
                 if (node->child_count > 1) generate_expression(f, node->children[1]);
                 else fprintf(f, "0");
                 emit_err_site_args(f, node);
                 fprintf(f, ")");
                 return;
            }
            if (strcmp(receiver->text, "ERR")==0 && strcmp(method, "depth")==0) {
                 fprintf(f, "come_ERR_depth()");
                 return;
            }
            if (strcmp(receiver->text, "ERR")==0 && strcmp(method, "fatal")==0) {
                 fprintf(f, "come_ERR_fatal_at(");
                 if (node->child_count > 1) emit_string_expr(f, node->children[1]);
                 else fprintf(f, "NULL");
                 emit_err_site_args(f, node);
                 fprintf(f, ")");
                 return;
            }

            // Module instances (COIL-140): create() builds
            // an instance of a stateful module; a stateful
            // function has no namespace call - the error
            // names the witness chain the analysis computed.
            if (is_import && strcmp(method, "create") == 0) {
                 if (!codegen_module_can_instance(receiver->text)) {
                     codegen_error("module '%s' cannot be instantiated: it does not define the create/destroy lifecycle pair - a module is instantiable only with both hooks; call its functions through the namespace instead", receiver->text);
                     return;
                 }
                 fprintf(f, "come_%s__create()", receiver->text);
                 return;
            }
            if (is_import) {
                 const char* chain = NULL;
                 if (codegen_fn_stateful(receiver->text, method, &chain)) {
                     codegen_error("'%s.%s' cannot be called on the namespace: it uses module state (%s) - create an instance first: var x = %s.create(), then x.%s(...)", receiver->text, method, chain, receiver->text, method);
                     return;
                 }
            }

            // std.qsort(arr, cmp): byte-array sort with a named
            // comparator (COIL-116, Come_std.md section 4).
            if (strcmp(receiver->text, "std")==0 && strcmp(method, "qsort")==0) {
                 ASTNode* arr_arg = node->child_count > 1 ? node->children[1] : NULL;
                 ASTNode* cb = node->child_count > 2 ? node->children[2] : NULL;
                 fprintf(f, "come_std__qsort(");
                 if (arr_arg) generate_expression(f, arr_arg);
                 fprintf(f, ", ");
                 if (cb && cb->type == AST_IDENTIFIER) {
                     const char* lt = get_local_variable_type(cb->text);
                     if (lt && is_fn_type(lt)) generate_expression(f, cb);
                     else if (find_module_fn_sig(cb->text)) fprintf(f, "come_%s__%s", current_module, cb->text);
                     else codegen_error("std.qsort: '%s' is not a function of this module", cb->text);
                 } else {
                     codegen_error("std.qsort expects a comparator function name (spec 8.3)");
                 }
                 fprintf(f, ")");
                 return;
            }

            // string.sprintf(fmt, ...) / string.sscanf(str, fmt,
            // ...) (COIL-117, Come_string.md): sprintf's format
            // and string arguments go as byte buffers (the
            // runtime formats with vsnprintf); sscanf's remaining
            // arguments are pointers, passed as written.
            if (strcmp(receiver->text, "string")==0 && strcmp(method, "sprintf")==0) {
                 fprintf(f, "come_string_sprintf(COME_CTX, ");
                 if (node->child_count > 1) emit_cstr_arg(f, node->children[1]);
                 for (int i = 2; i < node->child_count; i++) {
                     fprintf(f, ", ");
                     ASTNode* a = node->children[i];
                     if (a->type == AST_IDENTIFIER && get_local_variable_type(a->text) &&
                         strcmp(get_local_variable_type(a->text), "string") == 0) {
                         emit_cstr_arg(f, a);
                     } else {
                         generate_expression(f, a);
                     }
                 }
                 fprintf(f, ")");
                 return;
            }
            if (strcmp(receiver->text, "string")==0 && strcmp(method, "sscanf")==0) {
                 fprintf(f, "come_string_sscanf(");
                 if (node->child_count > 1) emit_string_obj(f, node->children[1]);
                 fprintf(f, ", ");
                 if (node->child_count > 2) emit_cstr_arg(f, node->children[2]);
                 for (int i = 3; i < node->child_count; i++) {
                     fprintf(f, ", ");
                     generate_expression(f, node->children[i]);
                 }
                 fprintf(f, ")");
                 return;
            }

            if (strcmp(receiver->text, "mem")==0 && strcmp(method, "cpy")==0) {
                 strcpy(c_func, "memcpy");
             } else if (strcmp(receiver->text, "std")==0 && strcmp(method, "printf")==0) {
                 strcpy(c_func, "printf"); 
             } else {
                 if (is_import) {
                     // New schema for imported modules: come_MODULE__FUNC
                     // Spec 3.4: a module that declares exports
                     // shows only those symbols outside itself.
                     if (!module_symbol_visible(receiver->text, method)) {
                         codegen_error("'%s.%s' is not exported by module '%s' (spec 3.4: only exported symbols are visible outside their module)", receiver->text, method, receiver->text);
                     }
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

             // std.proc: process & environment (COIL-115,
             // Come_std.md section 5).
             if (strcmp(receiver->text, "proc") == 0) {
                 if (strcmp(method, "abort") == 0) {
                     fprintf(f, "come_std__Proc__abort(NULL)");
                 } else if (strcmp(method, "exit") == 0) {
                     fprintf(f, "come_std__Proc__exit(NULL, ");
                     if (node->child_count > 1) generate_expression(f, node->children[1]);
                     else fprintf(f, "0");
                     fprintf(f, ")");
                 } else if (strcmp(method, "atexit") == 0) {
                     ASTNode* cb = node->child_count > 1 ? node->children[1] : NULL;
                     if (!cb || cb->type != AST_IDENTIFIER) {
                         codegen_error("std.proc.atexit expects a function name (spec 8.3)");
                         fprintf(f, "0");
                         return;
                     }
                     const char* lt = get_local_variable_type(cb->text);
                     fprintf(f, "come_std__Proc__atexit(NULL, ");
                     if (lt && is_fn_type(lt)) {
                         generate_expression(f, cb);   // fn parameter passed through
                     } else if (find_module_fn_sig(cb->text)) {
                         fprintf(f, "come_%s__%s", current_module, cb->text);
                     } else {
                         codegen_error("std.proc.atexit: '%s' is not a function of this module", cb->text);
                     }
                     fprintf(f, ")");
                 } else if (strcmp(method, "getenv") == 0 || strcmp(method, "system") == 0) {
                     fprintf(f, "come_std__Proc__%s(NULL, ", method);
                     if (node->child_count > 1) emit_string_expr(f, node->children[1]);
                     else fprintf(f, "NULL");
                     fprintf(f, ")");
                 } else {
                     codegen_error("std.proc has no method '%s' (Come_std.md section 5)", method);
                     fprintf(f, "0");
                 }
                 return;
             }

             // FILE methods on the pre-instantiated streams
             // (COIL-114): std.in / std.out / std.err are FILE
             // objects. puts/printf on out/err keep the fast paths
             // below; everything else goes to the runtime.
             if (strcmp(receiver->text, "in") == 0 || strcmp(receiver->text, "out") == 0 || strcmp(receiver->text, "err") == 0) {
                 int fast = strcmp(receiver->text, "in") != 0 &&
                            (strcmp(method, "puts") == 0 || strcmp(method, "printf") == 0);
                 if (!fast) {
                     char recv_c[64];
                     snprintf(recv_c, sizeof(recv_c), "std_%.*s", (int)sizeof(recv_c) - 5, receiver->text);
                     if (emit_file_method_call(f, recv_c, node)) return;
                     codegen_error("FILE has no method '%s' (Come_std.md section 2)", method);
                     fprintf(f, "0");
                     return;
                 }
             }

             // std.out.puts / std.err.puts (COIL-105: the spec's ERR
             // example writes with std.err.puts; neither existed)
             if ((strcmp(receiver->text, "out") == 0 || strcmp(receiver->text, "err") == 0) && strcmp(method, "puts") == 0) {
                 const char* stream = strcmp(receiver->text, "out") == 0 ? "stdout" : "stderr";
                 fprintf(f, "({ fputs(");
                 if (node->child_count > 1) {
                     ASTNode* a = node->children[1];
                     if (a->type == AST_STRING_LITERAL) {
                         generate_expression(f, a);
                     } else {
                         fprintf(f, "((");
                         generate_expression(f, a);
                         fprintf(f, ").data ? (");
                         generate_expression(f, a);
                         fprintf(f, ").data : \"\")");
                     }
                 } else {
                     fprintf(f, "\"\"");
                 }
                 fprintf(f, ", %s); fputc(10, %s); 0; })", stream, stream);
                 return;
             }

             if ((strcmp(receiver->text, "out") == 0 || strcmp(receiver->text, "err") == 0) && strcmp(method, "printf") == 0) {
                 strcpy(c_func, "fprintf");
                 if (strcmp(receiver->text, "out") == 0) fprintf(f, "fprintf(stdout, ");
                 else fprintf(f, "fprintf(stderr, ");
                 skip_receiver = 1;
                 // Loop args
                 char* fmt_modified = NULL;
                 int* bool_args = NULL;
                char* spec_args = NULL;
                 int arg_count = node->child_count;
                 
                 // Pre-scan for format string modification
                 if (arg_count > 1 && node->children[1]->type == AST_STRING_LITERAL) {
                     bool_args = calloc(arg_count + 1, sizeof(int)); // +1 safety
                     spec_args = calloc(arg_count + 1, sizeof(char));
                     char* raw_fmt = node->children[1]->text; 
                     fmt_modified = calloc(strlen(raw_fmt) * 2 + 100, 1);
                     
                     char* src = raw_fmt;
                     char* dst = fmt_modified;
                     int current_arg_idx = 2; // Arrrgs buffer index (2, 3...)
                     int len_mod = 0; // a length modifier (l, z, ...) was seen
                     
                     while (*src) {
                         if (*src == '%' && *(src+1) != '%') {
                             len_mod = 0;
                             *dst++ = *src++; // Copy %
                             
                             // Flags
                             while (*src && strchr("-+ #0", *src)) *dst++ = *src++;
                             
                             // Width
                             if (*src == '*') { *dst++ = *src++; current_arg_idx++; }
                             else while (*src && isdigit(*src)) *dst++ = *src++;
                             
                             // Precision
                             if (*src == '.') {
                                 *dst++ = *src++;
                                 if (*src == '*') { *dst++ = *src++; current_arg_idx++; }
                                 else while (*src && isdigit(*src)) *dst++ = *src++;
                             }
                             
                             // Length
                             while (*src && strchr("hljzL", *src)) { len_mod = 1; *dst++ = *src++; }
                             
                             // Specifier
                             if (*src == 't') {
                                 if (current_arg_idx < arg_count) bool_args[current_arg_idx] = 1;
                                 *dst++ = 's'; // Replace with %s
                                 src++;
                             } else if (*src == 'T') {
                                 if (current_arg_idx < arg_count) bool_args[current_arg_idx] = 2;
                                 *dst++ = 's'; // Replace with %s
                                 src++;
                             } else {
                                 if (*src) {
                                     if (current_arg_idx < arg_count && !len_mod)
                                         spec_args[current_arg_idx] = *src;
                                     *dst++ = *src++;
                                 }
                             }
                             current_arg_idx++;
                         } else {
                             if (*src == '%' && *(src+1) == '%') {
                                 *dst++ = *src++; // Copy first %
                                 *dst++ = *src++; // Copy second %
                             } else {
                                 *dst++ = *src++;
                             }
                         }
                     }
                     *dst = '\0';
                 }

                 // Design 3 (redesign): printf %s reads to
                 // NUL, which a view lacks at its end, so
                 // view arguments promote to copies here
                 // (the generic call-argument rule does not
                 // reach this dedicated loop).
                 int saved_view_promote_printf = g_view_promote;
                 g_view_promote = 1;
                 for (int i = 1; i < node->child_count; i++) {
                     if (i > 1) fprintf(f, ", ");
                     
                     if (i == 1 && fmt_modified) {
                         fprintf(f, "%s", fmt_modified);
                         continue;
                     }
                     
                     if (bool_args && bool_args[i] != 0) {
                         fprintf(f, "(");
                         generate_expression(f, node->children[i]);
                         if (bool_args[i] == 1) fprintf(f, " ? \"true\" : \"false\")");
                         else fprintf(f, " ? \"TRUE\" : \"FALSE\")");
                         continue;
                     }
                     
                     ASTNode* arg = node->children[i];
                     
                     // Detect if arg is string-typed expression
                     int is_str = 0;
                     if (arg->type == AST_IDENTIFIER) {
                          const char* type = get_local_variable_type(arg->text);
                          if (type && (strcmp(type, "string") == 0 || strcmp(type, "come_string_t") == 0)) {
                              is_str = 1;
                          }
                          if (!is_str && arg->type == AST_IDENTIFIER) {
                              const char* dt = dispatch_type_of(arg->text);
                              if (dt && strcmp(dt, "string") == 0) is_str = 1;
                          }
                      } else if (arg->type == AST_METHOD_CALL || arg->type == AST_CALL) {
                          // Check methods that return strings
                           // Check methods/functions that return strings (NOT len/size/count which return uint)
                           const char* name = arg->text;
                           // type() returns a string on maps,
                           // arrays and strings alike (COIL-103/116/119)
                           if (strcmp(name, "type") == 0 && arg->child_count > 0 &&
                               arg->children[0]->type == AST_IDENTIFIER) {
                               const char* rt = dispatch_type_of(arg->children[0]->text);
                               if (rt && (strncmp(rt, "map{", 4) == 0 || strchr(rt, '[') != NULL ||
                                          strcmp(rt, "string") == 0)) {
                                   is_str = 1;
                               }
                           }
                           
                           if (strcmp(name, "upper") == 0 || strcmp(name, "lower") == 0 ||
                               strcmp(name, "repeat") == 0 || strcmp(name, "replace") == 0 ||
                               strcmp(name, "trim") == 0 || strcmp(name, "ltrim") == 0 ||
                               strcmp(name, "rtrim") == 0 || strcmp(name, "substr") == 0 ||
                               strcmp(name, "view") == 0 ||
                               strcmp(name, "join") == 0 || strcmp(name, "new") == 0 ||
                               strcmp(name, "str") == 0 || strcmp(name, "gets") == 0 ||
                               strcmp(name, "where") == 0) {
                               is_str = 1;
                           }
                           // A namespace or instance call into
                           // a registered module whose function
                           // returns string (COIL-140).
                           if (!is_str && arg->type == AST_METHOD_CALL) {
                               const char* rr = method_call_module_ret(arg);
                               if (rr && strcmp(rr, "string") == 0) is_str = 1;
                           }
                     } else if (arg->type == AST_ARRAY_ACCESS) {
                         // String element access: string[] element or a
                         // map{string, string} read (COIL-102). The old
                         // hardcoded name list stays as a fallback.
                         ASTNode* arr = arg->children[0];
                         if (arr->type == AST_IDENTIFIER) {
                              const char* at = dispatch_type_of(arr->text);
                              char mkt[160], mvt[160];
                              if (at && strcmp(at, "string[]") == 0) {
                                  is_str = 1;
                              } else if (at && map_type_parts(at, mkt, sizeof(mkt), mvt, sizeof(mvt)) && strcmp(mvt, "string") == 0) {
                                  is_str = 1;
                              } else if (strcmp(arr->text, "parts")==0 || strcmp(arr->text, "groups")==0 || strcmp(arr->text, "regex_parts")==0 || strcmp(arr->text, "args")==0) {
                                  is_str = 1;
                              }
                         }
                     } else if (arg->type == AST_STRING_LITERAL) {
                         generate_expression(f, arg); // string literal is perfectly %s safe (char*)
                         continue;
                     }

                     if (is_str) {
                         fprintf(f, "((");
                         generate_expression(f, arg);
                         fprintf(f, ").data ? (");
                         generate_expression(f, arg);
                         fprintf(f, ").data : \"NULL\")");
                     } else if (spec_args && i < arg_count &&
                                (arg->type == AST_METHOD_CALL || arg->type == AST_CALL) &&
                                (spec_args[i] == 'd' || spec_args[i] == 'i' ||
                                 spec_args[i] == 'u')) {
                         // Runtime helpers commonly return long;
                         // match the conversion so generated code
                         // compiles without -Wformat noise.
                         fprintf(f, spec_args[i] == 'u' ? "(unsigned int)(" : "(int)(");
                         generate_expression(f, arg);
                         fprintf(f, ")");
                     } else {
                         generate_expression(f, arg);
                     }

                 }
                 g_view_promote = saved_view_promote_printf;
                 fprintf(f, ")");
                 return;
             }
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
        // Link methods (COIL-123, spec 9.2): read/write/close
        // on a link-typed receiver go to the channel runtime.
        else if ((strcmp(method, "read") == 0 || strcmp(method, "write") == 0 ||
                  strcmp(method, "close") == 0) &&
                 receiver->type == AST_IDENTIFIER &&
                 get_local_variable_type(receiver->text) &&
                 strcmp(get_local_variable_type(receiver->text), "link") == 0) {
             if (strcmp(method, "read") == 0) {
                 // The value form: dereference the traveling
                 // header cell and adopt the reader's context.
                 fprintf(f, "({ come_hdr_t* _c = (come_hdr_t*)come_link_read(");
                 generate_expression(f, receiver);
                 fprintf(f, ", COME_CTX); _c->ctx = COME_CTX; *_c; })");
             } else if (strcmp(method, "write") == 0) {
                 if (node->child_count > 1) {
                     ASTNode* arg = node->children[1];
                     const char* at = (arg->type == AST_IDENTIFIER) ? get_local_variable_type(arg->text) : NULL;
                     if (at && strcmp(at, "link") == 0) {
                         // A link end travels as its handle.
                         fprintf(f, "come_link_write(");
                         generate_expression(f, receiver);
                         fprintf(f, ", ");
                         generate_expression(f, arg);
                         fprintf(f, ")");
                     } else if (is_link_read_expr(arg)) {
                         // Forward the traveling cell as-is.
                         fprintf(f, "come_link_write(");
                         generate_expression(f, receiver);
                         fprintf(f, ", ");
                         emit_link_read_raw(f, arg);
                         fprintf(f, ")");
                     } else {
                         // Box the header value in a cell; its
                         // data block is re-parented under the
                         // cell so the channel owns both.
                         fprintf(f, "({ come_hdr_t* _b = (come_hdr_t*)mem_talloc_alloc(COME_CTX, sizeof(come_hdr_t)); __auto_type _v = (");
                         if (arg->type == AST_STRING_LITERAL) emit_string_expr(f, arg);
                         else generate_expression(f, arg);
                         fprintf(f, "); memcpy(_b, &_v, sizeof(come_hdr_t)); if (_b->data) mem_talloc_steal(_b, _b->data); come_link_write(");
                         generate_expression(f, receiver);
                         fprintf(f, ", _b); })");
                     }
                 } else {
                     codegen_error("link write needs a buffer argument");
                     fprintf(f, "0");
                 }
             } else {
                 fprintf(f, "come_link_close(");
                 generate_expression(f, receiver);
                 fprintf(f, ")");
             }
             return;
        }
        // Array toolbox (COIL-116): methods on an array-typed
        // receiver go to the array runtime. Must precede the map
        // branch below, which also claims remove.
        else if (is_array_toolbox_method(method) && is_array_receiver(receiver)) {
             emit_array_method(f, node, receiver, method);
             return;
        }
        // Detect Map methods (put, get, remove only - not len which is shared with string)
        else if (strcmp(method, "put") == 0 || strcmp(method, "get") == 0 || strcmp(method, "remove") == 0) {
             int is_map = 0;
             if (receiver->type == AST_IDENTIFIER) {
                 const char* type = dispatch_type_of(receiver->text);
                 if (type && (strcmp(type, "map") == 0 || strcmp(type, "come_map_t") == 0)) {
                     is_map = 1;
                 }
             }
             
             if (is_map) {
                 if (strcmp(method, "put") == 0 && node->child_count > 2) {
                     // Legacy map values are boxed cells (COIL-135).
                     fprintf(f, "({ come_string_t* _bp = (come_string_t*)mem_talloc_alloc(COME_CTX, sizeof(come_string_t)); *_bp = (");
                     emit_string_expr(f, node->children[2]);
                     fprintf(f, "); come_map_put(&(");
                     generate_expression(f, receiver);
                     fprintf(f, "), ");
                     emit_string_expr(f, node->children[1]);
                     fprintf(f, ", _bp); })");
                     return;
                 }
                 if (strcmp(method, "get") == 0 && node->child_count > 1) {
                     fprintf(f, "({ void* _p = come_map_get(");
                     generate_expression(f, receiver);
                     fprintf(f, ", ");
                     emit_string_expr(f, node->children[1]);
                     fprintf(f, "); _p ? *(come_string_t*)_p : (come_string_t){0}; })");
                     return;
                 }
                 snprintf(c_func, sizeof(c_func), "come_map_%s", method);
                 fprintf(f, "%s(", c_func);
                 if (strcmp(method, "remove") == 0) {
                     fprintf(f, "&(");
                     generate_expression(f, receiver);
                     fprintf(f, ")");
                 } else {
                     generate_expression(f, receiver);
                 }
                 for (int i = 1; i < node->child_count; i++) {
                     fprintf(f, ", ");
                     generate_expression(f, node->children[i]);
                 }
                 fprintf(f, ")");
                 return;
             }
        }
        // Universal methods on a struct value (spec 7.4,
        // COIL-127): length is the field count, bytesize the
        // C size of the struct, type is "struct".
        else if (struct_receiver_name(receiver) &&
                 (strcmp(method, "length") == 0 || strcmp(method, "bytesize") == 0 ||
                  strcmp(method, "type") == 0)) {
             const char* sname = struct_receiver_name(receiver);
             if (strcmp(method, "length") == 0) {
                 fprintf(f, "%d", struct_field_count(sname));
             } else if (strcmp(method, "bytesize") == 0) {
                 fprintf(f, "((int)sizeof(struct %s))", sname);
             } else {
                 fprintf(f, "come_string_new(COME_CTX, \"struct\")");
             }
             return;
        }
        // Typed map methods (COIL-103): the Come_map.md method table.
        // Must precede the string branch, which also claims length/chown.
        else if (is_typed_map_method(receiver, method)) {
             const char* rtype = dispatch_type_of(receiver->text);
             char mkt[160], mvt[160];
             map_type_parts(rtype, mkt, sizeof(mkt), mvt, sizeof(mvt));
             if (strcmp(method, "length") == 0) {
                 fprintf(f, "come_map_len(");
                 generate_expression(f, receiver);
                 fprintf(f, ")");
             } else if (strcmp(method, "has") == 0) {
                 if (node->child_count > 1) check_map_expr_type(mkt, node->children[1], "key");
                 fprintf(f, "(come_map_get(");
                 generate_expression(f, receiver);
                 fprintf(f, ", ");
                 if (node->child_count > 1) emit_string_expr(f, node->children[1]);
                 fprintf(f, ") != NULL)");
             } else if (strcmp(method, "delete") == 0) {
                 if (node->child_count > 1) check_map_expr_type(mkt, node->children[1], "key");
                 fprintf(f, "come_map_remove_e(&(");
                 generate_expression(f, receiver);
                 fprintf(f, "), ");
                 if (node->child_count > 1) emit_string_expr(f, node->children[1]);
                 fprintf(f, ")");
             } else if (strcmp(method, "clear") == 0) {
                 fprintf(f, "come_map_clear(&(");
                 generate_expression(f, receiver);
                 fprintf(f, "))");
             } else if (strcmp(method, "keys") == 0) {
                 fprintf(f, "come_map_keys(");
                 generate_expression(f, receiver);
                 fprintf(f, ")");
             } else if (strcmp(method, "lookup_lower") == 0) {
                 // Byte-range lookup with the range lowercased
                 // (Design 3): m.lookup_lower(bytes, pos, len)
                 // returns the value with ERR set on a miss,
                 // like a typed map read, but materializes no
                 // string. Args default to an empty range.
                 fprintf(f, "({ void* _p = come_map_get_lower_e(");
                 generate_expression(f, receiver);
                 fprintf(f, ", ");
                 if (node->child_count > 1) generate_expression(f, node->children[1]);
                 else fprintf(f, "(come_byte_array_t){0}");
                 fprintf(f, ", ");
                 if (node->child_count > 2) generate_expression(f, node->children[2]);
                 else fprintf(f, "0");
                 fprintf(f, ", ");
                 if (node->child_count > 3) generate_expression(f, node->children[3]);
                 else fprintf(f, "0");
                 if (strcmp(mvt, "int") == 0) {
                     fprintf(f, "); _p ? *(int*)_p : 0; })");
                 } else {
                     fprintf(f, "); _p ? *(come_string_t*)_p : come_string_new(COME_CTX, \"\"); })");
                 }
             } else if (strcmp(method, "type") == 0) {
                 fprintf(f, "come_string_new(COME_CTX, \"map\")");
             } else if (strcmp(method, "owner") == 0) {
                 fprintf(f, "(");
                 generate_expression(f, receiver);
                 fprintf(f, ").ctx");
             } else if (strcmp(method, "chown") == 0) {
                 ASTNode* a1 = node->child_count > 1 ? node->children[1] : NULL;
                 const char* at = (a1 && a1->type == AST_IDENTIFIER) ? get_local_variable_type(a1->text) : NULL;
                 int arg_is_buffer = at && (strchr(at, '[') != NULL || strcmp(at, "string") == 0 ||
                                            strncmp(at, "map{", 4) == 0);
                 if (arg_is_buffer) {
                     fprintf(f, "come_hdr_chown_to((come_hdr_t*)&(");
                     generate_expression(f, receiver);
                     fprintf(f, "), (const come_hdr_t*)&(");
                     generate_expression(f, a1);
                     fprintf(f, "))");
                 } else {
                     fprintf(f, "come_hdr_chown_ctx((come_hdr_t*)&(");
                     generate_expression(f, receiver);
                     fprintf(f, "), ");
                     if (a1) generate_expression(f, a1);
                     else fprintf(f, "COME_CTX");
                     fprintf(f, ")");
                 }
             } else if (strcmp(method, "dup") == 0) {
                 // dup(0) shares the map; dup()/dup(n) copies it
                 if (node->child_count > 1 && node->children[1]->type == AST_NUMBER &&
                     atoi(node->children[1]->text) == 0) {
                     generate_expression(f, receiver);
                 } else {
                     fprintf(f, "come_map_copy(");
                     generate_expression(f, receiver);
                     fprintf(f, ")");
                 }
             }
             return;
        }
        // String methods with C-string parameters (COIL-117):
        // separators, patterns and cutsets reach the runtime as
        // byte buffers. Receivers: a string variable, a literal,
        // or a call/index that produced a string.
        else if (is_cstr_param_string_method(method) &&
                 (receiver->type == AST_STRING_LITERAL ||
                  receiver->type == AST_METHOD_CALL ||
                  receiver->type == AST_ARRAY_ACCESS ||
                  (receiver->type == AST_IDENTIFIER &&
                   get_local_variable_type(receiver->text) &&
                   strcmp(get_local_variable_type(receiver->text), "string") == 0))) {
             emit_cstr_string_method(f, node, method);
             return;
        }
        // The universal methods on a plain string (spec 7):
        // dup, owner, type. (Arrays and maps claim theirs in
        // their own branches above.)
        else if ((strcmp(method, "dup") == 0 || strcmp(method, "owner") == 0 || strcmp(method, "type") == 0) &&
                 (receiver->type == AST_STRING_LITERAL ||
                  receiver->type == AST_METHOD_CALL ||
                  receiver->type == AST_ARRAY_ACCESS ||
                  (receiver->type == AST_IDENTIFIER &&
                   get_local_variable_type(receiver->text) &&
                   strcmp(get_local_variable_type(receiver->text), "string") == 0))) {
             if (strcmp(method, "owner") == 0) {
                 fprintf(f, "COME_CTX");
             } else if (strcmp(method, "type") == 0) {
                 fprintf(f, "come_string_new(COME_CTX, \"string\")");
             } else if (node->child_count > 1 && node->children[1]->type == AST_NUMBER &&
                        atoi(node->children[1]->text) == 0) {
                 emit_string_obj(f, receiver);   // dup(0) shares
             } else if (node->child_count > 1 && node->children[1]->type == AST_NUMBER) {
                 fprintf(f, "come_string_dup(");
                 emit_string_obj(f, receiver);
                 fprintf(f, ")");
             } else if (node->child_count > 1) {
                 fprintf(f, "((");
                 generate_expression(f, node->children[1]);
                 fprintf(f, ") == 0 ? ");
                 emit_string_obj(f, receiver);
                 fprintf(f, " : come_string_dup(");
                 emit_string_obj(f, receiver);
                 fprintf(f, "))");
             } else {
                 fprintf(f, "come_string_dup(");
                 emit_string_obj(f, receiver);
                 fprintf(f, ")");
             }
             return;
        }
        // Detect String methods (including len/length which are shared)
        else if (strcmp(method, "length") == 0 || strcmp(method, "len") == 0 ||
                 strcmp(method, "cmp") == 0 || strcmp(method, "casecmp") == 0 ||
                 strcmp(method, "upper") == 0 || strcmp(method, "lower") == 0 ||
                 strcmp(method, "trim") == 0 || strcmp(method, "ltrim") == 0 || strcmp(method, "rtrim") == 0 ||
                 strcmp(method, "replace") == 0 || strcmp(method, "split") == 0 ||
                 strcmp(method, "join") == 0 || strcmp(method, "substr") == 0 ||
                 strcmp(method, "view") == 0 || strcmp(method, "byte_view") == 0 ||
                 strcmp(method, "find") == 0 || strcmp(method, "rfind") == 0 || strcmp(method, "count") == 0 ||
                 strcmp(method, "chr") == 0 || strcmp(method, "rchr") == 0 || strcmp(method, "memchr") == 0 ||
                 strcmp(method, "isdigit") == 0 || strcmp(method, "isalpha") == 0 || 
                 strcmp(method, "isalnum") == 0 || strcmp(method, "isspace") == 0 || strcmp(method, "isascii") == 0 ||
                 strcmp(method, "repeat") == 0 || strcmp(method, "split_n") == 0 ||
                 strcmp(method, "regex") == 0 || strncmp(method, "regex_", 6) == 0 ||
                 strcmp(method, "chown") == 0 ||
                 strcmp(method, "tol") == 0 || strcmp(method, "tod") == 0 ||
                 strcmp(method, "toi") == 0 || strcmp(method, "tof") == 0 ||
                 strcmp(method, "utf8") == 0 ||
                 strcmp(method, "byte_array") == 0) {
            // Design 3 (redesign): views. In a storing
            // position (g_view_promote set by the
            // statement or call-argument site) a view
            // promotes to a copy, so no view variable
            // can ever dangle; elsewhere it is a true
            // zero-allocation view over the parent's
            // block. A literal receiver materializes
            // first, as for other string producers.
            if (strcmp(method, "view") == 0 || strcmp(method, "byte_view") == 0) {
                int is_byte = strcmp(method, "byte_view") == 0;
                fprintf(f, "%s(", g_view_promote ?
                    (is_byte ? "come_string_byte_range" : "come_string_substr") :
                    (is_byte ? "come_string_byte_view" : "come_string_view"));
                if (receiver->type == AST_STRING_LITERAL) {
                    fprintf(f, "come_string_new(NULL, ");
                    generate_expression(f, receiver);
                    fprintf(f, ")");
                } else {
                    generate_expression(f, receiver);
                }
                for (int i = 1; i < node->child_count; i++) {
                    fprintf(f, ", ");
                    generate_expression(f, node->children[i]);
                }
                fprintf(f, ")");
                return;
            }
            
            // Check if receiver is a map for len() - maps also have len()
            if (strcmp(method, "len") == 0 && receiver->type == AST_IDENTIFIER) {
                const char* type = get_local_variable_type(receiver->text);
                if (type && (strcmp(type, "map") == 0 || strcmp(type, "come_map_t") == 0)) {
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
                 
            if (strcmp(method, "chown") == 0) {
                // COIL-135: same slot-based re-parenting as the
                // array and map branches.
                ASTNode* a1 = node->child_count > 1 ? node->children[1] : NULL;
                const char* at = (a1 && a1->type == AST_IDENTIFIER) ? get_local_variable_type(a1->text) : NULL;
                int arg_is_buffer = at && (strchr(at, '[') != NULL || strcmp(at, "string") == 0 ||
                                           strncmp(at, "map{", 4) == 0);
                if (arg_is_buffer) {
                    fprintf(f, "come_hdr_chown_to((come_hdr_t*)&(");
                    generate_expression(f, receiver);
                    fprintf(f, "), (const come_hdr_t*)&(");
                    generate_expression(f, a1);
                    fprintf(f, "))");
                } else {
                    fprintf(f, "come_hdr_chown_ctx((come_hdr_t*)&(");
                    generate_expression(f, receiver);
                    fprintf(f, "), ");
                    if (a1) generate_expression(f, a1);
                    else fprintf(f, "COME_CTX");
                    fprintf(f, ")");
                }
                return;
            }
            if (strcmp(method, "length") == 0) {
                // A plain string's length counts CHARACTERS (spec
                // 6.5, COIL-110) - come_string_len walks the UTF-8.
                // A string LIST's length is its element count.
                int plain_string = 0;
                if (receiver->type == AST_STRING_LITERAL) plain_string = 1;
                else if (receiver->type == AST_IDENTIFIER) {
                    const char* rt = get_local_variable_type(receiver->text);
                    if (rt && strcmp(rt, "string") == 0) plain_string = 1;
                }
                else {
                    // Any other expression whose Come type is
                    // string (a map value read, a string element):
                    // COIL-135.
                    char tb[160];
                    const char* rt2 = expr_come_type(receiver, tb, sizeof(tb));
                    if (rt2 && strcmp(rt2, "string") == 0) plain_string = 1;
                }
                if (!plain_string && receiver->type == AST_METHOD_CALL) {
                    // A call that returns a plain string: string
                    // methods and the FILE string producers
                    // (gets/fname, COIL-114).
                    const char* mn = receiver->text;
                    if (strcmp(mn, "upper") == 0 || strcmp(mn, "lower") == 0 ||
                        strcmp(mn, "repeat") == 0 || strcmp(mn, "replace") == 0 ||
                        strcmp(mn, "trim") == 0 || strcmp(mn, "ltrim") == 0 ||
                        strcmp(mn, "rtrim") == 0 || strcmp(mn, "substr") == 0 ||
                        strcmp(mn, "view") == 0 ||
                        strcmp(mn, "join") == 0 || strcmp(mn, "gets") == 0 ||
                        strcmp(mn, "fname") == 0 || strcmp(mn, "str") == 0 ||
                        strcmp(mn, "where") == 0) {
                        plain_string = 1;
                    }
                }
                if (plain_string) {
                    strcpy(c_func, "come_string_len");
                } else {
                    // Parity step 3: array/list length
                    // intrinsic. come_string_list_len only
                    // returns list.count, so read the
                    // header field directly (no call).
                    // Plain strings are excluded above:
                    // their length counts characters, not
                    // bytes, so it still needs the walking
                    // call. The field is uint32_t, exactly
                    // like the call's return, so this
                    // preserves the unsigned comparison
                    // behavior (finding 38) -- it is not
                    // a fix for that finding.
                    fprintf(f, "(");
                    generate_expression(f, receiver);
                    fprintf(f, ".count)");
                    return;
                }
            }
            else if (strcmp(method, "tol") == 0) strcpy(c_func, node->child_count > 1 ? "come_string_tol_base" : "come_string_tol");
            else snprintf(c_func, sizeof(c_func), "come_string_%s", method);
        }
        // Detect Array methods
        else if (strcmp(method, "bytesize") == 0 || strcmp(method, "resize") == 0 || strcmp(method, "free") == 0 || strcmp(method, "slice") == 0) {
             if (strcmp(method, "resize") == 0 && receiver->type == AST_IDENTIFIER) {
                 const char* rt = get_local_variable_type(receiver->text);
                 if (rt && strcmp(rt, "string") == 0) {
                     codegen_error("strings are immutable: resize() does not apply to '%s' (spec 6.5)", receiver->text);
                 }
                 if (type_is_fixed_array(rt)) {
                     codegen_error("cannot resize fixed array '%s' (%s): resize() is dynamic-only - declare a dynamic array (T[]) when the length must change", receiver->text, rt);
                 }
             }
             // COIL-135: these four are emitted in full - they need
             // the element size or the header slot, which the generic
             // call tail cannot supply.
             {
                 char rtype[160] = "";
                 const char* rt = expr_come_type(receiver, rtype, sizeof(rtype));
                 if (strcmp(method, "free") == 0) {
                     fprintf(f, "come_hdr_free((come_hdr_t*)&(");
                     generate_expression(f, receiver);
                     fprintf(f, "))");
                     return;
                 }
                 if (!rt) {
                     codegen_error("'%s' needs a receiver whose type is known here", method);
                     fprintf(f, "0");
                     return;
                 }
                 if (strncmp(rt, "map{", 4) == 0) {
                     // Maps have no bytesize(): a map's byte size
                     // is an implementation detail (Come_types.md).
                     codegen_error("maps have no method '%s'", method);
                     fprintf(f, "0");
                     return;
                 }
                 if (strcmp(rt, "string") == 0) {
                     if (strcmp(method, "bytesize") == 0) {
                         fprintf(f, "((");
                         generate_expression(f, receiver);
                         fprintf(f, ").count)");
                         return;
                     }
                     codegen_error("strings have no method '%s' (spec 6.5)", method);
                     fprintf(f, "0");
                     return;
                 }
                 // An array type T[] or T[n]: the element's C type.
                 char et[96];
                 {
                     char elem[64];
                     const char* lb = strchr(rt, '[');
                     size_t el = lb ? (size_t)(lb - rt) : strlen(rt);
                     if (el >= sizeof(elem)) el = sizeof(elem) - 1;
                     memcpy(elem, rt, el);
                     elem[el] = 0;
                     if (strcmp(elem, "int") == 0) strcpy(et, "int");
                     else if (strcmp(elem, "byte") == 0) strcpy(et, "uint8_t");
                     else if (strcmp(elem, "string") == 0) strcpy(et, "come_string_t");
                     else if (strcmp(elem, "bool") == 0) strcpy(et, "bool");
                     else snprintf(et, sizeof(et), "%s", elem);
                 }
                 if (strcmp(method, "bytesize") == 0) {
                     fprintf(f, "((");
                     generate_expression(f, receiver);
                     fprintf(f, ").count * (int)sizeof(%s))", et);
                 } else if (strcmp(method, "resize") == 0) {
                     fprintf(f, "come_array_resize_g((come_hdr_t*)&(");
                     generate_expression(f, receiver);
                     fprintf(f, "), sizeof(%s), ", et);
                     if (node->child_count > 1) generate_expression(f, node->children[1]);
                     else fprintf(f, "0");
                     fprintf(f, ")");
                 } else { // slice
                     fprintf(f, "come_array_slice_g(");
                     generate_expression(f, receiver);
                     fprintf(f, ", sizeof(%s), ", et);
                     if (node->child_count > 1) generate_expression(f, node->children[1]);
                     else fprintf(f, "0");
                     fprintf(f, ", ");
                     if (node->child_count > 2) generate_expression(f, node->children[2]);
                     else fprintf(f, "0");
                     fprintf(f, ")");
                 }
                 return;
             }
        }
        else {
             // FILE receiver (COIL-114): FILE is the std module's
             // object type, not a user struct.
             if (receiver->type == AST_IDENTIFIER) {
                 const char* rt = get_local_variable_type(receiver->text);
                 if (rt && (strcmp(rt, "FILE") == 0 || strcmp(rt, "struct FILE") == 0)) {
                     if (emit_file_method_call(f, receiver->text, node)) return;
                     codegen_error("FILE has no method '%s' (Come_std.md section 2)", method);
                     fprintf(f, "0");
                     return;
                 }
             }
             // User defined struct method?
             // A method call on a struct literal (COIL-136):
             // materialize the value, then call with its
             // address as self - the same shape as a variable
             // receiver.
             if (receiver->type == AST_AGGREGATE_INIT &&
                 strncmp(receiver->text, "struct ", 7) == 0) {
                 const char* sname = receiver->text + 7;
                 if (strcmp(method, "length") == 0) {
                     fprintf(f, "%d", struct_field_count(sname));
                     return;
                 }
                 if (strcmp(method, "bytesize") == 0) {
                     fprintf(f, "((int)sizeof(struct %s))", sname);
                     return;
                 }
                 if (strcmp(method, "type") == 0) {
                     fprintf(f, "come_string_new(COME_CTX, \"struct\")");
                     return;
                 }
                 fprintf(f, "({ %s _t = ", receiver->text);
                 generate_expression(f, receiver);
                 fprintf(f, "; come_%s__%s__%s(&_t", current_module, sname, method);
                 for (int i = 1; i < node->child_count; i++) {
                     fprintf(f, ", ");
                     generate_expression(f, node->children[i]);
                 }
                 fprintf(f, "); })");
                 return;
             }

             // Try to resolve receiver variable type
             char struct_name[64] = "";
             if (receiver->type == AST_IDENTIFIER) {
                 const char* type = get_local_variable_type(receiver->text);
                 if (type) {
                     char ub2[300];
                     const char* ut = deref_ref_type(type, ub2, sizeof(ub2));
                     if (strncmp(ut, "struct ", 7) == 0) {
                         strcpy(struct_name, ut + 7);
                     } else {
                         strcpy(struct_name, ut);
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
                 if (strstr(full_type, "*") == NULL && !is_ref_type(full_type)) {
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
            if (!inst_mod) strcpy(c_func, method);
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
        
        // Arguments: a callee may retain its arguments,
        // so views here promote to copies (the receiver,
        // emitted above, is borrowed and keeps its views).
        int saved_view_promote = g_view_promote;
        g_view_promote = 1;
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
             
             // std module functions take string objects (COIL-114):
             // materialize literals for them, as for ERR.fatal.
             if (skip_receiver && receiver->type == AST_IDENTIFIER &&
                 strcmp(receiver->text, "std") == 0 && strcmp(c_func, "printf") != 0 &&
                 arg->type == AST_STRING_LITERAL) {
                    emit_string_expr(f, arg);
             // A call into an imported module (COIL-153): a
             // string literal argument materializes when the
             // callee's declared parameter is a string - the
             // conversion same-module and instance calls
             // already get. Tuple destructuring of such a
             // call rides on this too: its call is emitted
             // through this same path.
             } else if (skip_receiver && is_import && receiver->type == AST_IDENTIFIER &&
                        arg->type == AST_STRING_LITERAL &&
                        registry_param_is_string(receiver->text, method, i - 1)) {
                    emit_string_expr(f, arg);
             // Wrapper logic for string methods
             } else if ((strcmp(method, "cmp") == 0 || strcmp(method, "casecmp") == 0) && arg->type == AST_STRING_LITERAL) {
                    fprintf(f, "come_string_new(NULL, ");
                    generate_expression(f, arg);
                    fprintf(f, ")");
             } else {
                 generate_expression(f, arg);
             }
             first_arg = 0;
        }
        g_view_promote = saved_view_promote;
        
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
        
        check_call_args(node); // COIL-109 (spec 8.3)
        if (find_coroutine(node->text)) {
            codegen_error("coroutine '%s' cannot be called as a function; start it with spawn (spec 9.1)", node->text);
        }
        if (strcmp(node->text, "colink") == 0 && !find_module_fn_sig(node->text)) {
            codegen_error("colink() creates two links at once: use (a, b) = colink() (spec 9.2)");
        }
        if (strcmp(node->text, "wait") == 0 && !find_module_fn_sig(node->text)) {
            // wait(link, ...) (COIL-124, spec 9.2): pause until
            // one of the links is readable; returns that link.
            if (node->child_count == 0) {
                codegen_error("wait needs at least one link (spec 9.2)");
            }
            for (int i = 0; i < node->child_count; i++) {
                ASTNode* a = node->children[i];
                if (a->type == AST_IDENTIFIER) {
                    const char* t = get_local_variable_type(a->text);
                    if (!t || strcmp(t, "link") != 0) {
                        codegen_error("wait arguments must be links");
                    }
                }
            }
            fprintf(f, "({ come_link_t* __wait_ends[] = {");
            for (int i = 0; i < node->child_count; i++) {
                if (i) fprintf(f, ", ");
                generate_expression(f, node->children[i]);
            }
            fprintf(f, "}; come_link_wait(__wait_ends, %d); })", node->child_count);
            return;
        }
        // sprintf / sscanf are the string module's free functions
        // (COIL-117, Come_string.md). A user function of the same
        // name in this module shadows them.
        if ((strcmp(node->text, "sprintf") == 0 || strcmp(node->text, "sscanf") == 0) &&
            !find_module_fn_sig(node->text)) {
             if (strcmp(node->text, "sprintf") == 0) {
                 fprintf(f, "come_string_sprintf(COME_CTX, ");
                 if (node->child_count > 0) emit_cstr_arg(f, node->children[0]);
                 for (int i = 1; i < node->child_count; i++) {
                     fprintf(f, ", ");
                     ASTNode* a = node->children[i];
                     if (a->type == AST_IDENTIFIER && get_local_variable_type(a->text) &&
                         strcmp(get_local_variable_type(a->text), "string") == 0) {
                         emit_cstr_arg(f, a);
                     } else {
                         generate_expression(f, a);
                     }
                 }
                 fprintf(f, ")");
             } else {
                 fprintf(f, "come_string_sscanf(");
                 if (node->child_count > 0) emit_string_obj(f, node->children[0]);
                 fprintf(f, ", ");
                 if (node->child_count > 1) emit_cstr_arg(f, node->children[1]);
                 for (int i = 2; i < node->child_count; i++) {
                     fprintf(f, ", ");
                     generate_expression(f, node->children[i]);
                 }
                 fprintf(f, ")");
             }
             return;
        }
        // An unknown callee used to emit a mangled call and
        // fail in the LINKER ("undefined reference to
        // come_<module>__<name>") (COIL-129). Known callees:
        // this module's functions (with an argument-count
        // check), tuple functions, externs, function-typed
        // locals, the other files of a directory module, and
        // already-mangled internal names.
        {
            const char* csig = find_module_fn_sig(node->text);
            const char* clocal = get_local_variable_type(node->text);
            int known = csig || find_tuple_fn(node->text) ||
                        is_extern_fn(node->text) || is_group_symbol(node->text) ||
                        (clocal && is_fn_type(clocal)) ||
                        strncmp(node->text, "come_", 5) == 0 ||
                        strncmp(node->text, "std_", 4) == 0;
            if (!known) {
                codegen_error("call to undefined function '%s': no function with that name is declared in this module - check the spelling, define it, or import its module and call it as module.function", node->text);
            } else if (csig) {
                char pbuf[32][160];
                int np = call_param_types(node, pbuf, 32);
                const char* lp = strchr(csig, '(');
                const char* rp = strrchr(csig, ')');
                if (lp && rp && rp == lp + 1) np = 0; // "()" takes none
                if (np != node->child_count) {
                    codegen_error("function '%s' takes %d argument(s), %d given", node->text, np, node->child_count);
                }
            }
        }
        char mangled_name[8192];
        // Heuristic: if contains '.', it's fully qualified? No, AST_CALL text usually simple.
        // If it starts with "come_", assume already mangled/internal?
        const char* callee_local = get_local_variable_type(node->text);
        if (callee_local && is_fn_type(callee_local)) {
             // Calling through a function parameter: the parameter
             // itself is the C function pointer (COIL-109).
             strcpy(mangled_name, node->text);
        } else if (is_extern_fn(node->text)) {
             // An extern "C" function is called by its raw C name
             // (COIL-111).
             strcpy(mangled_name, node->text);
        } else if (strncmp(node->text, "come_", 5) == 0 || strncmp(node->text, "std_", 4) == 0) {
             strcpy(mangled_name, node->text);
        } else {
             // Local function call
             snprintf(mangled_name, sizeof(mangled_name), "come_%s__%s", current_module, node->text);
        }
        
        // COIL-140: a plain call to a stateful function of
        // this module passes the caller's instance along.
        // Only an instance-form body has one; anywhere else
        // the call is an error (a coroutine's isolation, or
        // a stateless function the analysis should have
        // marked - which cannot happen, so this is a guard).
        int inst_call = 0;
        if (cur_module_per_instance() &&
            codegen_fn_stateful(current_module, node->text, NULL)) {
            if (g_in_coroutine)
                codegen_error("coroutine cannot call '%s': it uses module state, and coroutines are isolated from module state (spec 9.1)", node->text);
            else if (!g_fn_instance_form)
                codegen_error("'%s' uses module state: it can only be called from another stateful function of '%s', or on an instance (var x = %s.create())", node->text, current_module, current_module);
            else
                inst_call = 1;
        }
        fprintf(f, "%s(", mangled_name);
        if (inst_call) fprintf(f, "__inst");
        {
            // A string literal is a char* in C; where the callee's
            // parameter is a string it must be materialized as a
            // come_string_t first - passing the raw literal made
            // the callee read string headers out of rodata and
            // crash the caller (COIL-133).
            char cparts[32][160];
            int cnp = call_param_types(node, cparts, 32);
            int saved_view_promote2 = g_view_promote;
            g_view_promote = 1; // Design 3 (redesign): call arguments may be retained
            for (int i = 0; i < node->child_count; i++) {
                if (i > 0 || inst_call) fprintf(f, ", ");
                if (i < cnp && strcmp(cparts[i], "string") == 0 &&
                    node->children[i]->type == AST_STRING_LITERAL)
                    emit_string_expr(f, node->children[i]);
                else if (i < cnp)
                    emit_ref_or_value_arg(f, node->children[i], cparts[i], node->text);
                else
                    generate_expression(f, node->children[i]);
            }
            g_view_promote = saved_view_promote2;
        }
        fprintf(f, ")");
    } else if (node->type == AST_AGGREGATE_INIT) {
        // { val, val } or { .field = val, ... }
        if (strncmp(node->text, "struct ", 7) == 0) {
            // Typed struct literal (COIL-136): a C compound
            // literal. A nested bare { } would be a buffer
            // field initialized inline, which a raw C
            // initializer cannot build - reject it.
            for (int ci = 0; ci < node->child_count; ci++) {
                ASTNode* ch2 = node->children[ci];
                ASTNode* val = (ch2->type == AST_ASSIGN && ch2->child_count >= 2)
                                   ? ch2->children[1] : ch2;
                if (val->type == AST_AGGREGATE_INIT &&
                    strncmp(val->text, "struct ", 7) != 0) {
                    codegen_error("a buffer field in a struct literal needs a variable: build the buffer first, then use it in the literal");
                    break;
                }
            }
            fprintf(f, "(%s)", node->text);
        }
        // An array literal in a general expression position
        // (COIL-145): materialize the header value instead of
        // pasting the raw braces, which are not a C value.
        // The declaration path sets g_raw_aggregate where it
        // needs the braces for its own C array or struct.
        if (strncmp(node->text, "struct ", 7) != 0 && !g_raw_aggregate &&
            !(node->child_count > 0 && node->children[0]->type == AST_MAP_PAIR)) {
            emit_array_literal_expr(f, node);
            return;
        }
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
        // String ternaries must agree on come_string_t on
        // both branches; a raw literal branch would decay to
        // char* and corrupt the result (spec 14, COIL-127).
        int str_tern = expr_is_stringish(node->children[1]) ||
                       expr_is_stringish(node->children[2]);
        fprintf(f, "(");
        generate_expression(f, node->children[0]);
        fprintf(f, " ? ");
        if (str_tern) emit_string_expr(f, node->children[1]);
        else generate_expression(f, node->children[1]);
        fprintf(f, " : ");
        if (str_tern) emit_string_expr(f, node->children[2]);
        else generate_expression(f, node->children[2]);
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
                // Check if either operand is a string
                int left_is_str = 0, right_is_str = 0;

                if (left->type == AST_IDENTIFIER) {
                    const char* type = get_local_variable_type(left->text);
                    if (type && (strcmp(type, "string") == 0 || strcmp(type, "come_string_t") == 0)) {
                        left_is_str = 1;
                    }
                }

                if (right->type == AST_STRING_LITERAL || right->type == AST_IDENTIFIER) {
                    if (right->type == AST_STRING_LITERAL) {
                        right_is_str = 1;
                    } else {
                        const char* type = get_local_variable_type(right->text);
                        if (type && (strcmp(type, "string") == 0 || strcmp(type, "come_string_t") == 0)) {
                            right_is_str = 1;
                        }
                    }
                }

                is_string_cmp = (left_is_str || right_is_str);

                // Spec 14: == on composite values tests
                // identity (same buffer), not contents - so
                // two string VARIABLES compare by pointer.
                // A literal has no buffer identity, so a
                // comparison involving a literal keeps the
                // content comparison (COIL-127).
                if (left->type == AST_IDENTIFIER && right->type == AST_IDENTIFIER &&
                    left_is_str && right_is_str) {
                    is_string_cmp = 0;
                }
            }
        }
        
        // Buffer identity / null-vs-buffer (COIL-135): two
        // buffer values are == when they share a data block;
        // a buffer compared with null tests its data pointer.
        // Only fires when both sides resolve to buffer types,
        // so string-vs-literal keeps the content path below.
        int buf_identity = 0, null_buf = 0;
        if (is_eq || is_neq) {
            char ob1[160], ob2[160];
            const char* t1 = expr_come_type(left, ob1, sizeof(ob1));
            const char* t2 = expr_come_type(right, ob2, sizeof(ob2));
            int b1 = t1 && (strcmp(t1, "string") == 0 || strchr(t1, '[') || strncmp(t1, "map{", 4) == 0);
            int b2 = t2 && (strcmp(t2, "string") == 0 || strchr(t2, '[') || strncmp(t2, "map{", 4) == 0);
            if ((left_is_null && b2) || (right_is_null && b1)) null_buf = 1;
            else if (!left_is_null && !right_is_null && b1 && b2) buf_identity = 1;
        }

        if (strcmp(node->text, "+") == 0 &&
            (expr_is_stringish(left) || expr_is_stringish(right))) {
            // String concatenation (spec 6.5): `+` on strings
            // produces a new string; C's + cannot do this.
            // Design 2 (redesign): a concat in a temporary
            // position (flag clear) goes to the bump arena
            // - it dies with its frame, so no talloc chunk
            // or adoption bookkeeping is needed. In a
            // storing position (flag set) it keeps the
            // talloc concat, so the value can outlive
            // its scope.
            if (g_view_promote) fprintf(f, "come_string_concat(COME_CTX, ");
            else fprintf(f, "come_string_concat_arena(");
            emit_string_expr(f, left);
            fprintf(f, ", ");
            emit_string_expr(f, right);
            fprintf(f, ")");
        } else if (buf_identity || null_buf) {
            if (null_buf) {
                ASTNode* other = left_is_null ? right : left;
                fprintf(f, "((");
                generate_expression(f, other);
                fprintf(f, ").data %s NULL)", is_eq ? "==" : "!=");
            } else {
                fprintf(f, "((");
                generate_expression(f, left);
                fprintf(f, ").data %s (", is_eq ? "==" : "!=");
                generate_expression(f, right);
                fprintf(f, ").data)");
            }
        } else if (is_string_cmp) {
            // Content comparison against a literal (either
            // side): both operands go through emit_string_expr
            // so variables pass as-is and only literals are
            // materialized (COIL-127).
            fprintf(f, "(come_string_cmp(");
            emit_string_expr(f, node->children[0]);
            fprintf(f, ", ");
            emit_string_expr(f, node->children[1]);
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
            int saved_view_promote3 = g_view_promote;
            g_view_promote = 1; // Design 3 (redesign): call arguments may be retained
            for (int i=0; i < node->child_count; i++) {
                 generate_expression(f, node->children[i]);
                 if (i < node->child_count - 1) fprintf(f, ", ");
            }
            g_view_promote = saved_view_promote3;
            fprintf(f, ")");
        }
    }
}

static void generate_node(FILE* f, ASTNode* node, int indent);

// === Module instances (COIL-140) ===
static int cur_module_has_state(void) {
    return codegen_module_has_state(current_module);
}

// The lifecycle hooks are named after the operations
// that run them (COIL-155): a module's create hook runs
// when it is set up - at program start for a singleton,
// per instance for a stateful module - and its destroy
// hook when it is torn down. The pre-COIL-155 names were
// init/exit. The generated C wrappers keep their
// historical names (__init_local/__exit_local).
static int fn_is_hook(const char* n) {
    return strcmp(n, "create") == 0 || strcmp(n, "module_create") == 0 ||
           strcmp(n, "destroy") == 0 || strcmp(n, "module_destroy") == 0;
}

// A function takes the instance form when its module has
// state and the analysis marked it stateful (lifecycle
// hooks always do). Coroutines never do: their isolation
// rule forbids module state outright.
static int fn_instance_form(ASTNode* fn) {
    if (!fn || fn->is_coroutine) return 0;
    if (!cur_module_per_instance()) return 0;
    return codegen_fn_stateful(current_module, fn->text, NULL) ||
           fn_is_hook(fn->text);
}

// The return statement for a call on a destroyed instance:
// the zero value of the function's return type.
static void emit_instance_zero_return(FILE* f, ASTNode* fn) {
    ASTNode* ret = fn->children[0];
    if (!ret || strcmp(ret->text, "void") == 0) {
        fprintf(f, "return;");
        return;
    }
    if (ret->text[0] == '(') {
        fprintf(f, "return (come_%s__%s_ret_t){0};", current_module, fn->text);
        return;
    }
    if (codegen_module_can_instance(ret->text)) {
        fprintf(f, "return NULL;");
        return;
    }
    char ct[300];
    come_type_to_c(ret->text, ct, sizeof(ct));
    fprintf(f, "return (%s){0};", ct);
}

// The guard every instance-form function opens with: a
// destroyed (or never created) instance sets ERR and the
// call returns the zero value - using one is always safe.
static void emit_instance_guard(FILE* f, ASTNode* fn, int indent) {
    emit_indent(f, indent);
    fprintf(f, "if (!__inst || !__inst->alive) { come_ERR_set(EBADF, \"module instance has been destroyed\"); ");
    emit_instance_zero_return(f, fn);
    fprintf(f, " }\n");
    // COIL-157: no context local here anymore. Buffers a
    // method creates belong to its scope (COME_CTX is the
    // scope stack); buffers it stores in a field are
    // adopted to __inst->ctx by the assignment path.
}

static void generate_program(FILE* f, ASTNode* node) {
    for (int i = 0; i < node->child_count; i++) {
        g_module_level = 1;
        generate_node(f, node->children[i], 0);
        g_module_level = 0;
        fputc('\n', f);
    }
}

static void generate_node(FILE* f, ASTNode* node, int indent) {
    if (!node) return;

    // Design 3 (redesign): views promote at storing
    // positions; a statement starts unpromoted, so a
    // flag set by one statement never leaks into the
    // next (nested statements reset on entry too, and
    // call arguments save/restore around themselves).
    g_view_promote = 0;

    // COIL-139: arm the ERR pending location with this
    // statement's source site (function bodies only).
    // COIL-157: the arm carries its own forced #line:
    // without it, #line arithmetic attributes the arm to
    // whatever line the previous statement's expansion
    // happened to end on, and line coverage credits the
    // wrong branch (a taken branch's arm landed on the
    // next branch's line).
    // A4 (redesign): the #line stays unconditional (OOB
    // slots read their location from it); the arm fires
    // only before statements that may set an error.
    if (g_in_fn_body && node->source_line > 0 &&
        node->type != AST_BLOCK && node->type != AST_FUNCTION &&
        node->type != AST_CASE && node->type != AST_DEFAULT &&
        node->type != AST_FALLTHROUGH) {
        emit_line_directive_forced(f, node);
        if (codegen_stmt_may_fail(node)) emit_err_arm(f, node, indent);
    }

    switch (node->type) {
        case AST_PROGRAM:
            generate_program(f, node);
            break;


      case AST_EXPORT:
          // Ignore exports in C codegen, visibility handled by C static/extern rules or just everything is visible for now
          break;

      case AST_FUNCTION: {
        // [RetType] [Name] [Args...] [Block/Body]
        if (node->is_extern) return; // COIL-111: prototype only (forward pass)
        g_module_level = 0; // COIL-140: a function body is never module level
        g_in_coroutine = node->is_coroutine ? 1 : 0; // COIL-122
        emit_line_directive(f, node);

        reset_local_variables();
        g_fn_const_count = 0; // COIL-129
        g_break_depth = 0;
        g_continue_depth = 0;
        // Register arguments
        // Children: 0=ret, 1..=args (until block)
        for (int i = 1; i < node->child_count; i++) {
             ASTNode* child = node->children[i];
             if (child->type == AST_VAR_DECL) {
                 if (child->child_count > 1) {
                     register_local_type(child->text, child->children[1]);
                 }
             }
             if (child->type == AST_BLOCK) break;
        }
        
        ASTNode* ret_type = node->children[0];
        if (codegen_module_can_instance(ret_type->text)) {
            codegen_error("functions cannot return a module instance in this version: create the instance in the caller, or pass it in as a parameter");
        }
        int body_idx = node->child_count - 1;

        strncpy(current_function_name, node->text, sizeof(current_function_name) - 1);
        current_function_name[sizeof(current_function_name) - 1] = '\0';
        // Parity step 1: no proof carries across functions.
        bound_proof_clear();
        // Unchecked: lexical flag starts from the
        // build flag per function (never leaks).
        g_unchecked = g_codegen_unchecked;
        // Interval slice: no nonneg carries across
        // functions either (a leaked `i` would prove
        // a parameter index nonneg in the next fn).
        g_nonneg_count = 0;
        g_cur_tuple = NULL;
        if (ret_type->text[0] == '(') {
            // Tuple return (spec 8.2): the C function returns
            // the generated struct by value.
            strncpy(current_function_return_type, ret_type->text, sizeof(current_function_return_type) - 1);
            current_function_return_type[sizeof(current_function_return_type) - 1] = '\0';
            g_cur_tuple = find_tuple_fn(node->text);
        } else {
            strncpy(current_function_return_type, ret_type->text, sizeof(current_function_return_type) - 1);
            current_function_return_type[sizeof(current_function_return_type) - 1] = '\0';
        }
        
        int is_main = (strcmp(node->text, "main") == 0 && !node->is_coroutine);
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
        if (strcmp(node->text, "create") == 0 || strcmp(node->text, "module_create") == 0) {
             snprintf(func_name, sizeof(func_name), "come_%s__init_local", current_module);
        } else if (strcmp(node->text, "destroy") == 0 || strcmp(node->text, "module_destroy") == 0) {
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


        if (node->is_coroutine) strcat(func_name, "__impl");

        // COIL-140: instance-form functions take the
        // instance as a hidden first parameter.
        int inst_form = fn_instance_form(node);

        emit_indent(f, indent);

        // Return type
        if (ret_type->text[0] == '(') {
            fprintf(f, "come_%s__%s_ret_t %s(", current_module, node->text, func_name);
        } else {
            // Through the type translator (COIL-151): a
            // map{...} return is come_map_t, never the raw
            // Come spelling pasted into C.
            char ct[300];
            come_type_to_c(ret_type->text, ct, sizeof(ct));
            fprintf(f, "%s %s(", ct, func_name);
        }
        
        // Args
        int has_args = 0;

        if (inst_form) {
            fprintf(f, "come_%s__instance_t* __inst", current_module);
            has_args = 1;
        }
        
        // Special case: nport injector
        if (strcmp(node->text, "nport") == 0) {
            fprintf(f, "struct TCP_ADDR* self");
            has_args = 1;
        }

        
        // Iterate manual args
        for (int i = 1; i < body_idx; i++) {
            if (has_args) fprintf(f, ", ");
            
            ASTNode* arg = node->children[i];
            if (arg->type == AST_VAR_DECL) {
                // int x
                ASTNode* type = arg->children[1];

                if (is_fn_type(type->text)) {
                    // Function parameter (COIL-109): RET (*name)(ARGS)
                    emit_fn_param(f, type->text, arg->text);
                // array?
                } else if (strstr(type->text, "[]")) {
                    // int input[] -> come_int_array_t input
                     char raw[64];
                     strncpy(raw, type->text, strlen(type->text)-2);
                     raw[strlen(type->text)-2] = 0;
                     if (strcmp(raw, "int")==0) fprintf(f, "come_int_array_t %s", arg->text);
                     else if (strcmp(raw, "byte")==0) fprintf(f, "come_byte_array_t %s", arg->text);
                     else if (strcmp(raw, "string")==0) fprintf(f, "come_string_list_t %s", arg->text);
                     else fprintf(f, "come_array_t %s", arg->text);
                } else if (is_main && strncmp(arg->text, "args", 4) == 0 && (strcmp(type->text, "string") == 0 || strcmp(type->text, "string[]") == 0)) {
                    // special case for main(string args) -> we pass string list
                    fprintf(f, "come_string_list_t %s", arg->text);
                } else if (strcmp(type->text, "link") == 0) {
                   fprintf(f, "come_link_t* %s", arg->text);
                } else if (codegen_module_can_instance(type->text)) {
                   // A module instance parameter (COIL-140):
                   // the module's name is the type.
                   fprintf(f, "come_%s__instance_t* %s", type->text, arg->text);
                } else if (is_ref_type(type->text)) {
                   // &(struct Vec3) p -> struct Vec3* p (COIL-136)
                   char ct[300];
                   come_type_to_c(type->text, ct, sizeof(ct));
                   fprintf(f, "%s %s", ct, arg->text);
                } else {
                   // Through the type translator (COIL-151):
                   // map{...} parameters are come_map_t.
                   char ct[300];
                   come_type_to_c(type->text, ct, sizeof(ct));
                   fprintf(f, "%s %s", ct, arg->text);
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

            if (inst_form) emit_instance_guard(f, node, indent + 4);
            int prev_in_fn = g_in_fn_body;
            int prev_inst = g_fn_instance_form;
            int prev_frameless = g_cur_fn_frameless;
            g_in_fn_body = 1;
            g_fn_instance_form = inst_form;
            // A5: main, coroutines, and instance-form
            // functions always keep a frame; the rest ask
            // the per-module frameless set.
            g_cur_fn_frameless = (!is_main && !node->is_coroutine && !inst_form &&
                                  codegen_is_frameless(current_module, node->text)) ? 1 : 0;
            // COIL-157: the function body is the marker
            // frame. It must be pushed after the guard
            // (the guard returns early, before any scope
            // exists) and popped on every exit: returns
            // pop to the marker themselves, and the
            // fallthrough path pops here.
            g_scope_depth = 0;
            g_local_add_depth = 0;
            g_loop_depth_count = 0;
            if (!g_cur_fn_frameless) {
                emit_indent(f, indent + 4);
                fprintf(f, "come_scope_push_marker();\n");
            }
            generate_block_children(f, body, indent + 4);
            // A5: the fallthrough pop below runs after the
            // flag is restored, so keep this function's
            // value in a local first.
            int fn_frameless = g_cur_fn_frameless;
            g_fn_instance_form = prev_inst;
            g_in_fn_body = prev_in_fn;
            g_cur_fn_frameless = prev_frameless;
            // COIL-157: a body ending in a return never
            // falls through, so the fallthrough pop (and
            // main's return 0) would be dead code - and
            // dead code still confuses line coverage.
            if (body->child_count == 0 ||
                body->children[body->child_count - 1]->type != AST_RETURN) {
                if (is_main) {
                    if (!fn_frameless) {
                        emit_indent(f, indent + 4);
                        fprintf(f, "come_scope_pop_to_marker();\n");
                    }
                    emit_indent(f, indent + 4);
                    fprintf(f, "return 0;\n");
                } else {
                    if (!fn_frameless) {
                        emit_indent(f, indent + 4);
                        fprintf(f, "come_scope_pop_to_marker();\n");
                    }
                }
            }
            emit_indent(f, indent);
            fprintf(f, "}\n");
        } else {
            fprintf(f, ";\n");
        }

        if (node->is_coroutine) {
            // Scheduler entry point (COIL-120): unpacks the
            // args struct and calls the implementation.
            fprintf(f, "void come_%s__%s__entry(void* come_args) {\n", current_module, node->text);
            if (fn_param_count(node) > 0) {
                fprintf(f, "    come_%s__%s__args_t* a = (come_%s__%s__args_t*)come_args;\n",
                        current_module, node->text, current_module, node->text);
                fprintf(f, "    %s(", func_name);
                int first = 1;
                for (int i = 1; i < body_idx; i++) {
                    ASTNode* arg = node->children[i];
                    if (arg->type != AST_VAR_DECL) continue;
                    if (!first) fprintf(f, ", ");
                    fprintf(f, "a->%s", arg->text);
                    first = 0;
                }
                fprintf(f, ");\n");
            } else {
                fprintf(f, "    %s();\n", func_name);
            }
            fprintf(f, "}\n");
        }

        return;
    }
    
    case AST_TYPE_ALIAS: {
        // Handled in Pass -1
        // fprintf(f, "typedef %s %s;\n", node->children[0]->text, node->text);
        break;
    }

    
    case AST_VAR_DECL: {
        // Design 3 (redesign): a declaration initializer
        // stores its value, so a view there promotes to
        // a copy (no view variable ever exists).
        // Design 1+2 (redesign), straight-line slice: a
        // transient local's concat initializer does NOT
        // store (it never escapes its scope), so it stays
        // in the bump arena. Module-level declarations
        // always promote: a module variable outlives any
        // frame, so an arena value there would dangle.
        if (g_module_level ||
            !codegen_transient_local(current_module, current_function_name, node->text))
            g_view_promote = 1;
        emit_line_directive(f, node);  // Emit #line for variable declaration
        ASTNode* type_node = node->children[1];
        register_local_type(node->text, type_node);

        ASTNode* init_expr = node->children[0];

        // A `var` with an initializer locks to the inferred
        // type right here (spec 10.1, COIL-127): downstream
        // code must see the real type, not "var".
        if (strcmp(type_node->text, "var") == 0 && init_expr) {
            const char* inferred = infer_assign_type(init_expr);
            if (inferred) update_local_variable_type(node->text, inferred);
        }

        // COIL-140: in a stateful module, a module-level
        // variable emits no C global - it lives in the
        // module's instances (the state home file re-emits
        // its initializer in __init_fields).
        if (g_module_level && cur_module_can_instance() &&
            codegen_module_var_index(current_module, node->text, NULL) >= 0) {
            break;
        }

        // COIL-142: in a singleton module, a module-level
        // header-typed variable (string, array, map) emits
        // the bare global only. Its usual declaration
        // initializer calls the runtime, which C forbids at
        // file scope ("initializer element is not
        // constant"); a zeroed header is a live empty value,
        // so the variable is ready to fill in init() or any
        // function. A written initializer gets a clear error
        // here instead of the broken C.
        if (g_module_level && !cur_module_can_instance() &&
            codegen_module_var_index(current_module, node->text, NULL) >= 0) {
            const char* et = type_node->text;
            if (strcmp(et, "var") == 0) {
                const char* lt = get_local_variable_type(node->text);
                if (lt) et = lt;
            }
            if (is_header_type(et)) {
                if (init_expr && !(init_expr->type == AST_NUMBER &&
                                    strcmp(init_expr->text, "0") == 0)) {
                    codegen_error("module-level %s '%s' cannot have an initializer in a singleton module: its value is built by the runtime, which cannot run at file scope - declare it bare and assign it in create() or main()", et, node->text);
                }
                char ct[160];
                header_c_type(et, ct, sizeof(ct));
                emit_indent(f, indent);
                fprintf(f, "%s %s;\n", ct, node->text);
                break;
            }
        }

        // COIL-140: a module instance variable - declared
        // with the module's name as its type, or inferred
        // from a create() call. The C value is the instance
        // pointer; the local's recorded type marks it so
        // method calls dispatch as instance calls.
        {
            const char* imod = NULL;
            if (codegen_module_can_instance(type_node->text)) imod = type_node->text;
            else if (init_expr && init_expr->type == AST_METHOD_CALL &&
                     strcmp(init_expr->text, "create") == 0 &&
                     init_expr->child_count > 0 &&
                     init_expr->children[0]->type == AST_IDENTIFIER &&
                     codegen_module_can_instance(init_expr->children[0]->text))
                imod = init_expr->children[0]->text;
            // COIL-141: an instance is created inside a
            // function only. At module scope there is no
            // moment to run create(): the declaration used
            // to fall through and emit the call as a C
            // file-scope initializer, which C rejects
            // ("initializer element is not constant").
            // Say so here, in Come's own words.
            if (imod && g_module_level) {
                codegen_error("a module instance can only be created inside a function: '%s' is declared at module scope - declare it inside a function and pass it as a parameter where it is needed", node->text);
                break;
            }
            if (imod && !g_module_level) {
                static char ibuf[320];
                snprintf(ibuf, sizeof(ibuf), "instance:%.80s", imod);
                update_local_variable_type(node->text, ibuf);
                emit_indent(f, indent);
                fprintf(f, "come_%s__instance_t* %s = ", imod, node->text);
                if (init_expr && !(init_expr->type == AST_NUMBER &&
                                   strcmp(init_expr->text, "0") == 0))
                    generate_expression(f, init_expr);
                else
                    fprintf(f, "NULL");
                fprintf(f, ";\n");
                break;
            }
        }

        if (is_ref_type(type_node->text)) {
            // A reference variable (COIL-136): a C pointer,
            // always initialized from &value or from another
            // reference variable.
            emit_indent(f, indent);
            char ct[300];
            come_type_to_c(type_node->text, ct, sizeof(ct));
            fprintf(f, "%s %s = ", ct, node->text);
            if (!init_expr || (init_expr->type == AST_NUMBER &&
                               strcmp(init_expr->text, "0") == 0)) {
                codegen_error("a reference variable must be initialized: %s %s = &value", type_node->text, node->text);
                fprintf(f, "NULL");
            } else {
                emit_ref_or_value_arg(f, init_expr, type_node->text, node->text);
            }
            fprintf(f, ";\n");
            break;
        }

        if (init_expr && init_expr->type == AST_IDENTIFIER && type_is_array(type_node->text)) {
            const char* src_type = get_local_variable_type(init_expr->text);
            if (src_type) check_array_transfer("initialize", node->text, type_node->text, init_expr->text, src_type);
        }
        if (init_expr) check_numeric_store(type_node->text, init_expr, node->text); // COIL-107
        if (init_expr && expr_is_fn_value(init_expr))
            codegen_error("functions can only be passed as arguments (spec 8.3): '%s' cannot initialize '%s'", init_expr->text, node->text);
        
        emit_indent(f, indent);
            if (strcmp(type_node->text, "string") == 0) {
                fprintf(f, "come_string_t %s = ", node->text);
                if (init_expr->type == AST_NUMBER && strcmp(init_expr->text, "0") == 0) {
                    fprintf(f, "{0}");
                } else if (init_expr->type == AST_STRING_LITERAL) {
                    fprintf(f, "come_string_new(COME_CTX, ");
                    generate_expression(f, init_expr);
                    fprintf(f, ")");
                } else {
                    emit_expr_as(f, init_expr, "come_string_t");
                }
                fprintf(f, ";\n");
            } else if (strcmp(type_node->text, "string[]") == 0) {
                if (init_expr && init_expr->type == AST_AGGREGATE_INIT) {
                    // String array literal (COIL-100): a header value
                    // (COIL-135) grown by appends, one owned string
                    // per element.
                    int count = init_expr->child_count;
                    fprintf(f, "come_string_list_t %s = COME_HDR_INIT(COME_CTX);\n", node->text);
                    for (int ei = 0; ei < count; ei++) {
                        ASTNode* elem = init_expr->children[ei];
                        emit_indent(f, indent);
                        fprintf(f, "{ come_string_t _e = ");
                        if (elem->type == AST_STRING_LITERAL) {
                            fprintf(f, "come_string_new(COME_CTX, ");
                            generate_expression(f, elem);
                            fprintf(f, ")");
                        } else {
                            generate_expression(f, elem);
                        }
                        fprintf(f, "; come_array_append_g((come_hdr_t*)&%s, sizeof(come_string_t), &_e); }\n", node->text);
                    }
                } else {
                fprintf(f, "come_string_list_t %s = ", node->text);
                if (init_expr->type == AST_NUMBER && strcmp(init_expr->text, "0") == 0) {
                    fprintf(f, "{0}");
                } else if (init_expr->type == AST_STRING_LITERAL && strcmp(init_expr->text, "\"__ARGS__\"") == 0) {
                    fprintf(f, "come_string_list_from_argv(COME_CTX, argc, argv)");
                } else {
                    generate_expression(f, init_expr);
                }
                fprintf(f, ";\n");
                }
                // Mark as potentially unused to avoid warnings
                emit_indent(f, indent);
                fprintf(f, "(void)%s;\n", node->text);
            } else if (strncmp(type_node->text, "map{", 4) == 0) {
                // Typed map declaration (COIL-102)
                char mkt[160], mvt[160];
                if (!map_type_parts(type_node->text, mkt, sizeof(mkt), mvt, sizeof(mvt))) {
                    codegen_error("malformed map type '%s'", type_node->text);
                    strcpy(mkt, "string");
                    strcpy(mvt, "int");
                } else if (!map_combo_supported(mkt, mvt)) {
                    map_combo_error(mkt, mvt);
                }
                if (init_expr && init_expr->type == AST_IDENTIFIER) {
                    fprintf(f, "come_map_t %s = ", node->text);
                    generate_expression(f, init_expr);
                    fprintf(f, ";\n");
                } else if (init_expr && init_expr->type != AST_AGGREGATE_INIT &&
                           !(init_expr->type == AST_NUMBER && strcmp(init_expr->text, "0") == 0)) {
                    // Expression init (dup(), a call returning a map, ...)
                    fprintf(f, "come_map_t %s = ", node->text);
                    emit_expr_as(f, init_expr, "come_map_t");
                    fprintf(f, ";\n");
                } else {
                    fprintf(f, "come_map_t %s = come_map_new(COME_CTX);\n", node->text);
                    if (init_expr && init_expr->type == AST_AGGREGATE_INIT) {
                        for (int pi = 0; pi < init_expr->child_count; pi++) {
                            ASTNode* pair = init_expr->children[pi];
                            if (pair->type != AST_MAP_PAIR || pair->child_count < 2) {
                                codegen_error("map literal for '%s' must use key: value pairs", node->text);
                                continue;
                            }
                            check_map_expr_type(mkt, pair->children[0], "key");
                            check_map_expr_type(mvt, pair->children[1], "value");
                            emit_indent(f, indent);
                            emit_map_put(f, NULL, node->text, mvt, pair->children[0], pair->children[1]);
                        }
                    }
                }
            } else if (strcmp(type_node->text, "FILE") == 0) {
                // FILE object (COIL-114): the std runtime type.
                // Zero-initialized (unopened) or from an initializer
                // expression such as std.tmpfile().
                if (init_expr && !(init_expr->type == AST_NUMBER && strcmp(init_expr->text, "0") == 0)) {
                    fprintf(f, "come_std__FILE_t %s = ", node->text);
                    generate_expression(f, init_expr);
                    fprintf(f, ";\n");
                } else {
                    fprintf(f, "come_std__FILE_t %s = {0};\n", node->text);
                }
            } else if (strcmp(type_node->text, "link") == 0) {
                // Channel end (COIL-123, spec 6.7): from colink
                // (the destructure statement), wait(), or another
                // link variable.
                if (init_expr && !(init_expr->type == AST_NUMBER && strcmp(init_expr->text, "0") == 0)) {
                    fprintf(f, "come_link_t* %s = ", node->text);
                    if (is_link_read_expr(init_expr)) emit_link_read_raw(f, init_expr);
                    else generate_expression(f, init_expr);
                    fprintf(f, ";\n");
                } else {
                    fprintf(f, "come_link_t* %s = NULL;\n", node->text);
                }
            } else if (strcmp(type_node->text, "bool") == 0) {
                fprintf(f, "bool %s = ", node->text);
                generate_expression(f, init_expr);
                fprintf(f, ";\n");
            } else if (strcmp(type_node->text, "var") == 0) {
                // Type inference
                if (init_expr->type == AST_STRING_LITERAL) {
                    fprintf(f, "come_string_t %s = come_string_new(COME_CTX, ", node->text);
                    generate_expression(f, init_expr);
                    fprintf(f, ");\n");
                } else if (is_link_read_expr(init_expr)) {
                    fprintf(f, "come_string_t %s = ", node->text);
                    emit_expr_as(f, init_expr, "come_string_t");
                    fprintf(f, ";\n");
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
                    
                    // An uninitialized declaration carries a
                    // synthetic 0 init; for arrays that means
                    // "no initializer", not the value 0 -
                    // taking it as an expression init left
                    // fixed arrays NULL (COIL-127).
                    int zero_init = init_expr && init_expr->type == AST_NUMBER &&
                                    strcmp(init_expr->text, "0") == 0;
                    int eff_fixed = fixed_size > 0 ? fixed_size : type_node->array_size;
                    if (init_expr && init_expr->type == AST_AGGREGATE_INIT) {
                        int count = init_expr->child_count;
                        int alloc_count = (eff_fixed > count) ? eff_fixed : count;

                        // Header value (COIL-135); the data block is
                        // sized for the literal and zero-filled past
                        // the given elements.
                        fprintf(f, "%s %s = COME_HDR_INIT(COME_CTX);\n", arr_type, node->text);
                        emit_indent(f, indent);
                        fprintf(f, "{ %s _vals[] = ", elem_type);
                        g_raw_aggregate = 1;
                        generate_expression(f, init_expr);
                        g_raw_aggregate = 0;
                        fprintf(f, "; come_array_ensure((come_hdr_t*)&%s, sizeof(%s), %u); memcpy(%s.data, _vals, sizeof(_vals)); %s.count = %u; }\n",
                                node->text, elem_type, alloc_count, node->text, node->text, count);
                    } else if (init_expr && !zero_init) {
                        // Initialized from expression (e.g. slice, function return)
                        fprintf(f, "%s %s = ", arr_type, node->text);
                        generate_expression(f, init_expr);
                        fprintf(f, ";\n");
                    } else if (eff_fixed > 0) {
                        // Fixed array: a header over a data block of
                        // exactly eff_fixed zeroed elements; it can
                        // never grow (the compiler forbids it).
                        fprintf(f, "%s %s = COME_HDR_INIT(COME_CTX);\n", arr_type, node->text);
                        emit_indent(f, indent);
                        fprintf(f, "%s.data = mem_talloc_alloc(COME_CTX, %u * sizeof(%s)); memset(%s.data, 0, %u * sizeof(%s)); %s.cap = %u; %s.count = %u;\n",
                                node->text, eff_fixed, elem_type, node->text, eff_fixed, elem_type,
                                node->text, eff_fixed, node->text, eff_fixed);
                    } else {
                        // Empty dynamic: a zeroed header, no allocation
                        // until the first growth (COIL-135).
                        fprintf(f, "%s %s = COME_HDR_INIT(COME_CTX);\n", arr_type, node->text);
                    }
                }
 else {
                     if (strcmp(type_node->text, "var")==0) {
                         fprintf(f, "int %s = ", node->text);
                     } else {
                         fprintf(f, "%s %s = ", type_node->text, node->text);
                     }
                     
                     // For struct types with aggregate initializers, preserve the syntax
                     if (init_expr && init_expr->type == AST_AGGREGATE_INIT &&
                         strncmp(type_node->text, "struct", 6) == 0) {
                         // Just emit the aggregate initializer as-is for structs
                         g_raw_aggregate = 1;
                         generate_expression(f, init_expr);
                         g_raw_aggregate = 0;
                     } else if (init_expr && init_expr->type == AST_NUMBER && strcmp(init_expr->text, "0") == 0) {
                          // Struct values zero-initialize with {0}
                          // (COIL-135: a struct whose first field is
                          // itself a header struct cannot take = 0).
                          const char* tn = type_node->text;
                          if (strncmp(tn, "struct ", 7) == 0) tn += 7;
                          if (strncmp(type_node->text, "struct", 6) == 0 || strncmp(type_node->text, "union", 5) == 0 ||
                              struct_field_count(tn) > 0) {
                              fprintf(f, "{0}");
                          } else {
                              generate_expression(f, init_expr);
                          }
                     } else if (init_expr && init_expr->type == AST_AGGREGATE_INIT &&
                                strcmp(type_node->text, "map") == 0) {
                         // A legacy map's {} is the raw C
                         // empty initializer (COIL-145: not an
                         // array literal).
                         g_raw_aggregate = 1;
                         generate_expression(f, init_expr);
                         g_raw_aggregate = 0;
                     } else {
                         generate_expression(f, init_expr);
                     }
                     fprintf(f, ";\n");
                }
            }
            break;
        }

    case AST_PRINTF: {
            emit_indent(f, indent);
            fputs("printf(", f);
            emit_c_string_literal(f, node->text);
            // Design 3 (redesign): printf %s reads to NUL,
            // which a view lacks at its end, so view
            // arguments promote to copies here.
            int saved_view_promote_stmt_printf = g_view_promote;
            g_view_promote = 1;
            
            for (int i = 0; i < node->child_count; i++) {
                fputs(", ", f);
                ASTNode* arg = node->children[i];
                
                if (arg->type == AST_STRING_LITERAL) {
                    emit_c_string_literal(f, arg->text);
                } else if (arg->type == AST_IDENTIFIER) {
                    const char* type = get_local_variable_type(arg->text);
                    int is_str = (type && (strcmp(type, "string") == 0 || strcmp(type, "come_string_t") == 0));
                    if (is_str) {
                        fprintf(f, "((%s).data ? (%s).data : \"NULL\")", arg->text, arg->text);
                    } else {
                        generate_expression(f, arg);
                    }
                      } else if (arg->type == AST_METHOD_CALL) {
                    char* m = arg->text;
                    const char* mret = method_call_module_ret(arg);
                    if (strcmp(m, "upper")==0 || strcmp(m, "lower")==0 || strcmp(m, "repeat")==0 ||
                        strcmp(m, "replace")==0 || strcmp(m, "trim")==0 || strcmp(m, "ltrim")==0 ||
                        strcmp(m, "rtrim")==0 || strcmp(m, "join")==0 || strcmp(m, "substr")==0 ||
                        strcmp(m, "view")==0 ||
                        strcmp(m, "regex_replace")==0 || strcmp(m, "str")==0 || strcmp(m, "where")==0 ||
                        (mret && strcmp(mret, "string") == 0)) {
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
                         const char* at = get_local_variable_type(arr_node->text);
                         char mkt[160], mvt[160];
                         if (at && map_type_parts(at, mkt, sizeof(mkt), mvt, sizeof(mvt))) {
                             // Map read: numeric unless V is string (COIL-102)
                             is_numeric = strcmp(mvt, "string") != 0;
                         } else if (strcmp(arr_node->text, "scaled")==0 ||
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
            g_view_promote = saved_view_promote_stmt_printf;
            fputs(");\n", f);
            break;
        }

        case AST_IF: {
            emit_line_directive(f, node);  // Emit #line for if statement
            emit_indent(f, indent);
            fprintf(f, "if (");
            generate_expression(f, node->children[0]);
            fprintf(f, ") {\n");
            // Parity step 1: a positive guard proves its
            // pair inside the then-branch only (after the
            // if, the guard may have been false).
            char pbuf[128], pidx[128];
            int is_pos = bound_positive_guard(node->children[0], pbuf, pidx);
            BoundProof saved_pos[64];
            int saved_pos_count = 0;
            if (is_pos) { saved_pos_count = bound_proof_save(saved_pos); bound_proof_add(pbuf, pidx); }
            generate_node(f, node->children[1], indent + 4);
            if (is_pos) { bound_proof_restore(saved_pos, saved_pos_count); }
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
            // A bare block is its own C scope: without braces,
            // two sibling blocks could not reuse a name.
            // COIL-157: it is also its own scope context:
            // buffers a block creates die at its end.
            // Redesign: a clean block's frame would be
            // empty, so it is skipped; the C braces stay,
            // and lexical depth counting is unchanged.
            emit_indent(f, indent);
            fprintf(f, "{\n");
            int clean = block_is_clean(node);
            int checkpoint = !clean && block_is_checkpoint(node);
            emit_indent(f, indent + 1);
            if (!clean) fprintf(f, checkpoint ? "come_scope_push_checkpoint();\n" : "come_scope_push();\n");
            scope_depth_push();
            int saved_unchecked = g_unchecked;
            if (node->is_unchecked) g_unchecked = 1;
            generate_block_children(f, node, indent + 1);
            g_unchecked = saved_unchecked;
            scope_depth_pop();
            // A block ending in a return never reaches its
            // pop: the return already popped to the marker.
            if (!clean && (node->child_count == 0 ||
                node->children[node->child_count - 1]->type != AST_RETURN)) {
                emit_indent(f, indent + 1);
                fprintf(f, "come_scope_pop();\n");
            }
            emit_indent(f, indent);
            fprintf(f, "}\n");
            break;
        }

        case AST_SPAWN: {
            // Design 3 (redesign): spawn arguments cross
            // into the new coroutine, which may retain
            // them, so views there promote to copies.
            g_view_promote = 1;
            // spawn name(args) (COIL-120, spec 9.1): build the
            // args block in the new coroutine's own context and
            // make it runnable. It runs when someone yields,
            // blocks, or returns.
            emit_line_directive(f, node);
            ASTNode* coro = find_coroutine(node->text);
            if (!coro) {
                codegen_error("spawn: '%s' is not a coroutine declared in this module", node->text);
                emit_indent(f, indent);
                fprintf(f, "/* spawn of unknown coroutine '%s' */\n", node->text);
                break;
            }
            int want = fn_param_count(coro);
            if (node->child_count != want) {
                codegen_error("spawn: coroutine '%s' takes %d argument(s), %d given",
                              node->text, want, node->child_count);
            }
            emit_indent(f, indent);
            fprintf(f, "{\n");
            emit_indent(f, indent + 4);
            fprintf(f, "come_coro_t* __come_co = come_coro_create(come_%s__%s__entry, &come_%s__ctx);\n",
                    current_module, node->text, current_module);
            if (want > 0) {
                emit_indent(f, indent + 4);
                fprintf(f, "come_%s__%s__args_t* __come_a = (come_%s__%s__args_t*)come_coro_args(__come_co, sizeof(come_%s__%s__args_t));\n",
                        current_module, node->text, current_module, node->text,
                        current_module, node->text);
                for (int i = 0; i < node->child_count && i < want; i++) {
                    ASTNode* param = coro->children[1 + i];
                    emit_indent(f, indent + 4);
                    fprintf(f, "__come_a->%s = ", param->text);
                    if (strcmp(param->children[1]->text, "string") == 0) {
                        emit_string_expr(f, node->children[i]);
                    } else {
                        generate_expression(f, node->children[i]);
                    }
                    fprintf(f, ";\n");
                    // Ownership transfer (COIL-122, spec 9.1):
                    // a buffer argument changes owner to the
                    // coroutine - the args block lives in the
                    // coroutine's own context, so stealing to it
                    // moves the buffer under the coroutine. A
                    // link is not a buffer: it is a handle into
                    // its channel, and its ownership moves by
                    // adoption below instead.
                    if (is_buffer_type(param->children[1]->text) &&
                        strcmp(param->children[1]->text, "link") != 0) {
                        // COIL-135: the field is a header value; steal
                        // its data block under the args block (which
                        // lives in the coroutine's context) and point
                        // the header's owner at it.
                        emit_indent(f, indent + 4);
                        fprintf(f, "if (__come_a->%s.data) mem_talloc_steal(__come_a, __come_a->%s.data);\n",
                                param->text, param->text);
                        emit_indent(f, indent + 4);
                        fprintf(f, "__come_a->%s.ctx = __come_a;\n", param->text);
                    }
                    // A link argument is adopted by the new
                    // coroutine (COIL-123): its end closes
                    // automatically when the coroutine finishes.
                    if (strcmp(param->children[1]->text, "link") == 0) {
                        emit_indent(f, indent + 4);
                        fprintf(f, "come_link_adopt(__come_a->%s, __come_co);\n", param->text);
                    }
                }
            }
            emit_indent(f, indent + 4);
            fprintf(f, "come_coro_start(__come_co);\n");
            emit_indent(f, indent);
            fprintf(f, "}\n");
            break;
        }

        case AST_YIELD: {
            emit_line_directive(f, node);
            emit_indent(f, indent);
            fprintf(f, "come_coro_yield();\n");
            break;
        }

        case AST_COLINK_ASSIGN: {
            // (a, b) = colink() (COIL-123, spec 9.2): declares
            // two link variables and creates the channel.
            // (a, b) = f() for a tuple-returning f (spec 8.2):
            // destructures the returned struct (COIL-127).
            emit_line_directive(f, node);
            ASTNode* rhs = node->children[2];
            int is_colink = rhs && rhs->type == AST_CALL && strcmp(rhs->text, "colink") == 0;
            if (rhs && !is_colink && (rhs->type == AST_CALL || rhs->type == AST_METHOD_CALL)) {
                // The callee may live in this file, a sibling
                // file of this module, or another module
                // entirely (COIL-146): resolve through the
                // registry as well as the local table, and
                // name the typedef by its defining module.
                char defmod[128];
                TupleFn scratch;
                const TupleFn* tf = resolve_tuple_call(rhs, defmod, sizeof(defmod), &scratch);
                if (!tf) {
                    codegen_error("the (a, b) = ... form is for colink() or a tuple-returning function (spec 8.2, 9.2)");
                    break;
                }
                if (tf->count != 2) {
                    codegen_error("'%s' returns %d values; this destructure binds 2 names", rhs->text, tf->count);
                    break;
                }
                // A unique temp per destructure: a function
                // may destructure several calls.
                static int tup_counter = 0;
                char tup[32];
                snprintf(tup, sizeof(tup), "__tup%d", tup_counter++);
                emit_indent(f, indent);
                fprintf(f, "come_%s__%s_ret_t %s = ", defmod, tf->name, tup);
                generate_expression(f, rhs);
                fprintf(f, ";\n");
                for (int i = 0; i < 2; i++) {
                    const char* name = node->children[i]->text;
                    emit_indent(f, indent);
                    if (!get_local_variable_type(name)) {
                        ASTNode* tn = ast_new(AST_IDENTIFIER);
                        strcpy(tn->text, tf->elems[i]);
                        register_local_type(name, tn);
                        char ct[300];
                        come_type_to_c(tf->elems[i], ct, sizeof(ct));
                        fprintf(f, "%s %s = %s._%d;\n", ct, name, tup, i);
                    } else {
                        fprintf(f, "%s = %s._%d;\n", name, tup, i);
                    }
                }
                break;
            }
            if (!is_colink) {
                codegen_error("the (a, b) = ... form is for colink() or a tuple-returning function (spec 8.2, 9.2)");
                break;
            }
            for (int i = 0; i < 2; i++) {
                ASTNode* tn = ast_new(AST_IDENTIFIER);
                strcpy(tn->text, "link");
                register_local_type(node->children[i]->text, tn);
                emit_indent(f, indent);
                fprintf(f, "come_link_t* %s;\n", node->children[i]->text);
            }
            emit_indent(f, indent);
            fprintf(f, "come_colink(&%s, &%s);\n",
                    node->children[0]->text, node->children[1]->text);
            break;
        }

        case AST_RETURN: {
            // Design 3 (redesign): a return value leaves
            // the function, so views in it promote to
            // copies. (Call arguments inside it are
            // re-decided per argument by the call sites,
            // which save/restore around themselves.)
            g_view_promote = 1;
            emit_line_directive(f, node);
            emit_indent(f, indent);
            if (g_in_coroutine && node->child_count > 0) {
                codegen_error("coroutines cannot return a value (spec 9.1); results travel through links");
            }
            if (g_cur_tuple) {
                // Forwarding a tuple call (COIL-146): return
                // f() where f returns the same number of
                // values - in this file, a sibling file, or
                // another module. Each function's return
                // struct is its own C type, so unless the
                // callee is this very function the value is
                // rebuilt field by field.
                // COIL-157: every exit adopts the value to
                // the caller's scope before popping the
                // marker frame, or its buffers would die
                // with this function.
                if (node->child_count == 1 &&
                    (node->children[0]->type == AST_CALL ||
                     node->children[0]->type == AST_METHOD_CALL)) {
                    char defmod[128];
                    TupleFn scratch;
                    const TupleFn* tf = resolve_tuple_call(node->children[0],
                                                           defmod, sizeof(defmod), &scratch);
                    if (tf && tf->count == g_cur_tuple->count) {
                        fprintf(f, "come_%s__%s_ret_t _come_ret = ", current_module, g_cur_tuple->name);
                        if (strcmp(defmod, current_module) == 0 &&
                            strcmp(tf->name, g_cur_tuple->name) == 0) {
                            generate_expression(f, node->children[0]);
                            fprintf(f, ";\n");
                        } else {
                            fprintf(f, "({ come_%s__%s_ret_t _t = ", defmod, tf->name);
                            generate_expression(f, node->children[0]);
                            fprintf(f, "; (come_%s__%s_ret_t){", current_module, g_cur_tuple->name);
                            for (int i = 0; i < tf->count; i++) {
                                if (i) fprintf(f, ", ");
                                fprintf(f, "_t._%d", i);
                            }
                            fprintf(f, "}; });\n");
                        }
                        for (int i = 0; i < g_cur_tuple->count; i++) {
                            char elv[64];
                            snprintf(elv, sizeof(elv), "_come_ret._%d", i);
                            emit_adopt_value(f, indent, elv, g_cur_tuple->elems[i], "come_scope_caller_ctx()");
                        }
                        emit_line_directive_forced(f, node);
                        emit_indent(f, indent);
                        if (!g_cur_fn_frameless) fprintf(f, "come_scope_pop_to_marker();\n");
                        emit_line_directive_forced(f, node);
                        emit_indent(f, indent);
                        fprintf(f, "return _come_ret;\n");
                        break;
                    }
                }
                // Tuple return (spec 8.2): build the generated
                // struct from the comma-separated values.
                if (node->child_count != g_cur_tuple->count) {
                    codegen_error("'%s' returns %d values (spec 8.2); this return gives %d",
                                  g_cur_tuple->name, g_cur_tuple->count, node->child_count);
                    break;
                }
                fprintf(f, "come_%s__%s_ret_t _come_ret = (come_%s__%s_ret_t){", current_module, g_cur_tuple->name, current_module, g_cur_tuple->name);
                for (int i = 0; i < node->child_count; i++) {
                    if (i) fprintf(f, ", ");
                    if (strcmp(g_cur_tuple->elems[i], "string") == 0) {
                        emit_string_expr(f, node->children[i]);
                    } else {
                        generate_expression(f, node->children[i]);
                    }
                }
                fprintf(f, "};\n");
                for (int i = 0; i < g_cur_tuple->count; i++) {
                    char elv[64];
                    snprintf(elv, sizeof(elv), "_come_ret._%d", i);
                    emit_adopt_value(f, indent, elv, g_cur_tuple->elems[i], "come_scope_caller_ctx()");
                }
                emit_line_directive_forced(f, node);
                emit_indent(f, indent);
                if (!g_cur_fn_frameless) fprintf(f, "come_scope_pop_to_marker();\n");
                emit_line_directive_forced(f, node);
                emit_indent(f, indent);
                fprintf(f, "return _come_ret;\n");
                break;
            }
            if (node->child_count > 1) {
                codegen_error("multiple return values need a tuple return type like (int, string) (spec 8.2)");
                break;
            }
            if (node->child_count > 0 && expr_is_fn_value(node->children[0]))
                codegen_error("functions cannot be returned (spec 8.3): '%s'", node->children[0]->text);
            if (strcmp(current_function_return_type, "void") == 0) {
                 emit_line_directive_forced(f, node);
                 if (!g_cur_fn_frameless) fprintf(f, "come_scope_pop_to_marker();\n");
                 emit_indent(f, indent);
                 fprintf(f, "return;\n");
            } else {
                if (node->child_count > 0) {
                    // COIL-157: materialize into a temp,
                    // adopt its buffers to the caller's
                    // scope, pop the marker, then return.
                    char rct[300];
                    come_type_to_c(current_function_return_type, rct, sizeof(rct));
                    fprintf(f, "%s _come_ret = ", rct);
                    // Same materialization as call arguments: a
                    // string function must return a come_string_t,
                    // not a raw literal (COIL-133).
                    char rb2[300];
                    const char* rtx = expr_come_type(node->children[0], rb2, sizeof(rb2));
                    if (is_ref_type(rtx)) {
                        // Returning a reference where the
                        // function declares the struct returns
                        // the referent's value (COIL-136).
                        char inner2[256];
                        ref_inner_type(rtx, inner2, sizeof(inner2));
                        if (strcmp(current_function_return_type, inner2) != 0) {
                            codegen_error("cannot return a reference here: the function returns %s - return the struct value instead", current_function_return_type);
                        }
                        fprintf(f, "*(");
                        generate_expression(f, node->children[0]);
                        fprintf(f, ")");
                    } else if (strcmp(current_function_return_type, "string") == 0 &&
                        node->children[0]->type == AST_STRING_LITERAL)
                        emit_string_expr(f, node->children[0]);
                    else
                        generate_expression(f, node->children[0]);
                    fprintf(f, ";\n");
                    const char* atype = current_function_return_type;
                    char abuf[300];
                    if (strcmp(atype, "var") == 0 && rtx) {
                        snprintf(abuf, sizeof(abuf), "%s", rtx);
                        atype = abuf;
                    }
                    emit_adopt_value(f, indent, "_come_ret", atype, "come_scope_caller_ctx()");
                    emit_line_directive_forced(f, node);
                    emit_indent(f, indent);
                    if (!g_cur_fn_frameless) fprintf(f, "come_scope_pop_to_marker();\n");
                    emit_line_directive_forced(f, node);
                    emit_indent(f, indent);
                    fprintf(f, "return _come_ret;\n");
                } else {
                    emit_line_directive_forced(f, node);
                    if (!g_cur_fn_frameless) fprintf(f, "come_scope_pop_to_marker();\n");
                    emit_line_directive_forced(f, node);
                    emit_indent(f, indent);
                    fprintf(f, "return 0;\n");
                }
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
            fprintf(f, "struct");
            if (node->layout_packed) fprintf(f, " __attribute__((packed))");
            if (node->layout_align > 0) fprintf(f, " __attribute__((aligned(%d)))", node->layout_align);
            fprintf(f, " %s {\n", node->text);
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
                         // Fix: if type ends in [], map to come_byte_array_t for structs
                         // Or use pointer? byte* items.
                         // But we want to support size?
                         // "byte[]" usually come_byte_array_t in my codegen.
                         fprintf(f, "come_hdr_t %s;\n", field->text);
                     } else {
                         fprintf(f, "%s %s;\n", type->text, field->text);
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
            // Design 3 (redesign): an assignment stores
            // its value (and, for a container element,
            // its key), so views anywhere in it promote
            // to copies.
            g_view_promote = 1;
            emit_line_directive(f, node);  // Emit #line for assignment
            // Assigning to a constant (COIL-129): a module
            // constant is not a variable, and the first-
            // assignment rule below would otherwise shadow it
            // with a fresh local and silently "work".
            if (node->child_count >= 1 &&
                node->children[0]->type == AST_IDENTIFIER &&
                !get_local_variable_type(node->children[0]->text) &&
                (is_const_name(node->children[0]->text) ||
                 is_fn_const_name(node->children[0]->text) ||
                 codegen_module_const_type(current_module, node->children[0]->text))) {
                codegen_error("cannot assign to '%s': it is a constant (spec 4) - declare a variable if the value must change", node->children[0]->text);
                break;
            }
            // Assigning to a name nothing declares (COIL-129):
            // compound assignments (+=, ...) cannot declare,
            // and a plain '=' to a group sibling's global is
            // fine - but a plain '=' to a wholly unknown name
            // is the first-assignment declaration below, so
            // only the compound forms are errors here.
            if (strcmp(node->text, "=") != 0 && node->child_count >= 1 &&
                node->children[0]->type == AST_IDENTIFIER &&
                !get_local_variable_type(node->children[0]->text) &&
                !is_module_var(node->children[0]->text) &&
                !is_group_symbol(node->children[0]->text)) {
                codegen_error("'%s' is not defined: '%s' needs a variable declared before it - declare it with a type first", node->children[0]->text, node->text);
                break;
            }
            // First assignment to an undeclared name declares
            // it with the inferred type (COIL-126; spec 9.1,
            // 9.2 and 10.1). The type locks at this assignment;
            // later assignments are ordinary stores.
            if (strcmp(node->text, "=") == 0 && node->child_count >= 2 &&
                node->children[0]->type == AST_IDENTIFIER &&
                !get_local_variable_type(node->children[0]->text) &&
                !is_module_var(node->children[0]->text)) {
                const char* inferred = infer_assign_type(node->children[1]);
                if (!inferred) {
                    codegen_error("cannot infer the type of '%s' from its first assignment; declare it with a type first", node->children[0]->text);
                    break;
                }
                ASTNode* tn = ast_new(AST_IDENTIFIER);
                strcpy(tn->text, inferred);
                register_local_type(node->children[0]->text, tn);
                emit_indent(f, indent);
                emit_inferred_decl(f, inferred, node->children[0]->text, node->children[1]);
                break;
            }
            // A `var` local locks its type at its first
            // assignment (spec 10.1, COIL-127).
            if (strcmp(node->text, "=") == 0 && node->child_count >= 2 &&
                node->children[0]->type == AST_IDENTIFIER) {
                const char* cur = get_local_variable_type(node->children[0]->text);
                if (cur && strcmp(cur, "var") == 0) {
                    const char* inferred = infer_assign_type(node->children[1]);
                    if (!inferred) {
                        codegen_error("cannot infer the type of var '%s' from its first assignment; declare it with a type first", node->children[0]->text);
                        break;
                    }
                    update_local_variable_type(node->children[0]->text, inferred);
                }
            }
            // A string value can never land in a non-string
            // variable, nor a numeric literal in a string
            // (the type-lock rule of spec 10.1, applied to
            // every typed local).
            if (node->child_count >= 2 &&
                node->children[0]->type == AST_IDENTIFIER) {
                const char* lt = get_local_variable_type(node->children[0]->text);
                if (lt && strcmp(lt, "var") != 0) {
                    int lhs_str = strcmp(lt, "string") == 0;
                    if (expr_is_stringish(node->children[1]) && !lhs_str) {
                        codegen_error("cannot assign a string to '%s' of type %s", node->children[0]->text, lt);
                        break;
                    }
                    if (lhs_str && (node->children[1]->type == AST_NUMBER ||
                                    node->children[1]->type == AST_BOOL_LITERAL)) {
                        codegen_error("cannot assign a non-string to string '%s'", node->children[0]->text);
                        break;
                    }
                }
            }
            emit_indent(f, indent);
            check_assign_store(node); // COIL-107
            // Assignment to a reference variable (COIL-136):
            // a struct value assigns through to the referent;
            // &value or another reference rebinds.
            if (strcmp(node->text, "=") == 0 && node->child_count >= 2 &&
                node->children[0]->type == AST_IDENTIFIER) {
                const char* lt = get_local_variable_type(node->children[0]->text);
                if (is_ref_type(lt)) {
                    ASTNode* rhs = node->children[1];
                    if (rhs->type == AST_UNARY_OP && strcmp(rhs->text, "&") == 0) {
                        generate_expression(f, node->children[0]);
                        fprintf(f, " = ");
                        generate_expression(f, rhs);
                        fprintf(f, ";\n");
                        break;
                    }
                    char rb[300];
                    const char* rt2 = expr_come_type(rhs, rb, sizeof(rb));
                    if (!is_ref_type(rt2)) {
                        fprintf(f, "(*");
                        generate_expression(f, node->children[0]);
                        fprintf(f, ") = ");
                        generate_expression(f, rhs);
                        fprintf(f, ";\n");
                        break;
                    }
                    // reference = reference: falls through to
                    // the plain assignment below (a rebind).
                }
            }
            // Step 4b remainder (statement form): a concat
            // assignment to a first-assignment-transient
            // local bypasses adoption, like a declaration
            // init -- direct arena assignment, no _come_v
            // temp, no promote. The expression-form
            // bypass (in generate_expression) covers
            // nested assignments; this covers statements.
            if (strcmp(node->text, "=") == 0 && node->child_count >= 2 &&
                node->children[0]->type == AST_IDENTIFIER &&
                node->children[1]->type == AST_BINARY_OP &&
                strcmp(node->children[1]->text, "+") == 0 &&
                get_local_variable_type(node->children[0]->text) &&
                codegen_transient_first_assign(current_module, current_function_name,
                                               node->children[0]->text)) {
                int saved_promote_stmt = g_view_promote;
                g_view_promote = 0;
                emit_indent(f, indent);
                generate_expression(f, node->children[0]);
                fprintf(f, " = ");
                generate_expression(f, node->children[1]);
                fprintf(f, ";\n");
                g_view_promote = saved_promote_stmt;
                break;
            }
            // Typed map index write: m[key] = value (COIL-102)
            int map_write = 0;
            if (strcmp(node->text, "=") == 0 && node->children[0]->type == AST_ARRAY_ACCESS) {
                ASTNode* acc = node->children[0];
                ASTNode* mbase = acc->children[0];
                if (mbase->type == AST_IDENTIFIER) {
                    const char* mbt = dispatch_type_of(mbase->text);
                    char mkt[160], mvt[160];
                    if (mbt && map_type_parts(mbt, mkt, sizeof(mkt), mvt, sizeof(mvt))) {
                        if (!map_combo_supported(mkt, mvt)) map_combo_error(mkt, mvt);
                        check_map_expr_type(mkt, acc->children[1], "key");
                        check_map_expr_type(mvt, node->children[1], "value");
                        emit_map_put(f, mbase, mbase->text, mvt, acc->children[1], node->children[1]);
                        map_write = 1;
                    }
                }
            }
            if (!map_write) {
            // A string literal assigned to a string variable
            // materializes, exactly as in a declaration;
            // emitting the raw C literal would type-mismatch
            // (COIL-126).
            // COIL-157: the literal paths are plain stores
            // only; a compound op must fall through to the
            // plain tail instead of silently becoming '='.
            if (strcmp(node->text, "=") == 0 &&
                node->children[1]->type == AST_STRING_LITERAL &&
                (node->children[0]->type == AST_IDENTIFIER ||
                 node->children[0]->type == AST_MEMBER_ACCESS)) {
                // A string literal assigned to a string variable
                // or a string field materializes (COIL-126/135).
                char ltb[160];
                const char* lt = expr_come_type(node->children[0], ltb, sizeof(ltb));
                if (lt && strcmp(lt, "string") == 0) {
                    // COIL-157: materialize into a temp and
                    // adopt it to the destination's scope,
                    // or a literal stored into an outer
                    // variable from a loop body would die
                    // at the iteration's end.
                    char dst[160];
                    ASTNode* root = node->children[0];
                    while (root->type == AST_MEMBER_ACCESS && root->child_count >= 1)
                        root = root->children[0];
                    if (root->type == AST_IDENTIFIER) dst_scope_text(root->text, dst, sizeof(dst));
                    else snprintf(dst, sizeof(dst), "COME_CTX");
                    fprintf(f, "{\n");
                    emit_indent(f, indent + 4);
                    fprintf(f, "come_string_t _come_v = come_string_new(COME_CTX, ");
                    generate_expression(f, node->children[1]);
                    fprintf(f, ");\n");
                    emit_adopt_value(f, indent + 4, "_come_v", "string", dst);
                    emit_indent(f, indent + 4);
                    generate_expression(f, node->children[0]);
                    fprintf(f, " = _come_v;\n");
                    emit_indent(f, indent);
                    fprintf(f, "}\n");
                    break;
                }
            }
            if (node->children[0]->type == AST_IDENTIFIER && node->children[1]->type == AST_IDENTIFIER) {
                const char* lt = get_local_variable_type(node->children[0]->text);
                const char* rt = get_local_variable_type(node->children[1]->text);
                if (lt && rt) check_array_transfer("be assigned to", node->children[0]->text, lt, node->children[1]->text, rt);
            }
            if (node->children[0]->type == AST_IDENTIFIER && is_link_read_expr(node->children[1])) {
                const char* lt = get_local_variable_type(node->children[0]->text);
                if (lt && strcmp(lt, "link") == 0) {
                    generate_expression(f, node->children[0]);
                    fprintf(f, " %s ", node->text);
                    emit_link_read_raw(f, node->children[1]);
                    fprintf(f, ";\n");
                    break;
                }
            }
            if (node->children[0]->type == AST_IDENTIFIER && is_link_read_expr(node->children[1])) {
                const char* lt = get_local_variable_type(node->children[0]->text);
                if (lt && strcmp(lt, "string") == 0) {
                    generate_expression(f, node->children[0]);
                    fprintf(f, " %s ", node->text);
                    emit_expr_as(f, node->children[1], "come_string_t");
                    fprintf(f, ";\n");
                    break;
                }
            }
            if (strcmp(node->text, "=") == 0 &&
                node->children[0]->type == AST_ARRAY_ACCESS &&
                node->children[1]->type == AST_STRING_LITERAL) {
                // A string literal stored into a string element
                // materializes, like any string store.
                char atb[160];
                const char* at = expr_come_type(node->children[0], atb, sizeof(atb));
                if (at && strcmp(at, "string") == 0) {
                    // COIL-157: temp + adopt. A dynamic
                    // array's destination is its header's
                    // ctx (temped once, so an impure index
                    // is evaluated once); a fixed array's
                    // destination is its variable's scope.
                    ASTNode* hdr = node->children[0]->children[0];
                    char hb[300];
                    const char* ht = resolve_lhs_type(hdr, hb, sizeof(hb));
                    if (ht && type_is_fixed_array(ht)) {
                        char dst[160];
                        ASTNode* root = hdr;
                        while (root->type == AST_MEMBER_ACCESS && root->child_count >= 1)
                            root = root->children[0];
                        if (root->type == AST_IDENTIFIER) dst_scope_text(root->text, dst, sizeof(dst));
                        else snprintf(dst, sizeof(dst), "COME_CTX");
                        fprintf(f, "{\n");
                        emit_indent(f, indent + 4);
                        fprintf(f, "come_string_t _come_v = come_string_new(COME_CTX, ");
                        generate_expression(f, node->children[1]);
                        fprintf(f, ");\n");
                        emit_adopt_value(f, indent + 4, "_come_v", "string", dst);
                        emit_indent(f, indent + 4);
                        generate_expression(f, node->children[0]);
                        fprintf(f, " %s _come_v;\n", node->text);
                        emit_indent(f, indent);
                        fprintf(f, "}\n");
                    } else if (ht && strncmp(ht, "map{", 4) != 0) {
                        // typeof: the header's exact C type
                        // (see the plain tail above).
                        fprintf(f, "{\n");
                        emit_indent(f, indent + 4);
                        fprintf(f, "typeof(");
                        generate_expression(f, hdr);
                        fprintf(f, ") _come_h = ");
                        generate_expression(f, hdr);
                        fprintf(f, ";\n");
                        emit_indent(f, indent + 4);
                        fprintf(f, "come_string_t _come_v = come_string_new(COME_CTX, ");
                        generate_expression(f, node->children[1]);
                        fprintf(f, ");\n");
                        emit_adopt_value(f, indent + 4, "_come_v", "string", "_come_h.ctx");
                        emit_indent(f, indent + 4);
                        emit_lhs_rerooted(f, node->children[0], hdr, "_come_h");
                        fprintf(f, " %s _come_v;\n", node->text);
                        emit_indent(f, indent);
                        fprintf(f, "}\n");
                    } else {
                        generate_expression(f, node->children[0]);
                        fprintf(f, " %s come_string_new(COME_CTX, ", node->text);
                        generate_expression(f, node->children[1]);
                        fprintf(f, ");\n");
                    }
                    break;
                }
            }
            // COIL-157: a buffer stored into a longer-lived
            // variable must move scopes with it, or it dies
            // when the storing scope exits. Materialize the
            // value into a temp, adopt the temp to the
            // destination's scope, then store it.
            int adopted_store = 0;
            if (strcmp(node->text, "=") == 0 && node->child_count >= 2) {
                ASTNode* lhs = node->children[0];
                ASTNode* rhs = node->children[1];
                char ltb[300];
                const char* ltype = resolve_lhs_type(lhs, ltb, sizeof(ltb));
                if (!ltype && lhs->type == AST_IDENTIFIER) {
                    char rb[300];
                    const char* rt = expr_come_type(rhs, rb, sizeof(rb));
                    if (rt) { snprintf(ltb, sizeof(ltb), "%s", rt); ltype = ltb; }
                }
                // Map index writes keep their existing path
                // (see the expression site): a map index is
                // not an array element. Whole-map variable
                // assignments adopt like any other buffer.
                if (ltype && type_needs_adopt(ltype) &&
                    !(strncmp(ltype, "map{", 4) == 0 &&
                      lhs->type == AST_ARRAY_ACCESS)) {
                    char ctype[300];
                    come_type_to_c(ltype, ctype, sizeof(ctype));
                    ASTNode* anode = lhs_array_node(lhs);
                    int handled = 0;
                    if (anode && anode->child_count >= 1) {
                        char ab[300];
                        const char* atype = resolve_lhs_type(anode->children[0], ab, sizeof(ab));
                        if (atype && type_is_fixed_array(atype)) {
                            // Fixed arrays are inline in
                            // their variable: the value's
                            // destination is the array
                            // variable's scope, and the
                            // store itself is emitted once.
                            char dst[160];
                            ASTNode* root = anode->children[0];
                            while (root->type == AST_MEMBER_ACCESS && root->child_count >= 1)
                                root = root->children[0];
                            if (root->type == AST_IDENTIFIER) dst_scope_text(root->text, dst, sizeof(dst));
                            else snprintf(dst, sizeof(dst), "COME_CTX");
                            fprintf(f, "{\n");
                            emit_indent(f, indent + 4);
                            fprintf(f, "%s _come_v = ", ctype);
                            generate_expression(f, rhs);
                            fprintf(f, ";\n");
                            // Step 4c-arrays: no eager adopt
                            // into a transient container.
                            if (!(anode->child_count >= 1 &&
                                  anode->children[0]->type == AST_IDENTIFIER &&
                                  codegen_transient_container(current_module, current_function_name,
                                                              anode->children[0]->text)))
                                emit_adopt_value(f, indent + 4, "_come_v", ltype, dst);
                            emit_indent(f, indent + 4);
                            generate_expression(f, lhs);
                            fprintf(f, " = _come_v;\n");
                            emit_indent(f, indent);
                            fprintf(f, "}\n");
                            handled = 1;
                        } else if (atype) {
                            // Dynamic array store: temp the
                            // array header once; the
                            // destination scope is the
                            // temp's ctx, and the store path
                            // reuses it so the index is
                            // evaluated once. typeof takes
                            // the header's exact C type
                            // (string lists differ from
                            // plain headers) without
                            // evaluating it.
                            fprintf(f, "{\n");
                            emit_indent(f, indent + 4);
                            fprintf(f, "typeof(");
                            generate_expression(f, anode->children[0]);
                            fprintf(f, ") _come_arr = ");
                            generate_expression(f, anode->children[0]);
                            fprintf(f, ";\n");
                            emit_indent(f, indent + 4);
                            fprintf(f, "%s _come_v = ", ctype);
                            generate_expression(f, rhs);
                            fprintf(f, ";\n");
                            // Step 4c-arrays: no eager adopt
                            // into a transient container.
                            if (!(anode->child_count >= 1 &&
                                  anode->children[0]->type == AST_IDENTIFIER &&
                                  codegen_transient_container(current_module, current_function_name,
                                                              anode->children[0]->text)))
                                emit_adopt_value(f, indent + 4, "_come_v", ltype, "_come_arr.ctx");
                            emit_indent(f, indent + 4);
                            emit_lhs_rerooted(f, lhs, anode->children[0], "_come_arr");
                            fprintf(f, " = _come_v;\n");
                            emit_indent(f, indent);
                            fprintf(f, "}\n");
                            handled = 1;
                        }
                        // Unresolvable array type: fall
                        // through to the plain store.
                    }
                    if (!handled && !anode) {
                        // Identifier or member LHS: the
                        // destination scope comes from the
                        // root variable.
                        char dst[160];
                        ASTNode* root = lhs;
                        while (root->type == AST_MEMBER_ACCESS && root->child_count >= 1)
                            root = root->children[0];
                        if (root->type == AST_IDENTIFIER) dst_scope_text(root->text, dst, sizeof(dst));
                        else snprintf(dst, sizeof(dst), "COME_CTX");
                        fprintf(f, "{\n");
                        emit_indent(f, indent + 4);
                        fprintf(f, "%s _come_v = ", ctype);
                        generate_expression(f, rhs);
                        fprintf(f, ";\n");
                        emit_adopt_value(f, indent + 4, "_come_v", ltype, dst);
                        emit_indent(f, indent + 4);
                        generate_expression(f, lhs);
                        fprintf(f, " = _come_v;\n");
                        emit_indent(f, indent);
                        fprintf(f, "}\n");
                        adopted_store = 1;
                    } else if (handled) {
                        adopted_store = 1;
                    }
                    // Otherwise (unresolvable array type):
                    // fall through to the plain store.
                }
            }
            if (!adopted_store) {
                generate_expression(f, node->children[0]);
                fprintf(f, " %s ", node->text);
                generate_expression(f, node->children[1]);
                fprintf(f, ";\n");
            }
            }
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
            // Remember the name for this function: assigning
            // to a constant is an error wherever it was
            // declared (COIL-129). Module-level constants
            // are not function constants - the registry
            // tracks them, and their names must not leak
            // into a function's local-const set (COIL-148).
            if (!g_module_level && node->text[0] && !is_const_name(node->text) &&
                !is_fn_const_name(node->text) && g_fn_const_count < 128) {
                snprintf(g_fn_const_names[g_fn_const_count++], 128, "%.*s", 127, node->text);
            }
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
                // A module-level value constant is static
                // to its module (COIL-148): one definition,
                // under a namespaced name, in its declaring
                // file; every other file of the module - and
                // importers of an exported constant - sees
                // the extern the surface pass emitted.
                if (g_module_level &&
                    codegen_module_const_type(current_module, node->text)) {
                    fprintf(f, "const %s come_%s__const_%s = ", type, current_module, node->text);
                } else {
                    fprintf(f, "const %s %s = ", type, node->text);
                }
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
                    fprintf(f, "%s %s;\n", type->text, field->text);
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
            g_break_depth++;
            for (int i=1; i < node->child_count; i++) {
                generate_node(f, node->children[i], indent+4);
            }
            g_break_depth--;
            emit_indent(f, indent);
            fprintf(f, "}\n");
            break;
        }
        
        case AST_CASE: {
            emit_indent(f, indent);
            fprintf(f, "case ");
            generate_expression(f, node->children[0]);
            fprintf(f, ": {\n");
            // Spec 11.2: a case does NOT fall through unless
            // its last statement is `fallthrough` (COIL-127).
            int has_fallthrough = node->child_count > 1 &&
                node->children[node->child_count - 1]->type == AST_FALLTHROUGH;
            for (int i=1; i < node->child_count; i++) {
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

        case AST_FALLTHROUGH: {
            // Marker only: AST_CASE reads it to decide whether
            // to emit the break. Emits nothing itself.
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
            g_break_depth++;
            g_continue_depth++;
            // COIL-157: the body scope is per iteration -
            // a map built in a loop body must die at the
            // iteration's end, not at the function's.
            if (g_loop_depth_count < 64) g_loop_depths[g_loop_depth_count++] = g_scope_depth;
            // Body is a block usually?
            ASTNode* body = node->children[1];
            // Parity step 2: loop bodies start with a cleared
            // proof set (pre-loop proofs must not leak into
            // a body that runs many times); a positive loop
            // condition proves its pair at the top of every
            // iteration, so it is added for the body only.
            char wbuf[128], widx[128];
            int wproof = bound_positive_guard(node->children[0], wbuf, widx);
            BoundProof saved_loop[64];
            int saved_loop_count = bound_proof_save(saved_loop);
            bound_proof_clear();
            if (wproof) bound_proof_add(wbuf, widx);
            // Interval slice: a single-bound while
            // (`i < b.length()`) with i provably
            // nonneg at loop entry proves (b, i) at
            // body top. The body's own modifications
            // to i void the proof after themselves
            // (generate_guarded_stmt voids on assign,
            // call, and now i++/i--), so reads before
            // any modification are raw and reads after
            // one are checked -- sound for the common
            // `i = i + 1` / `i++` body patterns.
            {
                char sbuf[128] = "", sidx[128] = "";
                if (bound_cmp_kind(node->children[0], sidx, sbuf) == BOUND_LT_LEN &&
                    nonneg_contains(sidx))
                    bound_proof_add(sbuf, sidx);
            }
            if (body->type == AST_BLOCK) {
                 // Clean blocks (redesign): a frame that
                 // would be empty is skipped; lexical
                 // depth counting is unchanged.
                 int clean = block_is_clean(body);
                 int checkpoint = !clean && block_is_checkpoint(body);
                 emit_indent(f, indent+4);
                 if (!clean) fprintf(f, checkpoint ? "come_scope_push_checkpoint();\n" : "come_scope_push();\n");
                 scope_depth_push();
                 for(int i=0; i<body->child_count; i++) generate_guarded_stmt(f, body->children[i], indent+4);
                 scope_depth_pop();
                 // A body ending in a return never reaches
                 // its pop: the return already popped to
                 // the marker.
                 if (!clean && (body->child_count == 0 ||
                     body->children[body->child_count - 1]->type != AST_RETURN)) {
                     emit_indent(f, indent+4);
                     fprintf(f, "come_scope_pop();\n");
                 }
            } else {
                 generate_node(f, body, indent+4);
            }
            bound_proof_restore(saved_loop, saved_loop_count);
            g_loop_depth_count--;
            g_break_depth--;
            g_continue_depth--;
            emit_indent(f, indent);
            fprintf(f, "}\n");
            break;
        }

        case AST_DO_WHILE: {
            emit_line_directive(f, node);
            emit_indent(f, indent);
            fprintf(f, "do {\n");
            g_break_depth++;
            g_continue_depth++;
            if (g_loop_depth_count < 64) g_loop_depths[g_loop_depth_count++] = g_scope_depth;
             ASTNode* body = node->children[0];
            // Parity step 2: do-while bodies start cleared
            // (the cond runs after the body, so it cannot
            // guard the first iteration); no loop-cond proof.
            BoundProof saved_do[64];
            int saved_do_count = bound_proof_save(saved_do);
            bound_proof_clear();
            if (body->type == AST_BLOCK) {
                 // Clean blocks (redesign): a frame that
                 // would be empty is skipped; lexical
                 // depth counting is unchanged.
                 int clean = block_is_clean(body);
                 int checkpoint = !clean && block_is_checkpoint(body);
                 emit_indent(f, indent+4);
                 if (!clean) fprintf(f, checkpoint ? "come_scope_push_checkpoint();\n" : "come_scope_push();\n");
                 scope_depth_push();
                 for(int i=0; i<body->child_count; i++) generate_guarded_stmt(f, body->children[i], indent+4);
                 scope_depth_pop();
                 // A body ending in a return never reaches
                 // its pop: the return already popped to
                 // the marker.
                 if (!clean && (body->child_count == 0 ||
                     body->children[body->child_count - 1]->type != AST_RETURN)) {
                     emit_indent(f, indent+4);
                     fprintf(f, "come_scope_pop();\n");
                 }
            } else {
                 generate_node(f, body, indent+4);
            }
            bound_proof_restore(saved_do, saved_do_count);
            g_loop_depth_count--;
            g_break_depth--;
            g_continue_depth--;
            emit_indent(f, indent);
            fprintf(f, "} while (");
            generate_expression(f, node->children[1]);
            fprintf(f, ");\n");
            break;
        }
        
        
        case AST_CALL:
        case AST_POST_INC:
        case AST_POST_DEC:
        case AST_UNARY_OP:
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
                    // Register the loop variable: the body
                    // refers to it by name (COIL-129).
                    add_local_variable(decl->text, type->text);
                    fprintf(f, "%s %s = ", type->text, decl->text);
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
            g_break_depth++;
            g_continue_depth++;
            if (g_loop_depth_count < 64) g_loop_depths[g_loop_depth_count++] = g_scope_depth;
            ASTNode* body = node->children[3];
            // Parity step 2: a counted for proves its pair
            // at the top of every iteration (init nonneg +
            // cond i < len + nonneg update); other fors
            // start cleared like while bodies.
            char fbuf[128], fidx[128];
            int fproof = 0;
            if (node->child_count > 2 && node->children[0] && node->children[1] && node->children[2])
                fproof = bound_for_counted(node->children[0], node->children[1], node->children[2], fbuf, fidx);
            BoundProof saved_for[64];
            int saved_for_count = bound_proof_save(saved_for);
            bound_proof_clear();
            if (fproof) bound_proof_add(fbuf, fidx);
            if (body->type == AST_BLOCK) {
                fprintf(f, "{\n");
                // Clean blocks (redesign): a frame that
                // would be empty is skipped; lexical
                // depth counting is unchanged.
                int clean = block_is_clean(body);
                int checkpoint = !clean && block_is_checkpoint(body);
                emit_indent(f, indent + 4);
                if (!clean) fprintf(f, checkpoint ? "come_scope_push_checkpoint();\n" : "come_scope_push();\n");
                scope_depth_push();
                for (int i = 0; i < body->child_count; i++) {
                    generate_guarded_stmt(f, body->children[i], indent + 4);
                }
                scope_depth_pop();
                // A body ending in a return never reaches
                // its pop: the return already popped to
                // the marker.
                if (!clean && (body->child_count == 0 ||
                    body->children[body->child_count - 1]->type != AST_RETURN)) {
                    emit_indent(f, indent + 4);
                    fprintf(f, "come_scope_pop();\n");
                }
                emit_indent(f, indent);
                fprintf(f, "}\n");
            } else {
                // COIL-159: a bare body must be braced.
                // generate_node prepends the err_arm
                // statement, and without braces the arm -
                // not the body - became the loop body in
                // C, so the body ran once after the loop
                // (silent wrong results since COIL-139).
                // Plain C braces only: no scope frame,
                // preserving the bare body's semantics.
                fprintf(f, "{\n");
                generate_node(f, body, indent + 4);
                emit_indent(f, indent);
                fprintf(f, "}\n");
            }
            bound_proof_restore(saved_for, saved_for_count);
            g_loop_depth_count--;
            g_break_depth--;
            g_continue_depth--;
            break;
        }

        case AST_BREAK: {
            if (g_break_depth == 0) {
                codegen_error("'break' has no loop or switch to break out of - it is only allowed inside a for, while, do-while, or switch body");
            }
            emit_indent(f, indent);
            // COIL-157: unwind the current iteration's
            // scope (and any nested blocks) before the
            // jump, or the iteration's buffers would
            // survive until the function returns.
            if (g_loop_depth_count > 0)
                fprintf(f, "come_scope_pop_to_depth(%d);\n", g_loop_depths[g_loop_depth_count - 1]);
            emit_indent(f, indent);
            fprintf(f, "break;\n");
            break;
        }
        case AST_CONTINUE: {
            if (g_continue_depth == 0) {
                codegen_error("'continue' has no loop to continue - it is only allowed inside a for, while, or do-while body");
            }
            emit_indent(f, indent);
            if (g_loop_depth_count > 0)
                fprintf(f, "come_scope_pop_to_depth(%d);\n", g_loop_depths[g_loop_depth_count - 1]);
            emit_indent(f, indent);
            fprintf(f, "continue;\n");
            break;
        }
        default:
            break;
    }
}



// Render one parameter of a wrapper signature, mirroring
// the definition's own rendering so the two always agree.
static void emit_wrapper_param(FILE* f, ASTNode* arg, int with_name) {
    ASTNode* type = arg->children[1];
    const char* nm = with_name ? arg->text : "";
    if (is_fn_type(type->text)) {
        emit_fn_param(f, type->text, with_name ? arg->text : NULL);
    } else if (strstr(type->text, "[]")) {
        char raw[64];
        strncpy(raw, type->text, strlen(type->text) - 2);
        raw[strlen(type->text) - 2] = 0;
        if (strcmp(raw, "int") == 0) fprintf(f, "come_int_array_t %s", nm);
        else if (strcmp(raw, "byte") == 0) fprintf(f, "come_byte_array_t %s", nm);
        else if (strcmp(raw, "string") == 0) fprintf(f, "come_string_list_t %s", nm);
        else fprintf(f, "come_array_t %s", nm);
    } else if (strcmp(type->text, "link") == 0) {
        fprintf(f, "come_link_t* %s", nm);
    } else if (codegen_module_can_instance(type->text)) {
        fprintf(f, "come_%s__instance_t* %s", type->text, nm);
    } else if (is_ref_type(type->text)) {
        char ct[300];
        come_type_to_c(type->text, ct, sizeof(ct));
        fprintf(f, "%s %s", ct, nm);
    } else if (strcmp(type->text, "string") == 0) {
        fprintf(f, "come_string_t %s", nm);
    } else {
        char ct[300];
        come_type_to_c(type->text, ct, sizeof(ct));
        fprintf(f, "%s %s", ct, nm);
    }
}

// The instance wrapper for a stateless function of a
// stateful module (COIL-140): instance calls reach the
// plain function through here so the destroyed-instance
// guard applies to every method of the instance.
static void emit_instance_wrapper(FILE* f, ASTNode* fn) {
    ASTNode* ret = fn->children[0];
    int body_idx = fn->child_count - 1;
    if (ret->text[0] == '(')
        fprintf(f, "come_%s__%s_ret_t come_%s__%s__inst(come_%s__instance_t* __inst",
                current_module, fn->text, current_module, fn->text, current_module);
    else if (strcmp(ret->text, "string") == 0)
        fprintf(f, "come_string_t come_%s__%s__inst(come_%s__instance_t* __inst",
                current_module, fn->text, current_module);
    else {
        char ct[300];
        come_type_to_c(ret->text, ct, sizeof(ct));
        fprintf(f, "%s come_%s__%s__inst(come_%s__instance_t* __inst",
                ct, current_module, fn->text, current_module);
    }
    for (int i = 1; i < body_idx; i++) {
        ASTNode* arg = fn->children[i];
        if (arg->type != AST_VAR_DECL) continue;
        fprintf(f, ", ");
        emit_wrapper_param(f, arg, 1);
    }
    fprintf(f, ") {\n");
    emit_instance_guard(f, fn, 4);
    fprintf(f, "    ");
    if (strcmp(ret->text, "void") != 0) fprintf(f, "return ");
    fprintf(f, "come_%s__%s(", current_module, fn->text);
    int first = 1;
    for (int i = 1; i < body_idx; i++) {
        ASTNode* arg = fn->children[i];
        if (arg->type != AST_VAR_DECL) continue;
        if (!first) fprintf(f, ", ");
        fprintf(f, "%s", arg->text);
        first = 0;
    }
    fprintf(f, ");\n}\n\n");
}


// Render one parameter type of an imported function's
// prototype (the type text only; the rendering mirrors the
// prototype pre-pass in the defining file).
static void emit_proto_param(FILE* f, const char* t) {
    while (*t == ' ') t++;
    if (is_fn_type(t)) {
        emit_fn_param(f, t, NULL);
    } else if (strlen(t) >= 2 && strcmp(t + strlen(t) - 2, "[]") == 0) {
        char raw[64];
        snprintf(raw, sizeof(raw), "%.*s", (int)strlen(t) - 2, t);
        if (strcmp(raw, "int") == 0) fprintf(f, "come_int_array_t");
        else if (strcmp(raw, "byte") == 0) fprintf(f, "come_byte_array_t");
        else if (strcmp(raw, "string") == 0) fprintf(f, "come_string_list_t");
        else fprintf(f, "come_array_t");
    } else if (strcmp(t, "string") == 0) {
        fprintf(f, "come_string_t");
    } else if (strcmp(t, "link") == 0) {
        fprintf(f, "come_link_t*");
    } else if (codegen_module_can_instance(t)) {
        fprintf(f, "come_%s__instance_t*", t);
    } else if (is_ref_type(t)) {
        char ct[300];
        come_type_to_c(t, ct, sizeof(ct));
        fprintf(f, "%s", ct);
    } else {
        // Through the type translator (COIL-151): a
        // map{...} parameter is come_map_t in C.
        char ct[300];
        come_type_to_c(t, ct, sizeof(ct));
        fprintf(f, "%s", ct);
    }
}

// Prototypes for another module's functions, emitted into
// an importer's file (COIL-140 for stateful modules,
// COIL-146 for every module): without them a call is
// implicitly declared as returning int, which corrupts
// string, struct, and tuple returns alike. With `own` set
// the module is the file's own (a directory module's other
// files): everything is visible, and functions this file
// itself defines are skipped - the local passes declare
// those. For an import, only exported symbols are declared
// (spec 3.4), and the __inst twin exists only when the
// module has an instance surface at all.
static void emit_import_prototypes(FILE* f, const char* mod, int own) {
    int n = codegen_module_fn_count(mod);
    int has_state = codegen_module_can_instance(mod);
    for (int i = 0; i < n; i++) {
        const char *nm, *ret, *params;
        if (!codegen_module_fn_at(mod, i, &nm, &ret, &params)) continue;
        if (own) {
            if (find_module_fn_sig(nm)) continue;
        } else if (!module_symbol_visible(mod, nm)) {
            continue;
        }
        int st = codegen_fn_stateful(mod, nm, NULL);
        char rbuf[320];
        if (ret[0] == '(') snprintf(rbuf, sizeof(rbuf), "come_%s__%s_ret_t", mod, nm);
        else if (strcmp(ret, "string") == 0) snprintf(rbuf, sizeof(rbuf), "come_string_t");
        else come_type_to_c(ret, rbuf, sizeof(rbuf));
        char parts[32][160];
        int np = params[0] ? split_types(params, parts, 32) : 0;
        fprintf(f, "%s come_%s__%s(", rbuf, mod, nm);
        if (st) fprintf(f, "come_%s__instance_t*", mod);
        else if (np == 0) fprintf(f, "void");
        for (int j = 0; j < np; j++) {
            if (st || j > 0) fprintf(f, ", ");
            emit_proto_param(f, parts[j]);
        }
        fprintf(f, ");\n");
        if (!st && has_state) {
            fprintf(f, "%s come_%s__%s__inst(come_%s__instance_t*", rbuf, mod, nm, mod);
            for (int j = 0; j < np; j++) {
                fprintf(f, ", ");
                emit_proto_param(f, parts[j]);
            }
            fprintf(f, ");\n");
        }
    }
}

// One imported struct definition, rendered exactly as the
// defining module renders it (COIL-146): the layout an
// importer's field accesses and by-value calls are
// compiled against must be the defining layout.
static void emit_imported_struct(FILE* f, const char* name, const char* def,
                                 int packed, int align) {
    fprintf(f, "struct");
    if (packed) fprintf(f, " __attribute__((packed))");
    if (align > 0) fprintf(f, " __attribute__((aligned(%d)))", align);
    fprintf(f, " %s {\n", name);
    const char* p = def;
    while (*p) {
        const char* semi = strchr(p, ';');
        size_t flen = semi ? (size_t)(semi - p) : strlen(p);
        const char* us = memchr(p, '\x1f', flen);
        if (us) {
            char fname[128], ftype[160];
            snprintf(fname, sizeof(fname), "%.*s", (int)(us - p), p);
            snprintf(ftype, sizeof(ftype), "%.*s", (int)(flen - (size_t)(us - p) - 1), us + 1);
            size_t tl = strlen(ftype);
            if (tl > 2 && strcmp(ftype + tl - 2, "[]") == 0)
                fprintf(f, "    come_hdr_t %s;\n", fname);
            else
                fprintf(f, "    %s %s;\n", ftype, fname);
        }
        if (!semi) break;
        p = semi + 1;
    }
    fprintf(f, "};\n");
    fprintf(f, "typedef struct %s %s;\n", name, name);
}

// The full surface one module presents to a translation
// unit that is not its defining file (COIL-146): exported
// struct definitions, tuple return typedefs, and exact
// prototypes, in dependency order (structs before the
// typedefs and prototypes that name them). With `own` set
// the surface is the rest of the file's own module - a
// directory module's sibling files - where export lists do
// not apply and definitions this file makes itself are
// skipped (its own passes emit those).
static void emit_module_surface(FILE* f, const char* mod, int own) {
    int ns = codegen_module_struct_count(mod);
    for (int i = 0; i < ns; i++) {
        const char *nm, *def;
        int pk, al;
        if (!codegen_module_struct_at(mod, i, &nm, &def, &pk, &al)) continue;
        if (!own && !module_symbol_visible(mod, nm)) continue;
        if (own) {
            int local = 0;
            for (int j = 0; j < g_struct_count; j++)
                if (strcmp(g_struct_names[j], nm) == 0) local = 1;
            if (local) continue;
        }
        emit_imported_struct(f, nm, def, pk, al);
    }
    // Constant externs (COIL-148): one namespaced
    // definition lives in the declaring file; this unit
    // declares it extern - sibling files unconditionally,
    // importers for exported constants.
    int nc = codegen_module_const_count(mod);
    for (int i = 0; i < nc; i++) {
        const char *nm, *ct;
        if (!codegen_module_const_at(mod, i, &nm, &ct)) continue;
        if (!own && !module_symbol_visible(mod, nm)) continue;
        fprintf(f, "extern const %s come_%s__const_%s;\n", ct, mod, nm);
    }
    int nf = codegen_module_fn_count(mod);
    for (int i = 0; i < nf; i++) {
        const char *nm, *ret, *params;
        if (!codegen_module_fn_at(mod, i, &nm, &ret, &params)) continue;
        if (!ret || ret[0] != '(') continue;
        if (!own && !module_symbol_visible(mod, nm)) continue;
        if (own && find_tuple_fn(nm)) continue;
        char inner[600];
        snprintf(inner, sizeof(inner), "%.*s", (int)strlen(ret) - 2, ret + 1);
        char parts[8][160];
        int np = split_types(inner, parts, 8);
        fprintf(f, "typedef struct { ");
        for (int j = 0; j < np; j++) {
            char ct[300];
            char* s = parts[j];
            while (*s == ' ') memmove(s, s + 1, strlen(s));
            size_t l = strlen(s);
            while (l > 0 && s[l - 1] == ' ') s[--l] = 0;
            come_type_to_c(s, ct, sizeof(ct));
            fprintf(f, "%s _%d; ", ct, j);
        }
        fprintf(f, "} come_%s__%s_ret_t;\n", mod, nm);
    }
    emit_import_prototypes(f, mod, own);
}

// The instance runtime of a stateful module, emitted by
// its state home file after all functions: field
// initializers (the module variables' own initializers,
// re-homed), create(), and destroy().
static void emit_instance_runtime(FILE* f, ASTNode* ast) {
    int has_init = g_codegen_group_has_init;
    int has_exit = g_codegen_group_has_exit;
    if (has_init < 0 || has_exit < 0) {
        int hi = 0, he = 0;
        for (int i = 0; i < ast->child_count; i++) {
            ASTNode* ch = ast->children[i];
            if (!ch || ch->type != AST_FUNCTION) continue;
            if (strcmp(ch->text, "create") == 0 || strcmp(ch->text, "module_create") == 0) hi = 1;
            if (strcmp(ch->text, "destroy") == 0 || strcmp(ch->text, "module_destroy") == 0) he = 1;
        }
        if (has_init < 0) has_init = hi;
        if (has_exit < 0) has_exit = he;
    }
    fprintf(f, "static void come_%s__init_fields(come_%s__instance_t* __inst) {\n",
            current_module, current_module);
    {
        int save = g_fn_instance_form;
        g_fn_instance_form = 1;
        for (int i = 0; i < ast->child_count; i++) {
            ASTNode* v = ast->children[i];
            if (!v || v->type != AST_VAR_DECL) continue;
            const char* ftype = NULL;
            if (codegen_module_var_index(current_module, v->text, &ftype) < 0) continue;
            ASTNode* init = v->child_count > 0 ? v->children[0] : NULL;
            // The parser's dummy 0 for "no initializer":
            // the field is already zero from create's memset.
            if (!init || (init->type == AST_NUMBER && strcmp(init->text, "0") == 0)) {
                // COIL-157: a header-typed field with no
                // initializer still needs its ctx seeded
                // to the instance, or its first growth
                // would land in the caller's scope.
                if (ftype && (strcmp(ftype, "string") == 0 || strncmp(ftype, "map{", 4) == 0 ||
                              (strchr(ftype, '[') && !type_is_fixed_array(ftype))))
                    fprintf(f, "    __inst->%s.ctx = __inst->ctx;\n", v->text);
                continue;
            }
            fprintf(f, "    __inst->%s = ", v->text);
            if (init->type == AST_STRING_LITERAL) {
                fprintf(f, "come_string_new(COME_CTX, ");
                generate_expression(f, init);
                fprintf(f, ")");
            } else {
                generate_expression(f, init);
            }
            fprintf(f, ";\n");
            // COIL-157: the initializer ran in the
            // creator's scope, but the field must live
            // as long as the instance, so adopt it.
            if (ftype && type_needs_adopt(ftype)) {
                // Sized past the longest identifier the
                // lexer can hold (4095 bytes).
                char flv[4200];
                snprintf(flv, sizeof(flv), "__inst->%s", v->text);
                emit_adopt_value(f, 4, flv, ftype, "__inst->ctx");
            }
        }
        g_fn_instance_form = save;
    }
    // The hooks take the instance as an argument only
    // when the module holds per-instance state; a
    // variable-less module's hooks are plain functions
    // (COIL-156).
    if (has_init) {
        if (cur_module_per_instance())
            fprintf(f, "    come_%s__init_local(__inst);\n", current_module);
        else
            fprintf(f, "    come_%s__init_local();\n", current_module);
    }
    fprintf(f, "}\n\n");
    fprintf(f, "come_%s__instance_t* come_%s__create(void) {\n", current_module, current_module);
    fprintf(f, "    come_%s__instance_t* __inst = mem_talloc_alloc(come_%s__ctx, sizeof(come_%s__instance_t));\n",
            current_module, current_module, current_module);
    fprintf(f, "    memset(__inst, 0, sizeof(come_%s__instance_t));\n", current_module);
    fprintf(f, "    __inst->ctx = mem_talloc_new_ctx(NULL);\n");
    fprintf(f, "    __inst->alive = 1;\n");
    fprintf(f, "    come_%s__init_fields(__inst);\n", current_module);
    fprintf(f, "    return __inst;\n}\n\n");
    fprintf(f, "void come_%s__destroy(come_%s__instance_t* __inst) {\n", current_module, current_module);
    fprintf(f, "    if (!__inst || !__inst->alive) return;\n");
    if (has_exit) {
        if (cur_module_per_instance())
            fprintf(f, "    come_%s__exit_local(__inst);\n", current_module);
        else
            fprintf(f, "    come_%s__exit_local();\n", current_module);
    }
    fprintf(f, "    mem_talloc_free(__inst->ctx);\n");
    fprintf(f, "    memset(__inst, 0, sizeof(come_%s__instance_t));\n", current_module);
    fprintf(f, "}\n\n");
}

int generate_c_from_ast(ASTNode* ast, const char* out_file, const char* source_file, int gen_line_map) {
    FILE* f = fopen(out_file, "w");
    if (!f) return 1;
    
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

    g_codegen_errors = 0;

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

    // COIL-111 (spec 13, 3.3): extern "C" declarations may appear
    // Tuple functions are collected before every pass: the
    // typedef and prototype passes need them (COIL-127).
    collect_tuple_fns(ast);

    // only in system modules. The driver marks system-module
    // generation (source inside the compiler's own tree); the
    // default is a user module.
    if (!g_codegen_system_module && ast->type == AST_PROGRAM) {
        for (int i = 0; i < ast->child_count; i++) {
            ASTNode* ch = ast->children[i];
            if (ch && ch->type == AST_FUNCTION && ch->is_extern)
                codegen_error("extern \"C\" is only allowed in system modules (spec 13): '%s' in module '%s'",
                              ch->text, current_module);
        }
    }


    fprintf(f, "#include <stdio.h>\n");
    fprintf(f, "#include <string.h>\n");
    fprintf(f, "#include <stdbool.h>\n");
    fprintf(f, "#include <stdint.h>\n");
    fprintf(f, "#include \"come_string.h\"\n");
    fprintf(f, "#include \"come_array.h\"\n");
    fprintf(f, "#include \"come_map.h\"\n");
    fprintf(f, "#include \"come_coro.h\"\n");
    fprintf(f, "#include \"come_link.h\"\n");
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
        fprintf(f, "extern come_string_t come_ERR_str(void);\n");
        fprintf(f, "extern void come_ERR_clear(void);\n");
        fprintf(f, "extern void come_ERR_fatal(come_string_t msg);\n");
        fprintf(f, "extern void come_err_arm(const char*, const char*, const char*, int);\n");
        fprintf(f, "extern int come_ERR_no_at(int, const char*, const char*, const char*, int);\n");
        fprintf(f, "extern come_string_t come_ERR_str_at(int, const char*, const char*, const char*, int);\n");
        fprintf(f, "extern come_string_t come_ERR_where_at(int, const char*, const char*, const char*, int);\n");
        fprintf(f, "extern int come_ERR_depth(void);\n");
        fprintf(f, "extern void come_ERR_fatal_at(come_string_t, const char*, const char*, const char*, int);\n");
        // We need the type for ERR object too
        fprintf(f, "typedef struct come_std__ERR_t come_std__ERR_t;\n");
        fprintf(f, "extern come_std__ERR_t come_std__ERR;\n");
        // FILE object type and runtime (COIL-114, Come_std.md)
        fprintf(f, "#include <stdarg.h>\n");
        fprintf(f, "#include \"come_std.h\"\n");
        fprintf(f, "extern come_std__FILE_t std_in;\n");
        fprintf(f, "extern come_std__FILE_t std_out;\n");
        fprintf(f, "extern come_std__FILE_t std_err;\n");
        fprintf(f, "void come_std__FILE__init(come_std__FILE_t*);\n");
        fprintf(f, "void come_std__FILE__exit(come_std__FILE_t*);\n");
        fprintf(f, "bool come_std__FILE__open(come_std__FILE_t*, come_string_t, come_string_t);\n");
        fprintf(f, "void come_std__FILE__close(come_std__FILE_t*);\n");
        fprintf(f, "bool come_std__FILE__fdopen(come_std__FILE_t*, int, come_string_t);\n");
        fprintf(f, "bool come_std__FILE__reopen(come_std__FILE_t*, come_string_t, come_string_t);\n");
        fprintf(f, "int come_std__FILE__fileno(come_std__FILE_t*);\n");
        fprintf(f, "int come_std__FILE__printf(come_std__FILE_t*, come_string_t, ...);\n");
        fprintf(f, "int come_std__FILE__scanf(come_std__FILE_t*, come_string_t, ...);\n");
        fprintf(f, "int come_std__FILE__vprintf(come_std__FILE_t*, come_string_t, va_list);\n");
        fprintf(f, "int come_std__FILE__vscanf(come_std__FILE_t*, come_string_t, va_list);\n");
        fprintf(f, "long come_std__FILE__read(come_std__FILE_t*, come_byte_array_t, long);\n");
        fprintf(f, "long come_std__FILE__write(come_std__FILE_t*, come_byte_array_t, long);\n");
        fprintf(f, "int32_t come_std__FILE__getc(come_std__FILE_t*);\n");
        fprintf(f, "void come_std__FILE__putc(come_std__FILE_t*, int32_t);\n");
        fprintf(f, "come_string_t come_std__FILE__gets(come_std__FILE_t*);\n");
        fprintf(f, "long come_std__FILE__puts(come_std__FILE_t*, come_string_t);\n");
        fprintf(f, "come_string_t come_std__FILE__fname(come_std__FILE_t*);\n");
        fprintf(f, "void come_std__FILE__ungetc(come_std__FILE_t*, int32_t);\n");
        fprintf(f, "void come_std__FILE__seek(come_std__FILE_t*, long, int);\n");
        fprintf(f, "long come_std__FILE__tell(come_std__FILE_t*);\n");
        fprintf(f, "void come_std__FILE__rewind(come_std__FILE_t*);\n");
        fprintf(f, "bool come_std__FILE__isopen(come_std__FILE_t*);\n");
        fprintf(f, "bool come_std__FILE__eof(come_std__FILE_t*);\n");
        fprintf(f, "bool come_std__FILE__error(come_std__FILE_t*);\n");
        fprintf(f, "void come_std__FILE__flush(come_std__FILE_t*);\n");
        fprintf(f, "void come_std__FILE__clearerr(come_std__FILE_t*);\n");
        fprintf(f, "void come_std__FILE__setbuf(come_std__FILE_t*, come_byte_array_t, long);\n");
        fprintf(f, "void come_std__FILE__setvbuf(come_std__FILE_t*, come_byte_array_t, int, long);\n");
        fprintf(f, "void come_std__FILE__setlinebuf(come_std__FILE_t*);\n");
        fprintf(f, "bool come_std__remove(come_string_t);\n");
        fprintf(f, "come_std__FILE_t come_std__tmpfile(void);\n");
        fprintf(f, "come_string_t come_std__tmpname(void);\n");
        fprintf(f, "int come_std__rand(void);\n");
        fprintf(f, "void come_std__srand(unsigned int);\n");
        fprintf(f, "void come_std__qsort(come_byte_array_t, int (*)(int8_t, int8_t));\n");
        fprintf(f, "void come_std__Proc__abort(void*);\n");
        fprintf(f, "void come_std__Proc__exit(void*, int);\n");
        fprintf(f, "void come_std__Proc__atexit(void*, void (*)(void));\n");
        fprintf(f, "come_string_t come_std__Proc__getenv(void*, come_string_t);\n");
        fprintf(f, "int come_std__Proc__system(void*, come_string_t);\n");
    }
    // Macros for method dispatch
    fprintf(f, "#define COME_CTX come_scope_current()\n\n");

    // Forward declarations for imported stateful modules
    // come before everything else (COIL-154): this
    // module's own instance struct can hold another
    // module's instance as a field, so the imported
    // instance types must be named before it.
    for (int i = 0; i < current_import_count; i++) {
        if (codegen_module_can_instance(current_imports[i])) {
            fprintf(f, "typedef struct come_%s__instance_s come_%s__instance_t;\n",
                    current_imports[i], current_imports[i]);
            fprintf(f, "come_%s__instance_t* come_%s__create(void);\n",
                    current_imports[i], current_imports[i]);
            fprintf(f, "void come_%s__destroy(come_%s__instance_t*);\n",
                    current_imports[i], current_imports[i]);
        }
    }

    // Module instances (COIL-140): a stateful module's
    // variables become the fields of its instance struct.
    // The struct is defined here, before every prototype;
    // importers get an opaque forward typedef plus the
    // create/destroy declarations.
    if (cur_module_can_instance()) {
        ModuleState* ms = module_state_of(current_module);
        fprintf(f, "typedef struct come_%s__instance_s {\n", current_module);
        fprintf(f, "    TALLOC_CTX* ctx;\n");
        fprintf(f, "    int alive;\n");
        for (int i = 0; i < ms->var_count; i++) {
            char ct[300];
            // An instance-typed field (COIL-154): the
            // variable's type names another stateful
            // module, and the field holds that module's
            // instance pointer - the same shape an
            // instance parameter takes.
            if (codegen_module_can_instance(ms->var_types[i]))
                snprintf(ct, sizeof(ct), "come_%s__instance_t*", ms->var_types[i]);
            else
                come_type_to_c(ms->var_types[i], ct, sizeof(ct));
            fprintf(f, "    %s %s;\n", ct, ms->var_names[i]);
        }
        fprintf(f, "} come_%s__instance_t;\n", current_module);
        fprintf(f, "come_%s__instance_t* come_%s__create(void);\n", current_module, current_module);
        fprintf(f, "void come_%s__destroy(come_%s__instance_t*);\n\n", current_module, current_module);
    }
    for (int i = 0; i < current_import_count; i++) {
        // Every import's exported surface - struct
        // definitions, tuple typedefs, exact prototypes
        // (COIL-146). System modules are not in the
        // registry; their surface is the runtime headers
        // and the hand-written prototypes above. (The
        // instance forward declarations for stateful
        // imports were emitted above, before this module's
        // own instance struct - COIL-154.)
        emit_module_surface(f, current_imports[i], 0);
    }
    // The rest of this file's own module (COIL-146): a
    // directory module's sibling files define structs and
    // functions this file uses, and their definitions and
    // prototypes belong here too. The collections the
    // local checks consult run here as well as at the
    // generation tail - both passes collect the same file.
    collect_structs(ast);
    collect_module_fns(ast);
    emit_module_surface(f, current_module, 1);
    
    // Module memory context
    if (g_codegen_suppress_singletons) {
        // Non-primary file of a directory module: the primary file
        // defines the ctx global (COIL-108).
        fprintf(f, "extern TALLOC_CTX* come_%s__ctx;\n", current_module);
    } else {
        fprintf(f, "TALLOC_CTX* come_%s__ctx = NULL;\n", current_module);
    }
    
    // TODO: Extern imports - disabled for now to avoid linker errors
    // for (int i=0; i<current_import_count; i++) {
    //     fprintf(f, "extern TALLOC_CTX* come_%s__ctx;\n", current_imports[i]);
    // }

    // Only generate main if it's not a base module
    if (strcmp(current_module, "std") != 0 && strcmp(current_module, "string") != 0 && 
        strcmp(current_module, "array") != 0 && strcmp(current_module, "map") != 0) {

        // Scan AST to find main function and check if it has parameters
        int main_has_params = 0;
        int has_main_fn = 0;
        int main_is_void = 0;
        for (int i = 0; i < ast->child_count; i++) {
            ASTNode* child = ast->children[i];
            if (child && child->type == AST_FUNCTION && strcmp(child->text, "main") == 0) {
                has_main_fn = 1;
                // Spec 2.1: void main is one of the four entry
                // signatures; its prototype and call must be
                // void (COIL-127).
                if (child->child_count > 0 && child->children[0] &&
                    strcmp(child->children[0]->text, "void") == 0) {
                    main_is_void = 1;
                }
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

        // The C entry point belongs to the module that defines
        // main(). Library modules must not emit one: previously every
        // file got a wrapper, so any multi-module program failed to
        // link with duplicate main / undefined come_<lib>__main
        // (COIL-108).
        if (has_main_fn) {
        // Forward declare user main with correct signature
        if (main_has_params) {
            fprintf(f, "%s come_%s__main(come_string_list_t args);\n", main_is_void ? "void" : "int", current_module);
        } else {
            fprintf(f, "%s come_%s__main(void);\n", main_is_void ? "void" : "int", current_module);
        }

        // Forward declare module init/exit
        fprintf(f, "void come_%s__init_chain(void);\n", current_module);
        fprintf(f, "void come_%s__exit_chain(void);\n", current_module);

        fprintf(f, "\nint main(int argc, char* argv[]) {\n");
        // The entry module's context is set by its init
        // chain below (as for every module); allocations
        // before any scope is pushed land on the root.
        
        fprintf(f, "    come_coro_init();\n");
        fprintf(f, "    come_%s__init_chain();\n", current_module);
        fprintf(f, "    \n");
        
        if (main_is_void) {
            // Void main implies exit code 0 (spec 2.1).
            if (main_has_params) {
                fprintf(f, "    come_string_list_t args = come_string_list_from_argv(COME_CTX, argc, argv);\n");
                fprintf(f, "    come_%s__main(args);\n", current_module);
            } else {
                fprintf(f, "    come_%s__main();\n", current_module);
            }
            fprintf(f, "    int ret = 0;\n");
        } else if (main_has_params) {
            fprintf(f, "    // Convert argv to string[]\n");
            fprintf(f, "    come_string_list_t args = come_string_list_from_argv(COME_CTX, argc, argv);\n");
            fprintf(f, "    \n");
            fprintf(f, "    // Call user main\n");
            fprintf(f, "    int ret = come_%s__main(args);\n", current_module);
        } else {
            fprintf(f, "    // Call user main (no args)\n");
            fprintf(f, "    int ret = come_%s__main();\n", current_module);
        }
        
        fprintf(f, "    \n");
        fprintf(f, "    come_run_exits();\n");
        fprintf(f, "    mem_talloc_free(come_%s__ctx);\n", current_module);
        fprintf(f, "    return ret;\n");
        fprintf(f, "}\n");
        } // if (has_main_fn)
    }

    // Generate Module Init/Exit Chain
    fprintf(f, "\n/* Module Init/Exit Chain */\n");

    // The chain follows this module's imports. For a directory
    // module the driver supplies the union of the group's imports
    // (COIL-108); otherwise the file's own imports are used.
    const char** chain_imports = g_codegen_chain_imports ? g_codegen_chain_imports
                                                         : (const char**)current_imports;
    int chain_count = g_codegen_chain_imports ? g_codegen_chain_import_count
                                              : current_import_count;

    // Forward declare imported init/exit
    for (int i = 0; i < chain_count; i++) {
        fprintf(f, "extern void come_%s__init_chain(void);\n", chain_imports[i]);
        fprintf(f, "extern void come_%s__exit_chain(void);\n", chain_imports[i]);
    }

    if (!g_codegen_suppress_singletons) {
    fprintf(f, "void come_%s__init_chain(void);\n", current_module);
    fprintf(f, "void come_%s__exit_chain(void);\n", current_module);
    // Module Init
    fprintf(f, "void come_%s__init_chain(void) {\n", current_module);
    fprintf(f, "    static bool initialized = false;\n");
    fprintf(f, "    if (initialized) return;\n");
    fprintf(f, "    initialized = true;\n");
    // COIL-157: every module gets a real context here (it
    // used to stay NULL for non-entry modules, which made
    // their allocations root-owned). Module variables
    // adopted from a function scope land here.
    fprintf(f, "    come_%s__ctx = mem_talloc_new_ctx(NULL);\n", current_module);
    // Register this module's memory context with the coroutine
    // scheduler (COIL-120): context switches swap it per coroutine.
    fprintf(f, "    come_coro_register_ctx(&come_%s__ctx);\n", current_module);
    for (int i = 0; i < chain_count; i++) {
        fprintf(f, "    come_%s__init_chain();\n", chain_imports[i]);
    }
    // Call the module's init hook if one is defined (init() or the
    // spec 3.5 spelling module_init(); mangled to __init_local).
    // For a directory module the driver reports the group fact.
    int has_local_init = g_codegen_group_has_init;
    if (has_local_init < 0) {
        has_local_init = 0;
        for (int i = 0; i < ast->child_count; i++) {
            if (ast->children[i]->type == AST_FUNCTION &&
                (strcmp(ast->children[i]->text, "create") == 0 ||
                 strcmp(ast->children[i]->text, "module_create") == 0)) {
                has_local_init = 1;
                break;
            }
        }
    }
    if (has_local_init && !cur_module_per_instance()) {
        fprintf(f, "    come_%s__init_local();\n", current_module);
    }
    // Register this module's exit: run_exits replays registrations
    // in reverse, giving exact reverse-of-init exit order (3.5).
    fprintf(f, "    come_register_exit(come_%s__exit_chain);\n", current_module);
    fprintf(f, "}\n\n");

    // Module Exit
    fprintf(f, "void come_%s__exit_chain(void) {\n", current_module);
    fprintf(f, "    static bool exited = false;\n");
    fprintf(f, "    if (exited) return;\n");
    fprintf(f, "    exited = true;\n");
    // Call the module's exit hook if defined (exit()/module_exit())
    int has_local_exit = g_codegen_group_has_exit;
    if (has_local_exit < 0) {
        has_local_exit = 0;
        for (int i = 0; i < ast->child_count; i++) {
            if (ast->children[i]->type == AST_FUNCTION &&
                (strcmp(ast->children[i]->text, "destroy") == 0 ||
                 strcmp(ast->children[i]->text, "module_destroy") == 0)) {
                has_local_exit = 1;
                break;
            }
        }
    }
    if (has_local_exit && !cur_module_per_instance()) {
        fprintf(f, "    come_%s__exit_local();\n", current_module);
    }
    // Imported modules' exits are NOT called from here: each module
    // registered its own exit at init time, and come_run_exits()
    // replays them in exact reverse initialization order (3.5).
    fprintf(f, "}\n");
    } // if (!g_codegen_suppress_singletons)
    // Map type (not in come_types.h as it's a special case)
    // Map type moved to come_map.h

    fprintf(f, "#include <math.h>\n");
    fprintf(f, "#include <stdlib.h>\n");
    fprintf(f, "#include <arpa/inet.h>\n"); // For htons

    // Runtime Preamble
    fprintf(f, "\n/* Runtime Preamble */\n");

    
    fprintf(f, "#define come_free(p) mem_talloc_free(p)\n");
    fprintf(f, "#define come_net_hton(x) htons(x)\n");
    
    
    
    // Array Resize Helpers
    

    fprintf(f, "/* Runtime Preamble additions */\n");

    
    fprintf(f, "#define come_std_eprintf(...) fprintf(stderr, __VA_ARGS__)\n");

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

    // Tuple return structs (spec 8.2, COIL-127): one typedef
    // per tuple-returning function, before any prototype.
    for (int i = 0; i < g_tuple_fn_count; i++) {
        TupleFn* tf = &g_tuple_fns[i];
        fprintf(f, "typedef struct { ");
        for (int j = 0; j < tf->count; j++) {
            char ct[300];
            come_type_to_c(tf->elems[j], ct, sizeof(ct));
            fprintf(f, "%s _%d; ", ct, j);
        }
        fprintf(f, "} come_%s__%s_ret_t;\n", current_module, tf->name);
    }

    // Forward Prototypes
    if (g_verbose) printf("DEBUG: Starting Pass forward prototypes\n");
    for (int i=0; i<ast->child_count; i++) {
        ASTNode* child = ast->children[i];
        if (child->type == AST_FUNCTION) {
             if (child->is_coroutine) {
                 // Coroutine (COIL-120): other code in the module
                 // needs the args struct type and the scheduler
                 // entry point, not a callable prototype.
                 fprintf(f, "typedef struct {\n");
                 for (int j = 1; j < child->child_count; j++) {
                     ASTNode* arg = child->children[j];
                     if (arg->type == AST_BLOCK) break;
                     if (arg->type != AST_VAR_DECL) continue;
                     fprintf(f, "    ");
                     emit_coro_field_type(f, arg->children[1]->text);
                     fprintf(f, " %s;\n", arg->text);
                 }
                 fprintf(f, "} come_%s__%s__args_t;\n", current_module, child->text);
                 fprintf(f, "void come_%s__%s__entry(void*);\n", current_module, child->text);
                 continue;
             }
             if (strcmp(child->text, "main") == 0) continue; // Skip main prototype
             if (g_verbose) printf("DEBUG: Mapping prototype for %s\n", child->text);
             // But `add` error requires it.
             // Simple loop:
             if (child->child_count > 0 && child->children[0]->type != AST_BLOCK) {
                  ASTNode* ret = child->children[0];
                  
                  char func_name[8192];
                  int is_main = (strcmp(child->text, "main") == 0);
                  char* underscore = strchr(child->text, '_');
                  if (child->is_extern) {
                      // extern "C": the raw C prototype (COIL-111)
                      snprintf(func_name, sizeof(func_name), "%s", child->text);
                  } else if (underscore && !is_main && isupper(child->text[0])) {
                      // Struct method: first char is uppercase
                      long prefix_len = underscore - child->text;
                      snprintf(func_name, sizeof(func_name), "come_%s__%.*s__%s", current_module, (int)prefix_len, child->text, underscore + 1);
                  } else if (strcmp(child->text, "create") == 0 || strcmp(child->text, "module_create") == 0) {
                      snprintf(func_name, sizeof(func_name), "come_%s__init_local", current_module);
                  } else if (strcmp(child->text, "destroy") == 0 || strcmp(child->text, "module_destroy") == 0) {
                      snprintf(func_name, sizeof(func_name), "come_%s__exit_local", current_module);
                  } else {
                      // Regular function
                      snprintf(func_name, sizeof(func_name), "come_%s__%s", current_module, child->text);
                  }


                  if (ret->text[0] == '(') {
                       fprintf(f, "come_%s__%s_ret_t %s(", current_module, child->text, func_name);
                  } else {
                       if (strcmp(ret->text, "string") == 0) fprintf(f, "come_string_t %s(", func_name);
                       else {
                           // Through the type translator
                           // (COIL-151): map{...} -> come_map_t.
                           char ct[300];
                           come_type_to_c(ret->text, ct, sizeof(ct));
                           fprintf(f, "%s %s(", ct, func_name);
                       }
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

             // COIL-140: instance-form prototypes carry the
             // hidden instance parameter too.
             int inst_first = fn_instance_form(child);
             if (inst_first) fprintf(f, "come_%s__instance_t*", current_module);

             int first = (strcmp(child->text, "nport")==0 || inst_first) ? 0 : 1;
             
             for (int j=start_args; j<child->child_count; j++) {
                 if (child->children[j]->type == AST_BLOCK) break;
                 if (!first) fprintf(f, ", ");
                 ASTNode* arg = child->children[j];
                 if (arg->type == AST_VAR_DECL) {
                     ASTNode* type = arg->children[1];
                     if (is_fn_type(type->text)) {
                            emit_fn_param(f, type->text, NULL); // COIL-109
                     // Array check
                     } else if (strstr(type->text, "[]")) {
                            char raw[64];
                            strncpy(raw, type->text, strlen(type->text)-2);
                            raw[strlen(type->text)-2] = 0;
                            
                            if (strcmp(raw, "int")==0) fprintf(f, "come_int_array_t");
                            else if (strcmp(raw, "byte")==0) fprintf(f, "come_byte_array_t");
                            else if (strcmp(raw, "string")==0) fprintf(f, "come_string_list_t");
                            else fprintf(f, "come_array_t");
                       } else if (type->text[0] == '(') {
                            fprintf(f, "void"); // Multi-return hack
                       } else {
                            if (strcmp(type->text, "string")==0) fprintf(f, "come_string_t");
                            else if (strcmp(type->text, "link")==0) fprintf(f, "come_link_t*");
                            else if (codegen_module_can_instance(type->text)) fprintf(f, "come_%s__instance_t*", type->text);
                            else if (is_ref_type(type->text)) {
                                // &(struct Vec3) -> struct Vec3* (COIL-136)
                                char ct[300];
                                come_type_to_c(type->text, ct, sizeof(ct));
                                fprintf(f, "%s", ct);
                            }
                            else {
                                // Through the type translator
                                // (COIL-151): map{...} ->
                                // come_map_t.
                                char ct[300];
                                come_type_to_c(type->text, ct, sizeof(ct));
                                fprintf(f, "%s", ct);
                            }
                       }
                  } else {
                     fprintf(f, "void*"); // Fallback
                 }
                 first = 0;
             }
             fprintf(f, ");\n");
        }
    }

    if (ast->type == AST_PROGRAM) {
        if (ast->text[0] != 0) {
            strncpy(current_module, ast->text, 255);
        }
        collect_module_fns(ast); // COIL-109
        collect_coroutines(ast); // COIL-120
        collect_module_vars(ast); // COIL-122
        collect_structs(ast); // COIL-127
        collect_tuple_fns(ast); // COIL-127
        collect_const_names(ast); // COIL-129
        generate_program(f, ast);
        // COIL-140: instance wrappers for this file's
        // stateless functions, and the instance runtime in
        // the state home file.
        if (cur_module_can_instance()) {
            for (int i = 0; i < ast->child_count; i++) {
                ASTNode* ch = ast->children[i];
                if (!ch || ch->type != AST_FUNCTION || ch->is_coroutine ||
                    ch->is_extern || fn_is_hook(ch->text))
                    continue;
                if (strcmp(ch->text, "main") == 0) continue;
                if (strchr(ch->text, '_') && isupper(ch->text[0])) continue; // struct method
                int has_body = 0;
                for (int k = 0; k < ch->child_count; k++)
                    if (ch->children[k] && ch->children[k]->type == AST_BLOCK) has_body = 1;
                if (!has_body) continue;
                if (!codegen_fn_stateful(current_module, ch->text, NULL))
                    emit_instance_wrapper(f, ch);
            }
            if (g_codegen_state_home) emit_instance_runtime(f, ast);
        }
    } else {
        generate_node(f, ast, 0);
    }

    fclose(f);
    return g_codegen_errors > 0 ? 1 : 0;
}
