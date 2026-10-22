#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "parser.h"
#include "codegen.h"
#include "ast.h"

int g_verbose = 0;  // defined in come_compiler.c for the real driver; the test provides its own

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
    return 0;
}
