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
extern int g_codegen_state_home;
extern int g_codegen_group_has_init;
extern int g_codegen_group_has_exit;
extern const char** g_codegen_chain_imports;
extern int g_codegen_chain_import_count;
// COIL-111: 1 while generating a system module (its source lives
// in the compiler's own tree); extern "C" is allowed only there.
extern int g_codegen_system_module;
// COIL-127 (spec 3.4): the driver registers each local module's
// exported names as it parses the module's files; codegen then
// rejects cross-module calls to symbols the module did not
// export. A module with no export statement stays open.
// The most names one module may export (COIL-152). The
// driver refuses a longer list outright; the registry
// never truncates one silently.
#define COME_MAX_EXPORTS 1024
// Registers exported names; returns -1 when a name did
// not fit under COME_MAX_EXPORTS (the caller refuses the
// build - a silently dropped export is the bug COIL-152
// removes).
int codegen_register_module_exports(const char* module, const char** names, int count);
// COIL-129: the driver registers every symbol (function,
// global, constant) of a directory module's files before
// generating any of them, so cross-file references are
// not mistaken for undefined names.
void codegen_register_group_symbols(const char* module, const char** names, int count);

// Module state registry (COIL-140): the driver analyzes each
// user module at parse time - its module-level variables and
// which functions use them, directly or through calls - and
// registers the result so codegen can split the namespace
// surface from the instance surface.
void codegen_register_module_state(const char* module,
                                   const char** var_names,
                                   const char** var_types, int var_count,
                                   const char** stateful,
                                   const char** chains, int stateful_count,
                                   int has_main,
                                   const char** fn_names, const char** fn_rets,
                                   const char** fn_params, int fn_count);
const char* codegen_module_fn_ret(const char* module, const char* fn);
int codegen_module_fn_count(const char* module);
int codegen_module_fn_at(const char* module, int i, const char** name,
                         const char** ret, const char** params);
int codegen_module_has_state(const char* module);
int codegen_fn_stateful(const char* module, const char* fn, const char** chain_out);
int codegen_module_var_index(const char* module, const char* name, const char** type_out);

// Module struct registry (COIL-146): the driver registers
// each user module's struct definitions (fields serialized
// as "name\x1ftype" pairs joined by ';') so an importer's
// codegen can emit the definitions its prototypes and
// field accesses need - the types half of spec 3.4.
void codegen_register_module_structs(const char* module,
                                     const char** names, const char** defs,
                                     const int* packeds, const int* aligns,
                                     int count);
int codegen_module_struct_count(const char* module);
int codegen_module_struct_at(const char* module, int i, const char** name,
                             const char** def, int* packed, int* align);

// Module const registry (COIL-148): each user module's
// module-level value constants with their C types. A
// constant is static to its module - one namespaced
// definition, externs everywhere else in the module, and
// qualified access (mod.NAME) for exporters.
void codegen_register_module_consts(const char* module,
                                    const char** names, const char** ctypes,
                                    int count);
const char* codegen_module_const_type(const char* module, const char* name);
int codegen_module_const_count(const char* module);
int codegen_module_const_at(const char* module, int i, const char** name,
                            const char** ctype);
#endif
