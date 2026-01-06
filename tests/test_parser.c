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
    return 0;
}
