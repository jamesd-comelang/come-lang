#ifndef CODEGEN_H
#define CODEGEN_H
#include "ast.h"
int generate_c_from_ast(ASTNode* ast, const char* out_file, const char* source_file, int gen_line_map);
#endif
