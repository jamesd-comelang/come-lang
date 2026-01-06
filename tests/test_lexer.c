#include <stdio.h>
#include <string.h>
#include "lexer.h"

int main() {
    TokenList tokens;
    if (lex_file("examples/hello.co", &tokens)) {
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mLexer failed\033[0m\n");
        return 1;
    }

    int found_printf = 0;
    for(int i=0;i<tokens.count;i++){
        if(tokens.tokens[i].type == TOKEN_IDENTIFIER && strcmp(tokens.tokens[i].text, "printf") == 0) found_printf = 1;
    }

    if(found_printf) {
        printf("\033[1;38;2;255;255;255;48;2;0;150;0mLexer test passed!\033[0m\n");
        return 0;
    } else {
        printf("\033[0;31mLexer test failed!\033[0m\n");
        return 1;
    }
}

