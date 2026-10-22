#ifndef CODEGEN_H
#define CODEGEN_H
#include "ast.h"
int generate_c_from_ast(ASTNode* ast, const char* out_file, const char* source_file, int gen_line_map);

// COIL-108: directory-module group control, set by the driver around
// generate_c_from_ast and reset to defaults (-1/0/NULL) afterwards.
// Defaults reproduce single-file behavior exactly.
//   g_codegen_suppress_singletons: this file is a non-primary file
//     of a directory module - the module ctx global and the
//     init/exit chain are defined by the primary file only.
//   g_codegen_group_has_init / _exit: whether ANY file of the group
//     defines the hook (-1: derive from this file's own AST).
//   g_codegen_chain_imports: the group's union import list, used
//     for the init/exit chain instead of this file's own imports.
extern int g_codegen_suppress_singletons;
extern int g_codegen_group_has_init;
extern int g_codegen_group_has_exit;
extern const char** g_codegen_chain_imports;
extern int g_codegen_chain_import_count;
#endif
