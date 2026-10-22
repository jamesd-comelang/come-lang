#include <stdio.h>
#include <string.h>
#include "parser.h"
#include "ast.h"

int main() {
    ASTNode* root = NULL;
    if (parse_file("examples/hello.co", &root) != 0) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mParser failed\033[0m\n");
        return 1;
    }

    if (root == NULL) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mParser returned NULL root\033[0m\n");
        return 1;
    }

    if (root->type != AST_PROGRAM) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mRoot type mismatch. Expected %d, got %d\033[0m\n", AST_PROGRAM, root->type);
        return 1;
    }

    // Check for main function
    int found_main = 0;
    for (int i = 0; i < root->child_count; i++) {
        ASTNode* node = root->children[i];
        if (node->type == AST_FUNCTION && strcmp(node->text, "main") == 0) {
            found_main = 1;
            break;
        }
    }

    if (!found_main) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mMissing main function\033[0m\n");
        return 1;
    }
 
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mParser test passed!\033[0m\n");
    ast_free(root);

    // COIL-100: { } is the one literal syntax. A [ ... ] array literal
    // must fail parsing; the same program with { ... } must parse.
    const char* bad_path = "build/tests/bracket_literal.co";
    const char* good_path = "build/tests/brace_literal.co";
    FILE* f = fopen(bad_path, "w");
    if (!f) { printf("Parser test setup failed (cannot write temp file)\n"); return 1; }
    fputs("module main\nimport std\n\nint main() {\n    int a[] = [1, 2, 3]\n    return 0\n}\n", f);
    fclose(f);
    f = fopen(good_path, "w");
    if (!f) { printf("Parser test setup failed (cannot write temp file)\n"); return 1; }
    fputs("module main\nimport std\n\nint main() {\n    int a[] = {1, 2, 3}\n    return 0\n}\n", f);
    fclose(f);

    ASTNode* bad_ast = NULL;
    if (parse_file(bad_path, &bad_ast) == 0) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-100: [...] literal parsed but must be an error\033[0m\n");
        return 1;
    }
    if (bad_ast) ast_free(bad_ast);

    ASTNode* good_ast = NULL;
    if (parse_file(good_path, &good_ast) != 0 || !good_ast) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mCOIL-100: {...} literal failed to parse\033[0m\n");
        return 1;
    }
    ast_free(good_ast);
    remove(bad_path);
    remove(good_path);
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-100 literal syntax test passed!\033[0m\n");
    return 0;
}
