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
    return 0;
}
