#ifndef PARSER_H
#define PARSER_H
#include "lexer.h"
#include "ast.h"
int parse_file(const char* filename, ASTNode** out_ast);
#endif
