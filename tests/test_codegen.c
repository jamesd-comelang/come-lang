#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "parser.h"
#include "codegen.h"
#include "ast.h"

int g_verbose = 0;

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

    ast_free(root);
    root = NULL;

    if (parse_file("tests/fixtures/var_type_mismatch.co", &root) != 0) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mParser failed for var mismatch fixture\033[0m\n");
        return 1;
    }

    if (generate_c_from_ast(root, "build/tests/var_type_mismatch.c", "tests/fixtures/var_type_mismatch.co", 0) == 0) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCodegen accepted incompatible var reassignment\033[0m\n");
        return 1;
    }
    ast_free(root);
    root = NULL;

    if (parse_file("tests/fixtures/var_enum_inference.co", &root) != 0) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mParser failed for var enum fixture\033[0m\n");
        return 1;
    }

    const char* enum_out = "build/tests/var_enum_inference.c";
    if (generate_c_from_ast(root, enum_out, "tests/fixtures/var_enum_inference.co", 0) != 0) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCodegen failed for var enum fixture\033[0m\n");
        return 1;
    }

    f = fopen(enum_out, "r");
    if (!f) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mEnum output file not created\033[0m\n");
        return 1;
    }
    char buf[8192];
    size_t nread = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[nread] = '\0';
    if (!strstr(buf, "int color = YELLOW;")) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mVar enum inference did not emit int color\033[0m\n");
        return 1;
    }

    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCodegen test passed!\033[0m\n");
    ast_free(root);
    return 0;
}
