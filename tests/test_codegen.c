#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "parser.h"
#include "codegen.h"
#include "ast.h"

int g_verbose = 0;  // defined in come_compiler.c for the real driver; the test provides its own

// Write src to path, parse and generate. gen_ok returns 1 when
// generation succeeds, gen_fail when it fails (parse or codegen).
static int gen_common(const char* path, const char* src) {
    FILE* cf = fopen(path, "w");
    if (!cf) return 0;
    fputs(src, cf);
    fclose(cf);
    ASTNode* ast = NULL;
    if (parse_file(path, &ast) != 0 || !ast) return 0;
    char out_c[512];
    snprintf(out_c, sizeof(out_c), "%s.c", path);
    int rc = generate_c_from_ast(ast, out_c, path, 0);
    ast_free(ast);
    return rc == 0;
}
static int gen_ok(const char* path, const char* src) {
    return gen_common(path, src);
}
static int gen_fail(const char* path, const char* src) {
    return !gen_common(path, src);
}

int main() {
    ASTNode* root = NULL;
    // parse_file currently returns a hardcoded AST, so the input file doesn't matter much
    // but we need it to succeed.
    if (parse_file("examples/hello.co", &root) != 0) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mParser failed\033[0m\n");
        return 1;
    }

    const char* out_file = "build/tests/test_output.c";
    if (generate_c_from_ast(root, out_file, "examples/hello.co", 0) != 0) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCodegen failed\033[0m\n");
        return 1;
    }

    // Verify output file exists and has some content
    FILE* f = fopen(out_file, "r");
    if (!f) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mOutput file not created\033[0m\n");
        return 1;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    if (size == 0) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mOutput file is empty\033[0m\n");
        return 1;
    }

    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCodegen test passed!\033[0m\n");
    ast_free(root);

    // COIL-101: fixed arrays stay fixed. Generation must FAIL for a
    // resize on a fixed array and for fixed->dynamic promotion, and
    // SUCCEED for ordinary fixed/dynamic use.
    struct { const char* path; const char* src; int should_fail; const char* what; } cases[3] = {
        { "build/tests/coil101_resize_fixed.co",
          "module main\nimport std\n\nint main() {\n    int fx1[3] = {1, 2, 3}\n    fx1.resize(9)\n    return 0\n}\n",
          1, "resize on a fixed array" },
        { "build/tests/coil101_promote.co",
          "module main\nimport std\n\nint main() {\n    int fx2[3] = {1, 2, 3}\n    int dn2[] = fx2\n    return 0\n}\n",
          1, "fixed array initializing a dynamic array" },
        { "build/tests/coil101_good.co",
          "module main\nimport std\n\nint main() {\n    int fx3[3] = {1, 2, 3}\n    int sm3[3]\n    sm3 = fx3\n    int dn3[] = {4, 5}\n    dn3.resize(9)\n    return fx3[0] + sm3[1] + dn3[2]\n}\n",
          0, "fixed and dynamic arrays used normally" },
    };
    for (int i = 0; i < 3; i++) {
        FILE* cf = fopen(cases[i].path, "w");
        if (!cf) { printf("Codegen test setup failed (cannot write temp file)\n"); return 1; }
        fputs(cases[i].src, cf);
        fclose(cf);
        ASTNode* ast = NULL;
        if (parse_file(cases[i].path, &ast) != 0 || !ast) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-101: parse failed for case: %s\033[0m\n", cases[i].what);
            return 1;
        }
        int rc = generate_c_from_ast(ast, "build/tests/coil101_out.c", cases[i].path, 0);
        if (cases[i].should_fail && rc == 0) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-101: generation succeeded but must fail: %s\033[0m\n", cases[i].what);
            return 1;
        }
        if (!cases[i].should_fail && rc != 0) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-101: generation failed but must succeed: %s\033[0m\n", cases[i].what);
            return 1;
        }
        ast_free(ast);
        remove(cases[i].path);
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-101 fixed-array test passed!\033[0m\n");

    // COIL-102: typed maps. Generation must SUCCEED for a well-typed
    // map{string, int} program and FAIL for a mixed-type literal and
    // for a wrong key type.
    struct { const char* path; const char* src; int should_fail; const char* what; } mcases[3] = {
        { "build/tests/coil102_good.co",
          "module main\nimport std\n\nint main() {\n    map{string, int} m = {\"a\": 1, \"b\": 2}\n    m[\"c\"] = 3\n    return m[\"a\"] + m[\"c\"]\n}\n",
          0, "well-typed map{string, int} program" },
        { "build/tests/coil102_mixed.co",
          "module main\nimport std\n\nint main() {\n    map{string, int} m = {\"a\": 1, \"b\": \"two\"}\n    return 0\n}\n",
          1, "mixed-type map literal" },
        { "build/tests/coil102_badkey.co",
          "module main\nimport std\n\nint main() {\n    map{string, int} m = {7: 1}\n    return 0\n}\n",
          1, "int key in a string-keyed map literal" },
    };
    for (int i = 0; i < 3; i++) {
        FILE* cf = fopen(mcases[i].path, "w");
        if (!cf) { printf("Codegen test setup failed (cannot write temp file)\n"); return 1; }
        fputs(mcases[i].src, cf);
        fclose(cf);
        ASTNode* ast = NULL;
        if (parse_file(mcases[i].path, &ast) != 0 || !ast) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-102: parse failed for case: %s\033[0m\n", mcases[i].what);
            return 1;
        }
        int rc = generate_c_from_ast(ast, "build/tests/coil102_out.c", mcases[i].path, 0);
        if (mcases[i].should_fail && rc == 0) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-102: generation succeeded but must fail: %s\033[0m\n", mcases[i].what);
            return 1;
        }
        if (!mcases[i].should_fail && rc != 0) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-102: generation failed but must succeed: %s\033[0m\n", mcases[i].what);
            return 1;
        }
        ast_free(ast);
        remove(mcases[i].path);
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-102 typed-map test passed!\033[0m\n");

    // COIL-103: the method table and ERR-on-missing must reach the
    // runtime helpers in the generated C.
    const char* p103 = "build/tests/coil103_methods.co";
    FILE* f3 = fopen(p103, "w");
    if (!f3) { printf("Codegen test setup failed (cannot write temp file)\n"); return 1; }
    fputs("module main\nimport std\n\nint main() {\n"
          "    map{string, int} m = {\"a\": 1}\n"
          "    int miss = m[\"ghost\"]\n"
          "    int n = m.length()\n"
          "    bool h = m.has(\"a\")\n"
          "    string ks[] = m.keys()\n"
          "    m.delete(\"a\")\n"
          "    map{string, int} c = m.dup()\n"
          "    m.clear()\n"
          "    return miss + n\n}\n", f3);
    fclose(f3);
    ASTNode* ast3 = NULL;
    if (parse_file(p103, &ast3) != 0 || !ast3) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-103: parse failed\033[0m\n");
        return 1;
    }
    const char* out103 = "build/tests/coil103_out.c";
    if (generate_c_from_ast(ast3, out103, p103, 0) != 0) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-103: generation failed\033[0m\n");
        return 1;
    }
    ast_free(ast3);
    remove(p103);
    FILE* of = fopen(out103, "r");
    if (!of) { printf("COIL-103: output file missing\n"); return 1; }
    static char buf[262144];
    size_t blen = fread(buf, 1, sizeof(buf) - 1, of);
    fclose(of);
    buf[blen] = 0;
    const char* need[4] = { "come_map_get_e", "come_map_keys", "come_map_remove_e", "come_map_copy" };
    for (int i = 0; i < 4; i++) {
        if (!strstr(buf, need[i])) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-103: generated C is missing %s\033[0m\n", need[i]);
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-103 map methods test passed!\033[0m\n");

    // COIL-104: struct layout modifiers must reach the generated C
    // as GCC attributes; a default struct must carry none. (Runtime
    // layout vs the C ABI reference is covered in src/core/t/06-layout.co.)
    const char* p104 = "build/tests/coil104_layout.co";
    FILE* f4 = fopen(p104, "w");
    if (!f4) { printf("Codegen test setup failed (cannot write temp file)\n"); return 1; }
    fputs("module main\nimport std\n\n"
          "struct Plain {\n    byte a\n    int b\n}\n\n"
          "packed struct Packed {\n    byte a\n    int b\n}\n\n"
          "align(16) struct Aligned {\n    int v\n}\n\n"
          "int main() {\n    return 0\n}\n", f4);
    fclose(f4);
    ASTNode* ast4 = NULL;
    if (parse_file(p104, &ast4) != 0 || !ast4) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-104: parse failed\033[0m\n");
        return 1;
    }
    const char* out104 = "build/tests/coil104_out.c";
    if (generate_c_from_ast(ast4, out104, p104, 0) != 0) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-104: generation failed\033[0m\n");
        return 1;
    }
    ast_free(ast4);
    remove(p104);
    FILE* of4 = fopen(out104, "r");
    if (!of4) { printf("COIL-104: output file missing\n"); return 1; }
    static char buf4[262144];
    size_t blen4 = fread(buf4, 1, sizeof(buf4) - 1, of4);
    fclose(of4);
    buf4[blen4] = 0;
    if (!strstr(buf4, "struct __attribute__((packed)) Packed {")) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-104: packed attribute missing in generated C\033[0m\n");
        return 1;
    }
    if (!strstr(buf4, "struct __attribute__((aligned(16))) Aligned {")) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-104: aligned(16) attribute missing in generated C\033[0m\n");
        return 1;
    }
    if (!strstr(buf4, "struct Plain {")) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-104: default struct must have no attributes\033[0m\n");
        return 1;
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-104 struct layout test passed!\033[0m\n");

    // COIL-111: extern "C" is rejected in a user module (the
    // default) and generates a raw C prototype + unmangled calls
    // in a system module.
    {
        const char* src111 =
            "module sysmod\nimport std\n\n"
            "extern \"C\" int abs(int x)\n\n"
            "int magnitude(int x) {\n    return abs(x)\n}\n";
        if (!gen_fail("build/tests/coil111_user.co", src111)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-111: extern in a user module must be a compile error\033[0m\n");
            return 1;
        }
        g_codegen_system_module = 1;
        int ok111 = gen_ok("build/tests/coil111_sys.co", src111);
        g_codegen_system_module = 0;
        if (!ok111) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-111: extern in a system module must generate\033[0m\n");
            return 1;
        }
        static char buf111[262144];
        FILE* of111 = fopen("build/tests/coil111_sys.co.c", "r");
        size_t bl111 = fread(buf111, 1, sizeof(buf111) - 1, of111);
        fclose(of111);
        buf111[bl111] = 0;
        if (!strstr(buf111, "int abs(int") || !strstr(buf111, "return abs(x);") ||
            strstr(buf111, "come_sysmod__abs")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-111: raw prototype / unmangled call missing in generated C\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-111 extern test passed!\033[0m\n");

    // COIL-133: string literals in argument and return positions
    // must be materialized as come_string_t - passing the raw C
    // literal made callees read string headers out of rodata and
    // crash the caller.
    {
        const char* src133 =
            "module main\nimport std\n\n"
            "string shout(string s) {\n    return s.upper()\n}\n"
            "string greet() {\n    return \"hi\"\n}\n"
            "int main() {\n    string b = shout(\"abc\")\n    return b.length()\n}\n";
        if (!gen_ok("build/tests/coil133.co", src133)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-133: derived-return program must generate\033[0m\n");
            return 1;
        }
        static char buf133[262144];
        FILE* of133 = fopen("build/tests/coil133.co.c", "r");
        size_t bl133 = fread(buf133, 1, sizeof(buf133) - 1, of133);
        fclose(of133);
        buf133[bl133] = 0;
        if (!strstr(buf133, "come_main__shout(come_string_new(COME_CTX, \"abc\"))") ||
            !strstr(buf133, "return come_string_new(COME_CTX, \"hi\");")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-133: literal argument / return not materialized in generated C\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-133 derived string return test passed!\033[0m\n");

    // COIL-110: character-first strings (spec 6.5). A plain
    // string's .length() must generate the character-counting
    // come_string_len, s[i] the wchar accessor, and mutation
    // attempts must be compile errors.
    {
        const char* ok110 =
            "module main\nimport std\n\nint main() {\n"
            "    string s = \"abc\"\n"
            "    int n = s.length()\n"
            "    wchar c = s[1]\n"
            "    return n + c\n}\n";
        if (!gen_ok("build/tests/coil110_ok.co", ok110)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-110: string length/index program must generate\033[0m\n");
            return 1;
        }
        static char buf110[262144];
        FILE* of110 = fopen("build/tests/coil110_ok.co.c", "r");
        size_t bl110 = fread(buf110, 1, sizeof(buf110) - 1, of110);
        fclose(of110);
        buf110[bl110] = 0;
        if (!strstr(buf110, "come_string_len(") || !strstr(buf110, "come_string_wchar_at_e(")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-110: char-length / wchar-index calls missing in generated C\033[0m\n");
            return 1;
        }
        const char* bad110a =
            "module main\nimport std\n\nint main() {\n"
            "    string s = \"abc\"\n    s[0] = 'x'\n    return 0\n}\n";
        if (!gen_fail("build/tests/coil110_a.co", bad110a)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-110: s[i] = ... must be a compile error\033[0m\n");
            return 1;
        }
        const char* bad110b =
            "module main\nimport std\n\nint main() {\n"
            "    string s = \"abc\"\n    s.resize(2)\n    return 0\n}\n";
        if (!gen_fail("build/tests/coil110_b.co", bad110b)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-110: string resize() must be a compile error\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-110 string conformance test passed!\033[0m\n");

    // COIL-109: functions as arguments (spec 8.3). The declarator
    // parameter must generate a C function pointer, the call must
    // pass the mangled function name, and signature mismatches,
    // non-function arguments and function values in stores must
    // all be compile errors.
    {
        const char* ok109 =
            "module main\nimport std\n\n"
            "int ascending(int a, int b) {\n    return a - b\n}\n"
            "void sort(int data[], int compare(int a, int b)) {\n}\n"
            "int main() {\n    int nums[] = {1}\n    sort(nums, ascending)\n    return 0\n}\n";
        if (!gen_ok("build/tests/coil109_ok.co", ok109)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-109: the spec sort example must generate\033[0m\n");
            return 1;
        }
        static char buf109[262144];
        FILE* of109 = fopen("build/tests/coil109_ok.co.c", "r");
        size_t bl109 = fread(buf109, 1, sizeof(buf109) - 1, of109);
        fclose(of109);
        buf109[bl109] = 0;
        if (!strstr(buf109, "int (*compare)(int, int)") ||
            !strstr(buf109, "come_main__ascending")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-109: declarator / function reference missing in generated C\033[0m\n");
            return 1;
        }
        const char* bad109a =
            "module main\nimport std\n\n"
            "int wrong(string a, string b) {\n    return 0\n}\n"
            "void sort(int data[], int compare(int a, int b)) {\n}\n"
            "int main() {\n    int nums[] = {1}\n    sort(nums, wrong)\n    return 0\n}\n";
        if (!gen_fail("build/tests/coil109_a.co", bad109a)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-109: mismatched signature must be a compile error\033[0m\n");
            return 1;
        }
        const char* bad109b =
            "module main\nimport std\n\n"
            "int ascending(int a, int b) {\n    return 0\n}\n"
            "int main() {\n    int x = ascending\n    return x\n}\n";
        if (!gen_fail("build/tests/coil109_b.co", bad109b)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-109: storing a function in a variable must be a compile error\033[0m\n");
            return 1;
        }
        const char* bad109c =
            "module main\nimport std\n\n"
            "void sort(int data[], int compare(int a, int b)) {\n}\n"
            "int main() {\n    int nums[] = {1}\n    sort(nums, 42)\n    return 0\n}\n";
        if (!gen_fail("build/tests/coil109_c.co", bad109c)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-109: a non-function argument must be a compile error\033[0m\n");
            return 1;
        }
        const char* bad109d =
            "module main\nimport std\n\n"
            "int ascending(int a, int b) {\n    return 0\n}\n"
            "int take(int v) {\n    return v\n}\n"
            "int main() {\n    return take(ascending)\n}\n";
        if (!gen_fail("build/tests/coil109_d.co", bad109d)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-109: passing a function to a value parameter must be a compile error\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-109 function arguments test passed!\033[0m\n");

    // COIL-108: directory-module codegen. A non-primary file must
    // not define the module singletons; the primary's chain must
    // call the group hook even when its own file defines none; the
    // spec's module_init/module_exit spellings mangle to the hooks.
    {
        const char* p108 = "build/tests/coil108_secondary.co";
        FILE* sf = fopen(p108, "w");
        fputs("module greeter\nimport std\n\nvoid extra() {\n}\n", sf);
        fclose(sf);
        ASTNode* a108 = NULL;
        if (parse_file(p108, &a108) != 0 || !a108) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-108: parse failed\033[0m\n");
            return 1;
        }
        static char buf108[262144];
        g_codegen_suppress_singletons = 1;
        int rc108 = generate_c_from_ast(a108, "build/tests/coil108_secondary.c", p108, 0);
        g_codegen_suppress_singletons = 0;
        FILE* of108 = fopen("build/tests/coil108_secondary.c", "r");
        size_t bl108 = fread(buf108, 1, sizeof(buf108) - 1, of108);
        fclose(of108);
        buf108[bl108] = 0;
        if (rc108 != 0 || !strstr(buf108, "extern TALLOC_CTX* come_greeter__ctx;") ||
            strstr(buf108, "come_greeter__ctx = NULL") ||
            strstr(buf108, "void come_greeter__init(void) {")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-108: secondary file must not define singletons\033[0m\n");
            return 1;
        }
        g_codegen_group_has_init = 1;
        rc108 = generate_c_from_ast(a108, "build/tests/coil108_primary.c", p108, 0);
        g_codegen_group_has_init = -1;
        of108 = fopen("build/tests/coil108_primary.c", "r");
        bl108 = fread(buf108, 1, sizeof(buf108) - 1, of108);
        fclose(of108);
        buf108[bl108] = 0;
        if (rc108 != 0 || !strstr(buf108, "come_greeter__init_local();") ||
            !strstr(buf108, "come_register_exit(come_greeter__exit);")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-108: primary chain must call the group hook and register the exit\033[0m\n");
            return 1;
        }
        ast_free(a108);

        const char* p108h = "build/tests/coil108_hook.co";
        FILE* hf = fopen(p108h, "w");
        fputs("module hook\nimport std\n\nvoid module_init() {\n}\n\nvoid module_exit() {\n}\n", hf);
        fclose(hf);
        ASTNode* a108h = NULL;
        if (parse_file(p108h, &a108h) != 0 || !a108h ||
            generate_c_from_ast(a108h, "build/tests/coil108_hook.c", p108h, 0) != 0) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-108: hook module must generate\033[0m\n");
            return 1;
        }
        FILE* ofh = fopen("build/tests/coil108_hook.c", "r");
        size_t blh = fread(buf108, 1, sizeof(buf108) - 1, ofh);
        fclose(ofh);
        buf108[blh] = 0;
        if (!strstr(buf108, "come_hook__init_local") || !strstr(buf108, "come_hook__exit_local") ||
            !strstr(buf108, "come_hook__init_local();")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-108: module_init/module_exit must mangle to the local hooks\033[0m\n");
            return 1;
        }
        ast_free(a108h);
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-108 directory module test passed!\033[0m\n");

    // COIL-107: narrowing stores are compile errors; lossless
    // widening and explicit T(value) casts generate fine.
    const char* ok107 =
        "module main\nimport std\n\nint main() {\n"
        "    long big = 1000\n"
        "    int x = int(big)\n"
        "    int i = 42\n"
        "    long l = i\n"
        "    double d = i\n"
        "    float f = 2.5\n"
        "    double d2 = f\n"
        "    byte b = 100\n"
        "    byte letter = 'A'\n"
        "    ubyte u = 200\n"
        "    uint ui = 7\n"
        "    long l2 = ui\n"
        "    return x\n}\n";
    if (!gen_ok("build/tests/coil107_ok.co", ok107)) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-107: widening/cast/literal program must generate\033[0m\n");
        return 1;
    }
    const char* bad107a =
        "module main\nimport std\n\nint main() {\n"
        "    long big = 1000\n    int x = big\n    return x\n}\n";
    if (!gen_fail("build/tests/coil107_a.co", bad107a)) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-107: long -> int declaration must be a compile error\033[0m\n");
        return 1;
    }
    const char* bad107b =
        "module main\nimport std\n\nint main() {\n"
        "    double d = 1.5\n    int j = 0\n    j = d\n    return j\n}\n";
    if (!gen_fail("build/tests/coil107_b.co", bad107b)) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-107: double -> int assignment must be a compile error\033[0m\n");
        return 1;
    }
    const char* bad107c =
        "module main\nimport std\n\nint main() {\n"
        "    long big = 5\n    double d = big\n    return 0\n}\n";
    if (!gen_fail("build/tests/coil107_c.co", bad107c)) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-107: long -> double must be a compile error (not exact past 2^53)\033[0m\n");
        return 1;
    }
    const char* bad107d =
        "module main\nimport std\n\nint main() {\n"
        "    byte b = 200\n    return 0\n}\n";
    if (!gen_fail("build/tests/coil107_d.co", bad107d)) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-107: literal 200 must not fit a (signed) byte\033[0m\n");
        return 1;
    }
    const char* bad107e =
        "module main\nimport std\n\nint main() {\n"
        "    int i = 42\n    byte b = i\n    return 0\n}\n";
    if (!gen_fail("build/tests/coil107_e.co", bad107e)) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-107: int variable -> byte must be a compile error\033[0m\n");
        return 1;
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-107 narrowing test passed!\033[0m\n");

    // COIL-105: ERR.set() must generate the predicate form and
    // ERR.fatal(msg) the runtime halt call.
    const char* p105 = "build/tests/coil105_err.co";
    FILE* f5 = fopen(p105, "w");
    if (!f5) { printf("Codegen test setup failed (cannot write temp file)\n"); return 1; }
    fputs("module main\nimport std\n\nint main() {\n"
          "    if (ERR.set()) {\n"
          "        ERR.fatal(\"cannot continue\")\n"
          "    }\n"
          "    return 0\n}\n", f5);
    fclose(f5);
    ASTNode* ast5 = NULL;
    if (parse_file(p105, &ast5) != 0 || !ast5) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-105: parse failed\033[0m\n");
        return 1;
    }
    const char* out105 = "build/tests/coil105_out.c";
    if (generate_c_from_ast(ast5, out105, p105, 0) != 0) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-105: generation failed\033[0m\n");
        return 1;
    }
    ast_free(ast5);
    remove(p105);
    FILE* of5 = fopen(out105, "r");
    if (!of5) { printf("COIL-105: output file missing\n"); return 1; }
    static char buf5[262144];
    size_t blen5 = fread(buf5, 1, sizeof(buf5) - 1, of5);
    fclose(of5);
    buf5[blen5] = 0;
    if (!strstr(buf5, "(come_ERR_no() != 0)")) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-105: ERR.set() predicate missing in generated C\033[0m\n");
        return 1;
    }
    if (!strstr(buf5, "come_ERR_fatal(come_string_new(")) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-105: ERR.fatal call missing in generated C\033[0m\n");
        return 1;
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-105 ERR test passed!\033[0m\n");

    // COIL-114: FILE is the std module's object type. Method
    // calls dispatch to come_std__FILE__*, a FILE variable is a
    // come_std__FILE_t, scope exit emits an auto-close, and an
    // unknown FILE method is a compile error.
    {
        const char* src114 =
            "module m114\nimport std\n\n"
            "int main() {\n"
            "    FILE f\n"
            "    if (f.open(\"x.txt\", \"w\")) {\n"
            "        f.puts(\"hi\")\n"
            "    }\n"
            "    return 0\n"
            "}\n";
        if (!gen_ok("build/tests/coil114.co", src114)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-114: FILE program must generate\033[0m\n");
            return 1;
        }
        FILE* of114 = fopen("build/tests/coil114.co.c", "r");
        if (!of114) { printf("COIL-114: output file missing\n"); return 1; }
        static char buf114[262144];
        size_t blen114 = fread(buf114, 1, sizeof(buf114) - 1, of114);
        fclose(of114);
        buf114[blen114] = 0;
        if (!strstr(buf114, "come_std__FILE_t f = {0};")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-114: FILE declaration must use the std runtime type\033[0m\n");
            return 1;
        }
        if (!strstr(buf114, "come_std__FILE__open(&f")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-114: FILE method must dispatch to the std runtime\033[0m\n");
            return 1;
        }
        if (!strstr(buf114, "come_std__FILE__close(&f);")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-114: scope exit must emit the FILE auto-close\033[0m\n");
            return 1;
        }
        const char* bad114 =
            "module m114b\nimport std\n\n"
            "int main() {\n"
            "    FILE f\n"
            "    f.frobnicate()\n"
            "    return 0\n"
            "}\n";
        if (!gen_fail("build/tests/coil114bad.co", bad114)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-114: unknown FILE method must be a compile error\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-114 FILE codegen test passed!\033[0m\n");

    // COIL-115: std.proc dispatches to the Proc runtime; atexit
    // receives the callback by its mangled module name; unknown
    // proc methods are compile errors.
    {
        const char* src115 =
            "module m115\nimport std\n\n"
            "void cleanup() {\n}\n"
            "int main() {\n"
            "    std.proc.atexit(cleanup)\n"
            "    string h = std.proc.getenv(\"HOME\")\n"
            "    return 0\n"
            "}\n";
        if (!gen_ok("build/tests/coil115.co", src115)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-115: std.proc program must generate\033[0m\n");
            return 1;
        }
        FILE* of115 = fopen("build/tests/coil115.co.c", "r");
        if (!of115) { printf("COIL-115: output file missing\n"); return 1; }
        static char buf115[262144];
        size_t blen115 = fread(buf115, 1, sizeof(buf115) - 1, of115);
        fclose(of115);
        buf115[blen115] = 0;
        if (!strstr(buf115, "come_std__Proc__atexit(NULL, come_m115__cleanup)")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-115: atexit must receive the mangled callback\033[0m\n");
            return 1;
        }
        if (!strstr(buf115, "come_std__Proc__getenv(NULL, come_string_new(")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-115: getenv must receive a string object\033[0m\n");
            return 1;
        }
        const char* bad115 =
            "module m115b\nimport std\n\n"
            "int main() {\n"
            "    std.proc.frobnicate()\n"
            "    return 0\n"
            "}\n";
        if (!gen_fail("build/tests/coil115bad.co", bad115)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-115: unknown std.proc method must be a compile error\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-115 proc codegen test passed!\033[0m\n");

    // COIL-116: array toolbox methods dispatch to the array
    // runtime; growing a fixed array is a compile error.
    {
        const char* src116 =
            "module m116\nimport std\n\n"
            "int main() {\n"
            "    int a[] = {3, 1, 2}\n"
            "    a.append(4)\n"
            "    bool has = a.contains(2)\n"
            "    return 0\n"
            "}\n";
        if (!gen_ok("build/tests/coil116.co", src116)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-116: array toolbox program must generate\033[0m\n");
            return 1;
        }
        FILE* of116 = fopen("build/tests/coil116.co.c", "r");
        if (!of116) { printf("COIL-116: output file missing\n"); return 1; }
        static char buf116[262144];
        size_t blen116 = fread(buf116, 1, sizeof(buf116) - 1, of116);
        fclose(of116);
        buf116[blen116] = 0;
        if (!strstr(buf116, "come_int_array_append")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-116: append must dispatch to the array runtime\033[0m\n");
            return 1;
        }
        if (!strstr(buf116, "come_int_array_contains")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-116: contains must dispatch to the array runtime\033[0m\n");
            return 1;
        }
        const char* bad116 =
            "module m116b\nimport std\n\n"
            "int main() {\n"
            "    int fixed[3] = {1, 2, 3}\n"
            "    fixed.append(4)\n"
            "    return 0\n"
            "}\n";
        if (!gen_fail("build/tests/coil116bad.co", bad116)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-116: appending to a fixed array must be a compile error\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-116 array codegen test passed!\033[0m\n");

    // COIL-117: string methods with C-string parameters emit the
    // argument's byte buffer (->data), not the string object;
    // sprintf is the string module's free function.
    {
        const char* src117 =
            "module m117\nimport std\nimport string\n\n"
            "int main() {\n"
            "    string csv = \"a,b\"\n"
            "    string sep = \",\"\n"
            "    string parts[] = csv.split(sep)\n"
            "    string out = sprintf(\"%d\", 3)\n"
            "    return 0\n"
            "}\n";
        if (!gen_ok("build/tests/coil117.co", src117)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-117: string toolbox program must generate\033[0m\n");
            return 1;
        }
        FILE* of117 = fopen("build/tests/coil117.co.c", "r");
        if (!of117) { printf("COIL-117: output file missing\n"); return 1; }
        static char buf117[262144];
        size_t blen117 = fread(buf117, 1, sizeof(buf117) - 1, of117);
        fclose(of117);
        buf117[blen117] = 0;
        if (!strstr(buf117, "come_string_split(csv, (sep ? sep->data : \"\"))")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-117: split must pass the separator's bytes, not the object\033[0m\n");
            return 1;
        }
        if (!strstr(buf117, "come_string_sprintf(COME_CTX,")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-117: sprintf must reach the string runtime\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-117 string codegen test passed!\033[0m\n");

    // COIL-119: a bare block after a method call is a statement,
    // not a trailing closure of that call; string dup reaches the
    // string runtime.
    {
        const char* src119 =
            "module m119\nimport std\n\n"
            "int main() {\n"
            "    string a = \"x\"\n"
            "    string b = a.dup()\n"
            "    std.out.printf(\"%s\\n\", b)\n"
            "    {\n"
            "        int inner = 1\n"
            "        std.out.printf(\"%d\\n\", inner)\n"
            "    }\n"
            "    return 0\n"
            "}\n";
        if (!gen_ok("build/tests/coil119.co", src119)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-119: bare-block program must generate\033[0m\n");
            return 1;
        }
        FILE* of119 = fopen("build/tests/coil119.co.c", "r");
        if (!of119) { printf("COIL-119: output file missing\n"); return 1; }
        static char buf119[262144];
        size_t blen119 = fread(buf119, 1, sizeof(buf119) - 1, of119);
        fclose(of119);
        buf119[blen119] = 0;
        if (!strstr(buf119, "come_string_dup(a)")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-119: string dup must reach the string runtime\033[0m\n");
            return 1;
        }
        if (strstr(buf119, "__cb")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-119: a bare block must not become a trailing closure\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-119 examples codegen test passed!\033[0m\n");

    // COIL-120: a coroutine declaration produces an args struct
    // and a scheduler entry; spawn builds the args block through
    // the runtime; yield reaches come_coro_yield. Spawning a
    // plain function, or with the wrong arity, is an error.
    {
        const char* src120 =
            "module m120\nimport std\n\n"
            "coroutine worker(string tag, int n) {\n"
            "    yield\n"
            "}\n"
            "int main() {\n"
            "    spawn worker(\"A\", 3)\n"
            "    yield\n"
            "    return 0\n"
            "}\n";
        if (!gen_ok("build/tests/coil120.co", src120)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-120: coroutine program must generate\033[0m\n");
            return 1;
        }
        FILE* of120 = fopen("build/tests/coil120.co.c", "r");
        if (!of120) { printf("COIL-120: output file missing\n"); return 1; }
        static char buf120[262144];
        size_t blen120 = fread(buf120, 1, sizeof(buf120) - 1, of120);
        fclose(of120);
        buf120[blen120] = 0;
        if (!strstr(buf120, "come_m120__worker__args_t")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-120: args struct missing\033[0m\n");
            return 1;
        }
        if (!strstr(buf120, "come_coro_create(come_m120__worker__entry")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-120: spawn must create via the scheduler entry\033[0m\n");
            return 1;
        }
        if (!strstr(buf120, "come_coro_yield()")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-120: yield must reach the runtime\033[0m\n");
            return 1;
        }
        const char* bad120a =
            "module m120b\nimport std\n\n"
            "int helper() { return 1 }\n"
            "int main() {\n"
            "    spawn helper()\n"
            "    return 0\n"
            "}\n";
        if (!gen_fail("build/tests/coil120bad.co", bad120a)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-120: spawning a plain function must be a compile error\033[0m\n");
            return 1;
        }
        const char* bad120b =
            "module m120c\nimport std\n\n"
            "coroutine w(int n) { yield }\n"
            "int main() {\n"
            "    spawn w(1, 2)\n"
            "    return 0\n"
            "}\n";
        if (!gen_fail("build/tests/coil120bad2.co", bad120b)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-120: spawn with wrong arity must be a compile error\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-120 coroutine codegen test passed!\033[0m\n");

    // COIL-121: spawn works from inside a coroutine too (nested
    // spawn), and every spawn site emits the full
    // create/args/start sequence.
    {
        const char* src121 =
            "module m121\nimport std\n\n"
            "coroutine leaf(int id) {\n"
            "    yield\n"
            "}\n"
            "coroutine parent(int id) {\n"
            "    spawn leaf(id)\n"
            "    yield\n"
            "}\n"
            "int main() {\n"
            "    spawn parent(1)\n"
            "    yield\n"
            "    return 0\n"
            "}\n";
        if (!gen_ok("build/tests/coil121.co", src121)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-121: nested spawn program must generate\033[0m\n");
            return 1;
        }
        FILE* of121 = fopen("build/tests/coil121.co.c", "r");
        if (!of121) { printf("COIL-121: output file missing\n"); return 1; }
        static char buf121[262144];
        size_t blen121 = fread(buf121, 1, sizeof(buf121) - 1, of121);
        fclose(of121);
        buf121[blen121] = 0;
        int creates = 0;
        const char* scan = buf121;
        while ((scan = strstr(scan, "come_coro_create(")) != NULL) { creates++; scan++; }
        if (creates != 2) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-121: expected 2 spawn sites, found %d\033[0m\n", creates);
            return 1;
        }
        if (!strstr(buf121, "come_coro_start(__come_co)")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-121: spawn must start the coroutine\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-121 spawn codegen test passed!\033[0m\n");

    // COIL-122: isolation violations are compile errors, and
    // spawn transfers buffer ownership (a steal into the args
    // block) while value arguments are plain copies.
    {
        const char* ok122 =
            "module m122\nimport std\n\n"
            "coroutine w(string s, int n) {\n"
            "    yield\n"
            "}\n"
            "int main() {\n"
            "    spawn w(\"x\", 1)\n"
            "    yield\n"
            "    return 0\n"
            "}\n";
        if (!gen_ok("build/tests/coil122.co", ok122)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-122: buffer spawn program must generate\033[0m\n");
            return 1;
        }
        FILE* of122 = fopen("build/tests/coil122.co.c", "r");
        if (!of122) { printf("COIL-122: output file missing\n"); return 1; }
        static char buf122[262144];
        size_t blen122 = fread(buf122, 1, sizeof(buf122) - 1, of122);
        fclose(of122);
        buf122[blen122] = 0;
        if (!strstr(buf122, "mem_talloc_steal(__come_a, __come_a->s)")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-122: string argument ownership must move at spawn\033[0m\n");
            return 1;
        }
        if (strstr(buf122, "mem_talloc_steal(__come_a, __come_a->n)")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-122: value argument must be copied, not stolen\033[0m\n");
            return 1;
        }
        const char* bad122a =
            "module m122b\nimport std\n"
            "int shared = 1\n"
            "coroutine w() {\n"
            "    shared = 2\n"
            "    yield\n"
            "}\n"
            "int main() { spawn w()\n yield\n return 0 }\n";
        if (!gen_fail("build/tests/coil122bad.co", bad122a)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-122: module variable write in a coroutine must be a compile error\033[0m\n");
            return 1;
        }
        const char* bad122b =
            "module m122c\nimport std\n"
            "int shared = 1\n"
            "coroutine w() {\n"
            "    int v = shared\n"
            "    yield\n"
            "}\n"
            "int main() { spawn w()\n yield\n return 0 }\n";
        if (!gen_fail("build/tests/coil122bad2.co", bad122b)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-122: module variable read in a coroutine must be a compile error\033[0m\n");
            return 1;
        }
        const char* bad122c =
            "module m122d\nimport std\n"
            "coroutine w() { yield }\n"
            "int main() { w()\n return 0 }\n";
        if (!gen_fail("build/tests/coil122bad3.co", bad122c)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-122: calling a coroutine must be a compile error\033[0m\n");
            return 1;
        }
        const char* bad122d =
            "module m122e\nimport std\n"
            "coroutine w() { return 5 }\n"
            "int main() { spawn w()\n yield\n return 0 }\n";
        if (!gen_fail("build/tests/coil122bad4.co", bad122d)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-122: returning a value from a coroutine must be a compile error\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-122 isolation codegen test passed!\033[0m\n");

    // COIL-123: (a, b) = colink() creates a channel; link
    // methods reach the channel runtime; a link spawn argument
    // is adopted by the coroutine. A bare colink() call is an
    // error - it makes two ends at once.
    {
        const char* src123 =
            "module m123\nimport std\n\n"
            "coroutine worker(link s) {\n"
            "    string req = s.read()\n"
            "    s.write(req)\n"
            "}\n"
            "int main() {\n"
            "    (a, b) = colink()\n"
            "    spawn worker(a)\n"
            "    b.write(\"ping\")\n"
            "    string r = b.read()\n"
            "    b.close()\n"
            "    return 0\n"
            "}\n";
        if (!gen_ok("build/tests/coil123.co", src123)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-123: link program must generate\033[0m\n");
            return 1;
        }
        FILE* of123 = fopen("build/tests/coil123.co.c", "r");
        if (!of123) { printf("COIL-123: output file missing\n"); return 1; }
        static char buf123[262144];
        size_t blen123 = fread(buf123, 1, sizeof(buf123) - 1, of123);
        fclose(of123);
        buf123[blen123] = 0;
        if (!strstr(buf123, "come_colink(&a, &b)")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-123: colink destructure must create the channel\033[0m\n");
            return 1;
        }
        if (!strstr(buf123, "come_link_read(") || !strstr(buf123, "come_link_write(") ||
            !strstr(buf123, "come_link_close(")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-123: link methods must reach the channel runtime\033[0m\n");
            return 1;
        }
        if (!strstr(buf123, "come_link_adopt(__come_a->s, __come_co)")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-123: a spawned coroutine must adopt its link end\033[0m\n");
            return 1;
        }
        const char* bad123 =
            "module m123b\nimport std\n\n"
            "int main() {\n"
            "    link a = colink()\n"
            "    return 0\n"
            "}\n";
        if (!gen_fail("build/tests/coil123bad.co", bad123)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-123: a bare colink() call must be a compile error\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-123 link codegen test passed!\033[0m\n");

    // COIL-124: wait(...) builds the end set and calls the
    // runtime; it needs at least one link, and its arguments
    // must be links.
    {
        const char* src124 =
            "module m124\nimport std\n\n"
            "int main() {\n"
            "    (a, b) = colink()\n"
            "    (c, d) = colink()\n"
            "    link ready = wait(b, d)\n"
            "    string v = ready.read()\n"
            "    return 0\n"
            "}\n";
        if (!gen_ok("build/tests/coil124.co", src124)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-124: wait program must generate\033[0m\n");
            return 1;
        }
        FILE* of124 = fopen("build/tests/coil124.co.c", "r");
        if (!of124) { printf("COIL-124: output file missing\n"); return 1; }
        static char buf124[262144];
        size_t blen124 = fread(buf124, 1, sizeof(buf124) - 1, of124);
        fclose(of124);
        buf124[blen124] = 0;
        if (!strstr(buf124, "come_link_wait(__wait_ends, 2)")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-124: wait must reach the runtime with its end set\033[0m\n");
            return 1;
        }
        const char* bad124a =
            "module m124b\nimport std\n\n"
            "int main() {\n"
            "    link ready = wait()\n"
            "    return 0\n"
            "}\n";
        if (!gen_fail("build/tests/coil124bad.co", bad124a)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-124: wait with no links must be a compile error\033[0m\n");
            return 1;
        }
        const char* bad124b =
            "module m124c\nimport std\n\n"
            "int main() {\n"
            "    int n = 5\n"
            "    link ready = wait(n)\n"
            "    return 0\n"
            "}\n";
        if (!gen_fail("build/tests/coil124bad2.co", bad124b)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-124: wait on a non-link must be a compile error\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-124 wait codegen test passed!\033[0m\n");

    // COIL-126: a first assignment to a fresh name declares
    // it with the inferred type (the spec's examples assign
    // reply = b.read() with no declaration). A value whose
    // type cannot be inferred is a compile error asking for
    // an explicit declaration.
    {
        const char* src126 =
            "module m126\nimport std\n\n"
            "coroutine echo(link s) {\n"
            "    msg = s.read()\n"
            "    s.write(msg)\n"
            "}\n\n"
            "int main() {\n"
            "    (a, b) = colink()\n"
            "    spawn echo(a)\n"
            "    b.write(\"x\")\n"
            "    reply = b.read()\n"
            "    n = 42\n"
            "    return 0\n"
            "}\n";
        if (!gen_ok("build/tests/coil126.co", src126)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-126: inferred-assignment program must generate\033[0m\n");
            return 1;
        }
        FILE* of126 = fopen("build/tests/coil126.co.c", "r");
        if (!of126) { printf("COIL-126: output file missing\n"); return 1; }
        static char buf126[262144];
        size_t blen126 = fread(buf126, 1, sizeof(buf126) - 1, of126);
        fclose(of126);
        buf126[blen126] = 0;
        if (!strstr(buf126, "come_string_t* reply =") || !strstr(buf126, "int n = 42")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-126: first assignments must emit inferred declarations\033[0m\n");
            return 1;
        }
        const char* bad126 =
            "module m126b\nimport std\n\n"
            "int compute() { return 7; }\n\n"
            "int main() {\n"
            "    x = compute()\n"
            "    return 0\n"
            "}\n";
        if (!gen_fail("build/tests/coil126bad.co", bad126)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-126: an uninferable first assignment must be a compile error\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-126 inferred-assignment codegen test passed!\033[0m\n");

    // COIL-127: spec-conformance fixes - void main (2.1),
    // tuple returns (8.2), the var type lock (10.1), and the
    // string/non-string assignment class check.
    {
        const char* src127 =
            "module m127\nimport std\n\n"
            "(int, string) add_n_compare(int a, int b) {\n"
            "    return (a + b), (a > b) ? \">\" : \"<=\"\n"
            "}\n\n"
            "void main() {\n"
            "    int sum\n"
            "    string cmp\n"
            "    (sum, cmp) = add_n_compare(5, 3)\n"
            "}\n";
        if (!gen_ok("build/tests/coil127.co", src127)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-127: tuple/void-main program must generate\033[0m\n");
            return 1;
        }
        FILE* of127 = fopen("build/tests/coil127.co.c", "r");
        if (!of127) { printf("COIL-127: output file missing\n"); return 1; }
        static char buf127[262144];
        size_t blen127 = fread(buf127, 1, sizeof(buf127) - 1, of127);
        fclose(of127);
        buf127[blen127] = 0;
        if (!strstr(buf127, "come_m127__add_n_compare_ret_t") ||
            !strstr(buf127, "void come_m127__main(void);") ||
            !strstr(buf127, "__tup")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-127: tuple typedef, void main prototype, or destructure temp missing\033[0m\n");
            return 1;
        }
        // var locks its type at the first assignment (10.1).
        const char* bad_var =
            "module m127b\nimport std\n\n"
            "int main() {\n"
            "    var x\n"
            "    x = 123\n"
            "    x = \"now a string\"\n"
            "    return 0\n"
            "}\n";
        if (!gen_fail("build/tests/coil127bad1.co", bad_var)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-127: changing a var's locked type must be a compile error\033[0m\n");
            return 1;
        }
        // A tuple function must return all its values (8.2).
        const char* bad_arity =
            "module m127c\nimport std\n\n"
            "(int, string) f(int a) {\n"
            "    return a\n"
            "}\n\n"
            "int main() { return 0; }\n";
        if (!gen_fail("build/tests/coil127bad2.co", bad_arity)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-127: a short tuple return must be a compile error\033[0m\n");
            return 1;
        }
        // Multiple values from a plain function (8.2).
        const char* bad_multi =
            "module m127d\nimport std\n\n"
            "int f(int a) {\n"
            "    return a, a\n"
            "}\n\n"
            "int main() { return 0; }\n";
        if (!gen_fail("build/tests/coil127bad3.co", bad_multi)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-127: multiple returns without a tuple type must be a compile error\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-127 spec-conformance codegen test passed!\033[0m\n");

    // COIL-128: the grammar's operator surface generates -
    // bitwise/shift levels parse (spec 14), every compound
    // assignment is accepted, and a prefix ++/-- used as a
    // statement is emitted (it used to be dropped silently).
    {
        const char* src128 =
            "module m128\nimport std\n\n"
            "int main() {\n"
            "    int x = 0x1F\n"
            "    int y = (x & 6) | (3 ^ 1)\n"
            "    int z = (y << 2) >> 1\n"
            "    z %= 7\n"
            "    ++z\n"
            "    --z\n"
            "    return z\n"
            "}\n";
        if (!gen_ok("build/tests/coil128.co", src128)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-128: operator-surface program must generate\033[0m\n");
            return 1;
        }
        FILE* of128 = fopen("build/tests/coil128.co.c", "r");
        if (!of128) { printf("COIL-128: output file missing\n"); return 1; }
        static char buf128[262144];
        size_t blen128 = fread(buf128, 1, sizeof(buf128) - 1, of128);
        fclose(of128);
        buf128[blen128] = 0;
        if (!strstr(buf128, "&") || !strstr(buf128, "<<") ||
            !strstr(buf128, "%=") || !strstr(buf128, "++") ||
            !strstr(buf128, "--")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-128: bitwise, shift, %=, or prefix ++/-- missing from generated C\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-128 grammar-surface codegen test passed!\033[0m\n");

    // COIL-129: the classic mistakes fail at the Come level
    // (the exact messages are pinned by run_errors_test.sh):
    // undefined variable, undefined function, wrong argument
    // count, assignment to a constant, break outside a loop.
    {
        const char* hdr129 = "module m129\nimport std\n\n";
        char src129[1024];
        snprintf(src129, sizeof(src129), "%sint main() {\n    return missing + 1\n}\n", hdr129);
        if (!gen_fail("build/tests/coil129a.co", src129)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-129: undefined variable must fail generation\033[0m\n");
            return 1;
        }
        snprintf(src129, sizeof(src129), "%sint main() {\n    return nosuchfn(3)\n}\n", hdr129);
        if (!gen_fail("build/tests/coil129b.co", src129)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-129: undefined function must fail generation\033[0m\n");
            return 1;
        }
        snprintf(src129, sizeof(src129), "%sint add(int a, int b) {\n    return a + b\n}\n\nint main() {\n    return add(1)\n}\n", hdr129);
        if (!gen_fail("build/tests/coil129c.co", src129)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-129: wrong argument count must fail generation\033[0m\n");
            return 1;
        }
        snprintf(src129, sizeof(src129), "%sconst K = 5\n\nint main() {\n    K = 6\n    return K\n}\n", hdr129);
        if (!gen_fail("build/tests/coil129d.co", src129)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-129: assigning to a constant must fail generation\033[0m\n");
            return 1;
        }
        snprintf(src129, sizeof(src129), "%sint main() {\n    break\n    return 0\n}\n", hdr129);
        if (!gen_fail("build/tests/coil129e.co", src129)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-129: break outside a loop must fail generation\033[0m\n");
            return 1;
        }
        // And the alias cast generates a C cast, not a call.
        const char* cast129 =
            "module m129f\nimport std\n\n"
            "alias tcpport_t = ushort\n\n"
            "int main() {\n"
            "    tcpport_t p = tcpport_t(8080)\n"
            "    return p\n"
            "}\n";
        if (!gen_ok("build/tests/coil129f.co", cast129)) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-129: alias cast must generate\033[0m\n");
            return 1;
        }
        FILE* of129 = fopen("build/tests/coil129f.co.c", "r");
        if (!of129) { printf("COIL-129: output file missing\n"); return 1; }
        static char buf129[262144];
        size_t blen129 = fread(buf129, 1, sizeof(buf129) - 1, of129);
        fclose(of129);
        buf129[blen129] = 0;
        if (!strstr(buf129, "(tcpport_t)") || strstr(buf129, "come_m129f__tcpport_t")) {
            printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-129: alias cast must emit (tcpport_t), not a mangled call\033[0m\n");
            return 1;
        }
    }
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-129 error-quality codegen test passed!\033[0m\n");
    return 0;
}
