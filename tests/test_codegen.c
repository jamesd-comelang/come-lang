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
    return 0;
}
