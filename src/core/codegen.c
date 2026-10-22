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

// Semantic errors found during generation (COIL-101). Generation
// continues so the user can see every error, but generate_c_from_ast
// returns nonzero when any occurred and the driver refuses the output.
static int g_codegen_errors = 0;

// COIL-108: directory-module group control (see codegen.h).
int g_codegen_suppress_singletons = 0;
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

#include "utils.h"
#include <ctype.h>

static void generate_node(FILE* f, ASTNode* node, int indent);
static void generate_expression(FILE* f, ASTNode* node);

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
static void collect_module_fns(ASTNode* ast) {
    g_module_fn_count = 0;
    if (!ast || ast->type != AST_PROGRAM) return;
    for (int i = 0; i < ast->child_count && g_module_fn_count < 256; i++) {
        ASTNode* ch = ast->children[i];
        if (ch && ch->type == AST_FUNCTION) {
            if (isupper((unsigned char)ch->text[0]) && strchr(ch->text, '_'))
                continue; // struct method (Rect_area): needs a receiver
            FnSig* e = &g_module_fns[g_module_fn_count++];
            snprintf(e->name, sizeof(e->name), "%s", ch->text);
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
            snprintf(g_module_vars[g_module_var_count++], 128, "%s", ch->text);
        }
    }
}

static int is_module_var(const char* name) {
    for (int i = 0; i < g_module_var_count; i++) {
        if (strcmp(g_module_vars[i], name) == 0) return 1;
    }
    return 0;
}

// 1 while generating a coroutine body (COIL-122).
static int g_in_coroutine = 0;

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
            if (strcmp(raw, "int") == 0) fprintf(f, "come_int_array_t*");
            else if (strcmp(raw, "byte") == 0) fprintf(f, "come_byte_array_t*");
            else if (strcmp(raw, "string") == 0) fprintf(f, "come_string_list_t*");
            else fprintf(f, "come_array_t*");
        } else {
            fprintf(f, "come_array_t*");
        }
    } else if (strcmp(type_text, "string") == 0) {
        fprintf(f, "come_string_t*");
    } else if (strncmp(type_text, "map{", 4) == 0) {
        fprintf(f, "come_map_t*");
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
// Emit a C function-pointer parameter: RET (*name)(ARGS) from a
// fn: type text. `name` may be NULL for prototypes. Array argument
// types map to the runtime array types, as for ordinary parameters.
static void emit_fn_param(FILE* f, const char* fn_text, const char* name) {
    const char* lp = strchr(fn_text, '(');
    const char* rp = strrchr(fn_text, ')');
    if (!lp || !rp) { fprintf(f, "void* %s", name ? name : ""); return; }
    char ret[96];
    snprintf(ret, sizeof(ret), "%.*s", (int)(lp - (fn_text + 3)), fn_text + 3);
    char inner[512];
    snprintf(inner, sizeof(inner), "%.*s", (int)(rp - lp - 1), lp + 1);
    fprintf(f, "%s (*%s)(", ret, name ? name : "");
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
                if (strcmp(raw, "int") == 0) fprintf(f, "come_int_array_t*");
                else if (strcmp(raw, "byte") == 0) fprintf(f, "come_byte_array_t*");
                else if (strcmp(raw, "string") == 0) fprintf(f, "come_string_list_t*");
                else fprintf(f, "come_array_t*");
            } else {
                fprintf(f, "%s", parts[i]);
            }
        }
    }
    fprintf(f, ")");
}
// The declared parameter types of a same-module callee, split
// from its fn: signature. Returns the count (0 when unknown).
static int call_param_types(ASTNode* call, char parts[][160], int max_parts) {
    const char* csig = find_module_fn_sig(call->text);
    if (!csig) return 0;
    const char* lp = strchr(csig, '(');
    const char* rp = strrchr(csig, ')');
    if (!lp || !rp) return 0;
    char inner[512];
    snprintf(inner, sizeof(inner), "%.*s", (int)(rp - lp - 1), lp + 1);
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
        const char* bt = get_local_variable_type(lhs->children[0]->text);
        if (bt && strcmp(bt, "string") == 0)
            codegen_error("strings are immutable: '%s[i] = ...' is not allowed (spec 6.5) - build a new string instead",
                          lhs->children[0]->text);
    }
    if (lhs->type == AST_IDENTIFIER) {
        const char* lt = get_local_variable_type(lhs->text);
        if (lt) check_numeric_store(lt, rhs, lhs->text);
    } else if (lhs->type == AST_ARRAY_ACCESS && lhs->child_count >= 1 &&
               lhs->children[0]->type == AST_IDENTIFIER) {
        const char* bt = get_local_variable_type(lhs->children[0]->text);
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
            const char* t = get_local_variable_type(e->text);
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
    const char* t = get_local_variable_type(receiver->text);
    if (!t || strncmp(t, "map{", 4) != 0) return 0;
    return strcmp(method, "length") == 0 || strcmp(method, "has") == 0 ||
           strcmp(method, "delete") == 0 || strcmp(method, "clear") == 0 ||
           strcmp(method, "keys") == 0 || strcmp(method, "type") == 0 ||
           strcmp(method, "owner") == 0 || strcmp(method, "chown") == 0 ||
           strcmp(method, "dup") == 0;
}

// Emit an expression in a position that needs a come_string_t*:
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
static void generate_block_children(FILE* f, ASTNode* block, int indent) {
    for (int i = 0; i < block->child_count; i++) {
        ASTNode* ch = block->children[i];
        if (ch && ch->type == AST_RETURN) emit_file_closes_for(f, block, i, indent);
        generate_node(f, ch, indent);
    }
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
    if (!receiver || receiver->type != AST_IDENTIFIER) return 0;
    const char* rt = get_local_variable_type(receiver->text);
    return rt && strchr(rt, '[') != NULL;
}

// Emit one array toolbox call. The receiver is an array variable;
// growing operations assign the (possibly moved) array back.
static void emit_array_method(FILE* f, ASTNode* node, ASTNode* receiver, const char* method) {
    const char* rt = get_local_variable_type(receiver->text);
    char elem[64];
    const char* lb = rt ? strchr(rt, '[') : NULL;
    size_t el = lb ? (size_t)(lb - rt) : 0;
    if (el >= sizeof(elem)) el = sizeof(elem) - 1;
    memcpy(elem, rt ? rt : "", el);
    elem[el] = 0;
    const char* prefix = NULL;
    if (strcmp(elem, "int") == 0) prefix = "come_int_array_";
    else if (strcmp(elem, "byte") == 0) prefix = "come_byte_array_";
    else if (strcmp(elem, "string") == 0) prefix = "come_string_list_";
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
        fprintf(f, "COME_CTX");
        return;
    }
    if (strcmp(method, "chown") == 0) {
        fprintf(f, "mem_talloc_steal(");
        if (a1) generate_expression(f, a1);
        fprintf(f, ", ");
        generate_expression(f, receiver);
        fprintf(f, ")");
        return;
    }
    if (strcmp(method, "dup") == 0) {
        if (a1 && a1->type == AST_NUMBER && atoi(a1->text) == 0) {
            generate_expression(f, receiver);   // dup(0) shares the buffer
            return;
        }
        if (prefix) fprintf(f, "%sdup(COME_CTX, ", prefix);
        else fprintf(f, "come_array_dup_g(COME_CTX, ");
        generate_expression(f, receiver);
        if (!prefix) {
            fprintf(f, ", sizeof((");
            generate_expression(f, receiver);
            fprintf(f, ")->items[0])");
        }
        fprintf(f, ")");
        return;
    }
    if (strcmp(method, "clear") == 0) {
        fprintf(f, "come_array_clear_g(");
        generate_expression(f, receiver);
        fprintf(f, ")");
        return;
    }
    if (strcmp(method, "reverse") == 0) {
        fprintf(f, "come_array_reverse_g(");
        generate_expression(f, receiver);
        fprintf(f, ", sizeof((");
        generate_expression(f, receiver);
        fprintf(f, ")->items[0]))");
        return;
    }
    if (strcmp(method, "remove") == 0) {
        fprintf(f, "(");
        generate_expression(f, receiver);
        fprintf(f, " = come_array_remove_g(");
        generate_expression(f, receiver);
        fprintf(f, ", sizeof((");
        generate_expression(f, receiver);
        fprintf(f, ")->items[0]), ");
        if (a1) generate_expression(f, a1);
        fprintf(f, "))");
        return;
    }
    if (strcmp(method, "cpy") == 0) {
        fprintf(f, "(");
        generate_expression(f, receiver);
        if (prefix) fprintf(f, " = %scpy(", prefix);
        else fprintf(f, " = come_array_cpy_g(");
        generate_expression(f, receiver);
        if (!prefix) {
            fprintf(f, ", sizeof((");
            generate_expression(f, receiver);
            fprintf(f, ")->items[0])");
        }
        fprintf(f, ", ");
        if (a1) generate_expression(f, a1);
        fprintf(f, "))");
        return;
    }
    if (strcmp(method, "sort") == 0) {
        if (!prefix) {
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
        fprintf(f, "%ssort(", prefix);
        generate_expression(f, receiver);
        fprintf(f, ", ");
        if (lt && is_fn_type(lt)) generate_expression(f, a1);
        else if (find_module_fn_sig(a1->text)) fprintf(f, "come_%s__%s", current_module, a1->text);
        else codegen_error("sort: '%s' is not a function of this module", a1->text);
        fprintf(f, ")");
        return;
    }
    // append / insert / set / contains / index carry an element.
    ASTNode* elem_expr = (strcmp(method, "insert") == 0) ? a2 : a1;
    if (prefix) {
        if (strcmp(method, "append") == 0 || strcmp(method, "insert") == 0) {
            fprintf(f, "(");
            generate_expression(f, receiver);
            fprintf(f, " = %s%s(", prefix, method);
            generate_expression(f, receiver);
            if (strcmp(method, "insert") == 0) {
                fprintf(f, ", ");
                if (a1) generate_expression(f, a1);
            }
            fprintf(f, ", ");
            if (elem_expr) {
                if (strcmp(elem, "string") == 0) emit_string_expr(f, elem_expr);
                else generate_expression(f, elem_expr);
            }
            fprintf(f, "))");
        } else {
            fprintf(f, "%s%s(", prefix, method);
            generate_expression(f, receiver);
            fprintf(f, ", ");
            if (elem_expr) {
                if (strcmp(elem, "string") == 0) emit_string_expr(f, elem_expr);
                else generate_expression(f, elem_expr);
            }
            fprintf(f, ")");
        }
        return;
    }
    // Generic element types: pass the element by address.
    fprintf(f, "({ __typeof__((");
    generate_expression(f, receiver);
    fprintf(f, ")->items[0]) _ae = (");
    if (elem_expr) generate_expression(f, elem_expr);
    fprintf(f, "); ");
    if (strcmp(method, "append") == 0) {
        generate_expression(f, receiver);
        fprintf(f, " = come_array_append_g(");
        generate_expression(f, receiver);
        fprintf(f, ", sizeof _ae, &_ae); })");
    } else if (strcmp(method, "insert") == 0) {
        generate_expression(f, receiver);
        fprintf(f, " = come_array_insert_g(");
        generate_expression(f, receiver);
        fprintf(f, ", sizeof _ae, ");
        if (a1) generate_expression(f, a1);
        fprintf(f, ", &_ae); })");
    } else if (strcmp(method, "set") == 0) {
        fprintf(f, "come_array_set_g(");
        generate_expression(f, receiver);
        fprintf(f, ", sizeof _ae, &_ae); })");
    } else {   // contains / index
        fprintf(f, "come_array_%s_g(", method);
        generate_expression(f, receiver);
        fprintf(f, ", sizeof _ae, &_ae); })");
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
    fprintf(f, "(");
    generate_expression(f, e);
    fprintf(f, " ? ");
    generate_expression(f, e);
    fprintf(f, "->data : \"\")");
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

// Emit one map store statement (ends with a newline). Int values are
// boxed in a cell owned by the current context; string values are
// stored as the string object itself.
static void emit_map_put(FILE* f, const char* map_var, const char* vtype, ASTNode* key_expr, ASTNode* val_expr) {
    if (strcmp(vtype, "int") == 0) {
        fprintf(f, "{ int* _vp = (int*)mem_talloc_alloc(COME_CTX, sizeof(int)); *_vp = (");
        generate_expression(f, val_expr);
        fprintf(f, "); come_map_put(&%s, ", map_var);
        emit_string_expr(f, key_expr);
        fprintf(f, ", _vp); }\n");
    } else {
        fprintf(f, "come_map_put(&%s, ", map_var);
        emit_string_expr(f, key_expr);
        fprintf(f, ", ");
        emit_string_expr(f, val_expr);
        fprintf(f, ");\n");
    }
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
        if (strcmp(node->text, "null") == 0) fprintf(f, "NULL");
        else if (expr_is_fn_value(node)) {
            // A named function used as a value: its code reference
            // (spec 8.3 - legal as a call argument; the store and
            // call checks reject other positions). Extern "C"
            // functions keep their raw C name (COIL-111).
            if (is_extern_fn(node->text)) fprintf(f, "%s", node->text);
            else fprintf(f, "come_%s__%s", current_module, node->text);
        }
        else fprintf(f, "%s", node->text);
    } else if (node->type == AST_UNARY_OP) {
        fprintf(f, "(%s", node->text);
        generate_expression(f, node->children[0]);
        fprintf(f, ")");
    } else if (node->type == AST_ARRAY_ACCESS) {
        ASTNode* base = node->children[0];
        char mkt[160], mvt[160];
        const char* bt = (base->type == AST_IDENTIFIER) ? get_local_variable_type(base->text) : NULL;
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
            fprintf(f, "({ void* _p = come_map_get_e(%s, ", base->text);
            emit_string_expr(f, node->children[1]);
            if (strcmp(mvt, "int") == 0) {
                fprintf(f, "); _p ? *(int*)_p : 0; })");
            } else {
                fprintf(f, "); _p ? (come_string_t*)_p : come_string_new(COME_CTX, \"\"); })");
            }
        } else {
            // COME_ARR_GET(arr, index)
            fprintf(f, "COME_ARR_GET(");
            generate_expression(f, node->children[0]);
            fprintf(f, ", ");
            generate_expression(f, node->children[1]);
            fprintf(f, ")");
        }
    } else if (node->type == AST_ASSIGN) {
        check_assign_store(node); // COIL-107
        if (node->children[0]->type == AST_IDENTIFIER && node->children[1]->type == AST_IDENTIFIER) {
            const char* lt = get_local_variable_type(node->children[0]->text);
            const char* rt = get_local_variable_type(node->children[1]->text);
            if (lt && rt) check_array_transfer("be assigned to", node->children[0]->text, lt, node->children[1]->text, rt);
        }
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
             fprintf(stderr, "DEBUG: Member access on pointer: '%s'\n", node->children[0]->text);
             fprintf(f, ")->%s", node->text);
        } else {
             fprintf(f, ").%s", node->text);
        }
    } else if (node->type == AST_METHOD_CALL) {
        char* method = node->text;
        char c_func[16384];
        int skip_receiver = 0;
        ASTNode* receiver = node->children[0];
        
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
            if (strcmp(receiver->text, "ERR")==0 && strcmp(method, "fatal")==0) {
                 fprintf(f, "come_ERR_fatal(");
                 if (node->child_count > 1) emit_string_expr(f, node->children[1]);
                 else fprintf(f, "NULL");
                 fprintf(f, ")");
                 return;
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
                     snprintf(recv_c, sizeof(recv_c), "std_%s", receiver->text);
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
                         fprintf(f, "(");
                         generate_expression(f, a);
                         fprintf(f, " ? ");
                         generate_expression(f, a);
                         fprintf(f, "->data : \"\")");
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
                 int arg_count = node->child_count;
                 
                 // Pre-scan for format string modification
                 if (arg_count > 1 && node->children[1]->type == AST_STRING_LITERAL) {
                     bool_args = calloc(arg_count + 1, sizeof(int)); // +1 safety
                     char* raw_fmt = node->children[1]->text; 
                     fmt_modified = calloc(strlen(raw_fmt) * 2 + 100, 1);
                     
                     char* src = raw_fmt;
                     char* dst = fmt_modified;
                     int current_arg_idx = 2; // Arrrgs buffer index (2, 3...)
                     
                     while (*src) {
                         if (*src == '%' && *(src+1) != '%') {
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
                             while (*src && strchr("hljzL", *src)) *dst++ = *src++;
                             
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
                                 if (*src) *dst++ = *src++;
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
                          if (type && (strcmp(type, "string") == 0 || strcmp(type, "come_string_t*") == 0)) {
                              is_str = 1;
                          }
                      } else if (arg->type == AST_METHOD_CALL || arg->type == AST_CALL) {
                          // Check methods that return strings
                           // Check methods/functions that return strings (NOT len/size/count which return uint)
                           const char* name = arg->text;
                           // type() returns a string on maps,
                           // arrays and strings alike (COIL-103/116/119)
                           if (strcmp(name, "type") == 0 && arg->child_count > 0 &&
                               arg->children[0]->type == AST_IDENTIFIER) {
                               const char* rt = get_local_variable_type(arg->children[0]->text);
                               if (rt && (strncmp(rt, "map{", 4) == 0 || strchr(rt, '[') != NULL ||
                                          strcmp(rt, "string") == 0)) {
                                   is_str = 1;
                               }
                           }
                           
                           if (strcmp(name, "upper") == 0 || strcmp(name, "lower") == 0 || 
                               strcmp(name, "repeat") == 0 || strcmp(name, "replace") == 0 || 
                               strcmp(name, "trim") == 0 || strcmp(name, "ltrim") == 0 || 
                               strcmp(name, "rtrim") == 0 || strcmp(name, "substr") == 0 || 
                               strcmp(name, "join") == 0 || strcmp(name, "new") == 0 || 
                               strcmp(name, "str") == 0 || strcmp(name, "gets") == 0) {
                               is_str = 1;
                           }
                     } else if (arg->type == AST_ARRAY_ACCESS) {
                         // String element access: string[] element or a
                         // map{string, string} read (COIL-102). The old
                         // hardcoded name list stays as a fallback.
                         ASTNode* arr = arg->children[0];
                         if (arr->type == AST_IDENTIFIER) {
                              const char* at = get_local_variable_type(arr->text);
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
                 const char* type = get_local_variable_type(receiver->text);
                 if (type && (strcmp(type, "map") == 0 || strcmp(type, "come_map_t*") == 0)) {
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
                     generate_expression(f, node->children[i]);
                 }
                 fprintf(f, ")");
                 return;
             }
        }
        // Typed map methods (COIL-103): the Come_map.md method table.
        // Must precede the string branch, which also claims length/chown.
        else if (is_typed_map_method(receiver, method)) {
             const char* rtype = get_local_variable_type(receiver->text);
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
                 fprintf(f, "come_map_remove_e(");
                 generate_expression(f, receiver);
                 fprintf(f, ", ");
                 if (node->child_count > 1) emit_string_expr(f, node->children[1]);
                 fprintf(f, ")");
             } else if (strcmp(method, "clear") == 0) {
                 fprintf(f, "come_map_clear(");
                 generate_expression(f, receiver);
                 fprintf(f, ")");
             } else if (strcmp(method, "keys") == 0) {
                 fprintf(f, "come_map_keys(COME_CTX, ");
                 generate_expression(f, receiver);
                 fprintf(f, ")");
             } else if (strcmp(method, "type") == 0) {
                 fprintf(f, "come_string_new(COME_CTX, \"map\")");
             } else if (strcmp(method, "owner") == 0) {
                 fprintf(f, "COME_CTX");
             } else if (strcmp(method, "chown") == 0) {
                 fprintf(f, "mem_talloc_steal(");
                 if (node->child_count > 1) generate_expression(f, node->children[1]);
                 fprintf(f, ", ");
                 generate_expression(f, receiver);
                 fprintf(f, ")");
             } else if (strcmp(method, "dup") == 0) {
                 // dup(0) shares the map; dup()/dup(n) copies it
                 if (node->child_count > 1 && node->children[1]->type == AST_NUMBER &&
                     atoi(node->children[1]->text) == 0) {
                     generate_expression(f, receiver);
                 } else {
                     fprintf(f, "come_map_copy(COME_CTX, ");
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
            
            // Check if receiver is a map for len() - maps also have len()
            if (strcmp(method, "len") == 0 && receiver->type == AST_IDENTIFIER) {
                const char* type = get_local_variable_type(receiver->text);
                if (type && (strcmp(type, "map") == 0 || strcmp(type, "come_map_t*") == 0)) {
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
                else if (receiver->type == AST_METHOD_CALL) {
                    // A call that returns a plain string: string
                    // methods and the FILE string producers
                    // (gets/fname, COIL-114).
                    const char* mn = receiver->text;
                    if (strcmp(mn, "upper") == 0 || strcmp(mn, "lower") == 0 ||
                        strcmp(mn, "repeat") == 0 || strcmp(mn, "replace") == 0 ||
                        strcmp(mn, "trim") == 0 || strcmp(mn, "ltrim") == 0 ||
                        strcmp(mn, "rtrim") == 0 || strcmp(mn, "substr") == 0 ||
                        strcmp(mn, "join") == 0 || strcmp(mn, "gets") == 0 ||
                        strcmp(mn, "fname") == 0 || strcmp(mn, "str") == 0) {
                        plain_string = 1;
                    }
                }
                strcpy(c_func, plain_string ? "come_string_len" : "come_string_list_len");
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
             if (strcmp(method, "free") == 0) strcpy(c_func, "come_free");
             else if (strcmp(method, "bytesize") == 0) strcpy(c_func, "come_array_bytesize");
             else if (strcmp(method, "slice") == 0) strcpy(c_func, "come_array_slice");
             else snprintf(c_func, sizeof(c_func), "come_array_%s", method);
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
             // Try to resolve receiver variable type
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
             
             // std module functions take string objects (COIL-114):
             // materialize literals for them, as for ERR.fatal.
             if (skip_receiver && receiver->type == AST_IDENTIFIER &&
                 strcmp(receiver->text, "std") == 0 && strcmp(c_func, "printf") != 0 &&
                 arg->type == AST_STRING_LITERAL) {
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
        
        fprintf(f, "%s(", mangled_name);
        {
            // A string literal is a char* in C; where the callee's
            // parameter is a string it must be materialized as a
            // come_string_t first - passing the raw literal made
            // the callee read string headers out of rodata and
            // crash the caller (COIL-133).
            char cparts[32][160];
            int cnp = call_param_types(node, cparts, 32);
            for (int i = 0; i < node->child_count; i++) {
                if (i > 0) fprintf(f, ", ");
                if (i < cnp && strcmp(cparts[i], "string") == 0 &&
                    node->children[i]->type == AST_STRING_LITERAL)
                    emit_string_expr(f, node->children[i]);
                else
                    generate_expression(f, node->children[i]);
            }
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
                // Check if either operand is a string
                int left_is_str = 0, right_is_str = 0;
                
                if (left->type == AST_IDENTIFIER) {
                    const char* type = get_local_variable_type(left->text);
                    if (type && (strcmp(type, "string") == 0 || strcmp(type, "come_string_t*") == 0)) {
                        left_is_str = 1;
                    }
                }
                
                if (right->type == AST_STRING_LITERAL || right->type == AST_IDENTIFIER) {
                    if (right->type == AST_STRING_LITERAL) {
                        right_is_str = 1;
                    } else {
                        const char* type = get_local_variable_type(right->text);
                        if (type && (strcmp(type, "string") == 0 || strcmp(type, "come_string_t*") == 0)) {
                            right_is_str = 1;
                        }
                    }
                }
                
                is_string_cmp = (left_is_str || right_is_str);
            }
        }
        
        if (is_string_cmp) {
            // Generate strcmp() call
            fprintf(f, "(come_string_cmp(");
            generate_expression(f, node->children[0]);
            fprintf(f, ", come_string_new(NULL, ");
            generate_expression(f, node->children[1]);
            fprintf(f, "), 0) %s 0)", is_eq ? "==" : "!=");
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
        if (node->is_extern) return; // COIL-111: prototype only (forward pass)
        g_in_coroutine = node->is_coroutine ? 1 : 0; // COIL-122
        emit_line_directive(f, node);

        reset_local_variables();
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
        int body_idx = node->child_count - 1;
        
        if (ret_type->text[0] == '(') {
            strcpy(current_function_return_type, "void");
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
        if (strcmp(node->text, "init") == 0 || strcmp(node->text, "module_init") == 0) {
             snprintf(func_name, sizeof(func_name), "come_%s__init_local", current_module);
        } else if (strcmp(node->text, "exit") == 0 || strcmp(node->text, "module_exit") == 0) {
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

        emit_indent(f, indent);

        // Return type
        // Handle "byte" etc alias?? no, just print text
        fprintf(f, "%s %s(", ret_type->text, func_name);
        
        // Args
        int has_args = 0;
        
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
                   fprintf(f, "%s %s", type->text, arg->text);
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

            
            generate_block_children(f, body, indent + 4);
            if (is_main) {
                emit_indent(f, indent + 4);
                fprintf(f, "return 0;\n");
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
        emit_line_directive(f, node);  // Emit #line for variable declaration
        ASTNode* type_node = node->children[1];
        register_local_type(node->text, type_node);

        ASTNode* init_expr = node->children[0];

        if (init_expr && init_expr->type == AST_IDENTIFIER && type_is_array(type_node->text)) {
            const char* src_type = get_local_variable_type(init_expr->text);
            if (src_type) check_array_transfer("initialize", node->text, type_node->text, init_expr->text, src_type);
        }
        if (init_expr) check_numeric_store(type_node->text, init_expr, node->text); // COIL-107
        if (init_expr && expr_is_fn_value(init_expr))
            codegen_error("functions can only be passed as arguments (spec 8.3): '%s' cannot initialize '%s'", init_expr->text, node->text);
        
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
                if (init_expr && init_expr->type == AST_AGGREGATE_INIT) {
                    // String array literal (COIL-100): build a real list -
                    // header + one owned string per element.
                    int count = init_expr->child_count;
                    fprintf(f, "come_string_list_t* %s = (come_string_list_t*)mem_talloc_alloc(COME_CTX, sizeof(uint32_t)*2 + %d * sizeof(come_string_t*));\n", node->text, count);
                    emit_indent(f, indent);
                    fprintf(f, "%s->size = %d; %s->count = %d;\n", node->text, count, node->text, count);
                    for (int ei = 0; ei < count; ei++) {
                        ASTNode* elem = init_expr->children[ei];
                        emit_indent(f, indent);
                        fprintf(f, "%s->items[%d] = ", node->text, ei);
                        if (elem->type == AST_STRING_LITERAL) {
                            fprintf(f, "come_string_new(COME_CTX, ");
                            generate_expression(f, elem);
                            fprintf(f, ")");
                        } else {
                            generate_expression(f, elem);
                        }
                        fprintf(f, ";\n");
                    }
                } else {
                fprintf(f, "come_string_list_t* %s = ", node->text);
                if (init_expr->type == AST_STRING_LITERAL && strcmp(init_expr->text, "\"__ARGS__\"") == 0) {
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
                    fprintf(f, "come_map_t* %s = ", node->text);
                    generate_expression(f, init_expr);
                    fprintf(f, ";\n");
                } else if (init_expr && init_expr->type != AST_AGGREGATE_INIT &&
                           !(init_expr->type == AST_NUMBER && strcmp(init_expr->text, "0") == 0)) {
                    // Expression init (dup(), a call returning a map, ...)
                    fprintf(f, "come_map_t* %s = ", node->text);
                    generate_expression(f, init_expr);
                    fprintf(f, ";\n");
                } else {
                    fprintf(f, "come_map_t* %s = come_map_new(COME_CTX);\n", node->text);
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
                            emit_map_put(f, node->text, mvt, pair->children[0], pair->children[1]);
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
                     if (strcmp(type_node->text, "var")==0) {
                         fprintf(f, "int %s = ", node->text);
                     } else {
                         fprintf(f, "%s %s = ", type_node->text, node->text);
                     }
                     
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
                              generate_expression(f, init_expr);
                          }
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
            // A bare block is its own C scope: without braces,
            // two sibling blocks could not reuse a name.
            emit_indent(f, indent);
            fprintf(f, "{\n");
            generate_block_children(f, node, indent + 1);
            emit_indent(f, indent);
            fprintf(f, "}\n");
            break;
        }

        case AST_SPAWN: {
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
            fprintf(f, "come_coro_t* co = come_coro_create(come_%s__%s__entry, &come_%s__ctx);\n",
                    current_module, node->text, current_module);
            if (want > 0) {
                emit_indent(f, indent + 4);
                fprintf(f, "come_%s__%s__args_t* a = (come_%s__%s__args_t*)come_coro_args(co, sizeof(come_%s__%s__args_t));\n",
                        current_module, node->text, current_module, node->text,
                        current_module, node->text);
                for (int i = 0; i < node->child_count && i < want; i++) {
                    ASTNode* param = coro->children[1 + i];
                    emit_indent(f, indent + 4);
                    fprintf(f, "a->%s = ", param->text);
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
                    // moves the buffer under the coroutine.
                    if (is_buffer_type(param->children[1]->text)) {
                        emit_indent(f, indent + 4);
                        fprintf(f, "mem_talloc_steal(a, a->%s);\n", param->text);
                    }
                }
            }
            emit_indent(f, indent + 4);
            fprintf(f, "come_coro_start(co);\n");
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

        case AST_RETURN: {
            emit_line_directive(f, node);
            emit_indent(f, indent);
            if (g_in_coroutine && node->child_count > 0) {
                codegen_error("coroutines cannot return a value (spec 9.1); results travel through links");
            }
            if (node->child_count > 0 && expr_is_fn_value(node->children[0]))
                codegen_error("functions cannot be returned (spec 8.3): '%s'", node->children[0]->text);
            if (strcmp(current_function_return_type, "void") == 0) {
                 fprintf(f, "return;\n");
            } else {
                fprintf(f, "return");
                if (node->child_count > 0) {
                    fprintf(f, " ");
                    // Same materialization as call arguments: a
                    // string function must return a come_string_t,
                    // not a raw literal (COIL-133).
                    if (strcmp(current_function_return_type, "string") == 0 &&
                        node->children[0]->type == AST_STRING_LITERAL)
                        emit_string_expr(f, node->children[0]);
                    else
                        generate_expression(f, node->children[0]);
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
                         // Fix: if type ends in [], map to come_byte_array_t* for structs
                         // Or use pointer? byte* items.
                         // But we want to support size?
                         // "byte[]" usually come_byte_array_t* in my codegen.
                         fprintf(f, "come_%s_array_t* %s;\n", raw_type, field->text);
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
            emit_line_directive(f, node);  // Emit #line for assignment
            emit_indent(f, indent);
            check_assign_store(node); // COIL-107
            // Typed map index write: m[key] = value (COIL-102)
            int map_write = 0;
            if (strcmp(node->text, "=") == 0 && node->children[0]->type == AST_ARRAY_ACCESS) {
                ASTNode* acc = node->children[0];
                ASTNode* mbase = acc->children[0];
                if (mbase->type == AST_IDENTIFIER) {
                    const char* mbt = get_local_variable_type(mbase->text);
                    char mkt[160], mvt[160];
                    if (mbt && map_type_parts(mbt, mkt, sizeof(mkt), mvt, sizeof(mvt))) {
                        if (!map_combo_supported(mkt, mvt)) map_combo_error(mkt, mvt);
                        check_map_expr_type(mkt, acc->children[1], "key");
                        check_map_expr_type(mvt, node->children[1], "value");
                        emit_map_put(f, mbase->text, mvt, acc->children[1], node->children[1]);
                        map_write = 1;
                    }
                }
            }
            if (!map_write) {
            if (node->children[0]->type == AST_IDENTIFIER && node->children[1]->type == AST_IDENTIFIER) {
                const char* lt = get_local_variable_type(node->children[0]->text);
                const char* rt = get_local_variable_type(node->children[1]->text);
                if (lt && rt) check_array_transfer("be assigned to", node->children[0]->text, lt, node->children[1]->text, rt);
            }
            generate_expression(f, node->children[0]);
            fprintf(f, " %s ", node->text);
            generate_expression(f, node->children[1]);
            fprintf(f, ";\n");
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
            for (int i=1; i < node->child_count; i++) {
                generate_node(f, node->children[i], indent+4);
            }
            // Explicit break needed unless Fallthrough? 
            // COME spec: "Does NOT fall through by default".
            // So we add break unless last stmt is Fallthrough (not tracked yet)
            // For now, always break.
            emit_indent(f, indent+4);
            fprintf(f, "break;\n");
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
        default:
            break;
    }
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
        fprintf(f, "extern void come_ERR_fatal(come_string_t* msg);\n");
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
        fprintf(f, "bool come_std__FILE__open(come_std__FILE_t*, come_string_t*, come_string_t*);\n");
        fprintf(f, "void come_std__FILE__close(come_std__FILE_t*);\n");
        fprintf(f, "bool come_std__FILE__fdopen(come_std__FILE_t*, int, come_string_t*);\n");
        fprintf(f, "bool come_std__FILE__reopen(come_std__FILE_t*, come_string_t*, come_string_t*);\n");
        fprintf(f, "int come_std__FILE__fileno(come_std__FILE_t*);\n");
        fprintf(f, "int come_std__FILE__printf(come_std__FILE_t*, come_string_t*, ...);\n");
        fprintf(f, "int come_std__FILE__scanf(come_std__FILE_t*, come_string_t*, ...);\n");
        fprintf(f, "int come_std__FILE__vprintf(come_std__FILE_t*, come_string_t*, va_list);\n");
        fprintf(f, "int come_std__FILE__vscanf(come_std__FILE_t*, come_string_t*, va_list);\n");
        fprintf(f, "long come_std__FILE__read(come_std__FILE_t*, come_byte_array_t*, long);\n");
        fprintf(f, "long come_std__FILE__write(come_std__FILE_t*, come_byte_array_t*, long);\n");
        fprintf(f, "int32_t come_std__FILE__getc(come_std__FILE_t*);\n");
        fprintf(f, "void come_std__FILE__putc(come_std__FILE_t*, int32_t);\n");
        fprintf(f, "come_string_t* come_std__FILE__gets(come_std__FILE_t*);\n");
        fprintf(f, "long come_std__FILE__puts(come_std__FILE_t*, come_string_t*);\n");
        fprintf(f, "come_string_t* come_std__FILE__fname(come_std__FILE_t*);\n");
        fprintf(f, "void come_std__FILE__ungetc(come_std__FILE_t*, int32_t);\n");
        fprintf(f, "void come_std__FILE__seek(come_std__FILE_t*, long, int);\n");
        fprintf(f, "long come_std__FILE__tell(come_std__FILE_t*);\n");
        fprintf(f, "void come_std__FILE__rewind(come_std__FILE_t*);\n");
        fprintf(f, "bool come_std__FILE__isopen(come_std__FILE_t*);\n");
        fprintf(f, "bool come_std__FILE__eof(come_std__FILE_t*);\n");
        fprintf(f, "bool come_std__FILE__error(come_std__FILE_t*);\n");
        fprintf(f, "void come_std__FILE__flush(come_std__FILE_t*);\n");
        fprintf(f, "void come_std__FILE__clearerr(come_std__FILE_t*);\n");
        fprintf(f, "void come_std__FILE__setbuf(come_std__FILE_t*, come_byte_array_t*, long);\n");
        fprintf(f, "void come_std__FILE__setvbuf(come_std__FILE_t*, come_byte_array_t*, int, long);\n");
        fprintf(f, "void come_std__FILE__setlinebuf(come_std__FILE_t*);\n");
        fprintf(f, "bool come_std__remove(come_string_t*);\n");
        fprintf(f, "come_std__FILE_t come_std__tmpfile(void);\n");
        fprintf(f, "come_string_t* come_std__tmpname(void);\n");
        fprintf(f, "int come_std__rand(void);\n");
        fprintf(f, "void come_std__srand(unsigned int);\n");
        fprintf(f, "void come_std__qsort(come_byte_array_t*, int (*)(int8_t, int8_t));\n");
        fprintf(f, "void come_std__Proc__abort(void*);\n");
        fprintf(f, "void come_std__Proc__exit(void*, int);\n");
        fprintf(f, "void come_std__Proc__atexit(void*, void (*)(void));\n");
        fprintf(f, "come_string_t* come_std__Proc__getenv(void*, come_string_t*);\n");
        fprintf(f, "int come_std__Proc__system(void*, come_string_t*);\n");
    }
    // Macros for method dispatch
    fprintf(f, "#define COME_CTX come_%s__ctx\n\n", current_module);
    
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
        for (int i = 0; i < ast->child_count; i++) {
            ASTNode* child = ast->children[i];
            if (child && child->type == AST_FUNCTION && strcmp(child->text, "main") == 0) {
                has_main_fn = 1;
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
        
        fprintf(f, "    come_coro_init();\n");
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
        fprintf(f, "    come_run_exits();\n");
        fprintf(f, "    mem_talloc_free(COME_CTX);\n");
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
        fprintf(f, "extern void come_%s__init(void);\n", chain_imports[i]);
        fprintf(f, "extern void come_%s__exit(void);\n", chain_imports[i]);
    }

    if (!g_codegen_suppress_singletons) {
    fprintf(f, "void come_%s__init(void);\n", current_module);
    fprintf(f, "void come_%s__exit(void);\n", current_module);
    // Module Init
    fprintf(f, "void come_%s__init(void) {\n", current_module);
    fprintf(f, "    static bool initialized = false;\n");
    fprintf(f, "    if (initialized) return;\n");
    fprintf(f, "    initialized = true;\n");
    // Register this module's memory context with the coroutine
    // scheduler (COIL-120): context switches swap it per coroutine.
    fprintf(f, "    come_coro_register_ctx(&come_%s__ctx);\n", current_module);
    for (int i = 0; i < chain_count; i++) {
        fprintf(f, "    come_%s__init();\n", chain_imports[i]);
    }
    // Call the module's init hook if one is defined (init() or the
    // spec 3.5 spelling module_init(); mangled to __init_local).
    // For a directory module the driver reports the group fact.
    int has_local_init = g_codegen_group_has_init;
    if (has_local_init < 0) {
        has_local_init = 0;
        for (int i = 0; i < ast->child_count; i++) {
            if (ast->children[i]->type == AST_FUNCTION &&
                (strcmp(ast->children[i]->text, "init") == 0 ||
                 strcmp(ast->children[i]->text, "module_init") == 0)) {
                has_local_init = 1;
                break;
            }
        }
    }
    if (has_local_init) {
        fprintf(f, "    come_%s__init_local();\n", current_module);
    }
    // Register this module's exit: run_exits replays registrations
    // in reverse, giving exact reverse-of-init exit order (3.5).
    fprintf(f, "    come_register_exit(come_%s__exit);\n", current_module);
    fprintf(f, "}\n\n");

    // Module Exit
    fprintf(f, "void come_%s__exit(void) {\n", current_module);
    fprintf(f, "    static bool exited = false;\n");
    fprintf(f, "    if (exited) return;\n");
    fprintf(f, "    exited = true;\n");
    // Call the module's exit hook if defined (exit()/module_exit())
    int has_local_exit = g_codegen_group_has_exit;
    if (has_local_exit < 0) {
        has_local_exit = 0;
        for (int i = 0; i < ast->child_count; i++) {
            if (ast->children[i]->type == AST_FUNCTION &&
                (strcmp(ast->children[i]->text, "exit") == 0 ||
                 strcmp(ast->children[i]->text, "module_exit") == 0)) {
                has_local_exit = 1;
                break;
            }
        }
    }
    if (has_local_exit) {
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
                  } else if (strcmp(child->text, "init") == 0 || strcmp(child->text, "module_init") == 0) {
                      snprintf(func_name, sizeof(func_name), "come_%s__init_local", current_module);
                  } else if (strcmp(child->text, "exit") == 0 || strcmp(child->text, "module_exit") == 0) {
                      snprintf(func_name, sizeof(func_name), "come_%s__exit_local", current_module);
                  } else {
                      // Regular function
                      snprintf(func_name, sizeof(func_name), "come_%s__%s", current_module, child->text);
                  }


                  if (ret->text[0] == '(') {
                       fprintf(f, "void %s(", func_name);
                  } else {
                       if (strcmp(ret->text, "string") == 0) fprintf(f, "come_string_t* %s(", func_name);
                       else fprintf(f, "%s %s(", ret->text, func_name);
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
                     if (is_fn_type(type->text)) {
                            emit_fn_param(f, type->text, NULL); // COIL-109
                     // Array check
                     } else if (strstr(type->text, "[]")) {
                            char raw[64];
                            strncpy(raw, type->text, strlen(type->text)-2);
                            raw[strlen(type->text)-2] = 0;
                            
                            if (strcmp(raw, "int")==0) fprintf(f, "come_int_array_t*");
                            else if (strcmp(raw, "byte")==0) fprintf(f, "come_byte_array_t*");
                            else if (strcmp(raw, "string")==0) fprintf(f, "come_string_list_t*");
                            else fprintf(f, "come_array_t*");
                       } else if (type->text[0] == '(') {
                            fprintf(f, "void"); // Multi-return hack
                       } else {
                            if (strcmp(type->text, "string")==0) fprintf(f, "come_string_t*");
                            else fprintf(f, "%s", type->text);
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
        generate_program(f, ast);
    } else {
        generate_node(f, ast, 0);
    }

    fclose(f);
    return g_codegen_errors > 0 ? 1 : 0;
}
