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
 
    ast_free(root);

    ASTNode* sig_root = NULL;
    if (parse_file("tests/fixtures/struct_method_signature.co", &sig_root) != 0 || !sig_root) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mParser failed for struct method signature fixture\033[0m\n");
        return 1;
    }

    ASTNode* counter = NULL;
    for (int i = 0; i < sig_root->child_count; i++) {
        ASTNode* node = sig_root->children[i];
        if (node->type == AST_STRUCT_DECL && strcmp(node->text, "Counter") == 0) {
            counter = node;
            break;
        }
    }
    if (!counter) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mMissing Counter struct\033[0m\n");
        return 1;
    }

    ASTNode* method = NULL;
    for (int i = 0; i < counter->child_count; i++) {
        ASTNode* child = counter->children[i];
        if (child->type == AST_FUNCTION && strcmp(child->text, "Counter_add") == 0) {
            method = child;
            break;
        }
    }
    if (!method || method->child_count < 3 ||
        strcmp(method->children[0]->text, "int") != 0 ||
        strcmp(method->children[1]->text, "self") != 0 ||
        strcmp(method->children[1]->children[1]->text, "Counter*") != 0 ||
        strcmp(method->children[2]->text, "delta") != 0 ||
        strcmp(method->children[2]->children[1]->text, "int") != 0) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mStruct method signature AST mismatch\033[0m\n");
        return 1;
    }

    printf("\033[1;38;2;255;255;255;48;2;0;150;0mParser test passed!\033[0m\n");
    ast_free(sig_root);
    return 0;
}
