// src/come_compiler.c
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <time.h>
#include <dirent.h>

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "codegen.h"
#include "common.h"

// Silence truncation warnings for path operations
#pragma GCC diagnostic ignored "-Wformat-truncation"

int g_verbose = 0;
static char g_project_root[PATH_MAX];
// The anchor for the src/ shapes of module resolution
// (spec 3.2). Normally the working directory (the project
// root); `come test` overrides it through COME_PROJECT_ROOT
// because it builds each test from a scratch working
// directory (COIL-150).
static char g_src_root[PATH_MAX];
static char g_ccache_dir[PATH_MAX];
static char g_build_dir[PATH_MAX];

/* ---------- Data Structures ---------- */

typedef struct StringList {
    char *value;
    struct StringList *next;
} StringList;

static StringList *g_object_files = NULL;
static StringList *g_visited_files = NULL;
// Design 6 (redesign): --single-tu concatenates every
// user .c file into one translation unit, in link order
// (so global initialization order matches a separate
// build), so the C compiler can inline across module
// and file boundaries without LTO.
static StringList *g_c_files = NULL;
static int g_single_tu = 0;

static void list_add(StringList **head, const char *val) {
    StringList *node = malloc(sizeof(StringList));
    node->value = strdup(val);
    node->next = *head;
    *head = node;
}

static int list_contains(StringList *head, const char *val) {
    while (head) {
        if (strcmp(head->value, val) == 0) return 1;
        head = head->next;
    }
    return 0;
}

static void list_free(StringList *head) {
    while (head) {
        StringList *next = head->next;
        free(head->value);
        free(head);
        head = next;
    }
}

/* ---------- Utilities ---------- */

static void analyze_module(const char *module_name, ASTNode **asts,
                           const char **files, int nast);

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}


// Check if file exists
static int file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

// Get file modification time. Returns 0 if not exists.
static time_t get_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_mtime;
}

// Create directory if not exists (non-recursive for now, used with built paths)
static void ensure_dir(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    
    // Iterate string to create parents
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            // Ignore error if exists
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static int run_cmd(const char *fmt, ...) {
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_verbose) fprintf(stderr, "[CMD] %s\n", buf);
    return system(buf);
}

// Extra C flags for every compile and link this process runs,
// taken from the COME_CFLAGS environment variable (unset =
// none). `come test --race` and `--cover` set it for their
// child builds (COIL-138); users may set it for plain builds.
static const char *cc_extra_flags(void) {
    static char buf[1024];
    const char *v = getenv("COME_CFLAGS");
    if (!v || !*v) return "";
    snprintf(buf, sizeof(buf), " %s", v);
    return buf;
}

static void check_build_essentials(void) {
    int ret = system("gcc --version > /dev/null 2>&1");
    if (ret != 0) {
        fprintf(stderr, "Error: Build essentials (gcc) not found.\n");
        fprintf(stderr, "Please install gcc/build-essential (e.g. apt install build-essential)\n");
        exit(1);
    }
}

// Find project root based on .git or build/come presence, or fallback to cwd
static void detect_project_root(char *out, size_t sz) {
    if (getcwd(out, sz) == NULL) die("getcwd failed");
}

/* ---------- Module Resolution (spec 3.2, COIL-108) ---------- */

// Facts about a directory module (a set of .co files that together
// form one module), computed by the driver and handed to codegen
// for each of its files.
typedef struct {
    int is_group;          // the files form one directory module
    int is_primary;        // this file defines the module singletons
    int has_init;          // some file of the group defines an init hook
    int has_exit;          // some file of the group defines an exit hook
    const char** imports;  // union of the group's imports
    int import_count;
    int is_state_home;     // this file declares the module's variables (COIL-140)
    const char* name;      // the group's module name (COIL-156 coverage)
} ModuleGroup;

static void compile_file(const char *source_path, const char *forced_o_path,
                         const ModuleGroup *group);
static void compile_import(const char *import_name, const char *current_file, int force_system);

/* ---------- Compilation ---------- */

// Is this source file part of a system module (COIL-111,
// spec 3.3)? System sources live in the compiler's own tree:
// <root>/src/ or <root>/lib/modules, root being the
// executable's grandparent directory.
static int file_is_system_module(const char *abs_path) {
    char exe_path_s[PATH_MAX];
    ssize_t len_s = readlink("/proc/self/exe", exe_path_s, sizeof(exe_path_s) - 1);
    if (len_s == -1) return 0;
    exe_path_s[len_s] = 0;
    char root[PATH_MAX];
    snprintf(root, sizeof(root), "%s", exe_path_s);
    dirname(root); // bin/build dir
    dirname(root); // the compiler tree root
    char prefix[PATH_MAX];
    snprintf(prefix, sizeof(prefix), "%s/src/", root);
    if (strncmp(abs_path, prefix, strlen(prefix)) == 0) {
        // A test program under a system module's t/ directory
        // is a user program that exercises the module, not
        // part of the module's implementation: it gets the
        // full user-module treatment (analysis, registry).
        // Classifying it as system skipped analyze_module,
        // so module-level state in such a test silently
        // compiled without the registry (COIL-142).
        const char *rest = abs_path + strlen(prefix);
        if (strstr(rest, "/t/") != NULL) return 0;
        return 1;
    }
    snprintf(prefix, sizeof(prefix), "%s/lib/modules/", root);
    if (strncmp(abs_path, prefix, strlen(prefix)) == 0) return 1;
    return 0;
}

// Compile a single file, recursing on imports. `group` is non-NULL
// when the file belongs to a directory module (COIL-108).
static void compile_file(const char *source_path, const char *forced_o_path,
                         const ModuleGroup *group) {
    char abs_path[PATH_MAX];
    if (!realpath(source_path, abs_path)) {
        snprintf(abs_path, sizeof(abs_path), "%s", source_path);
    }

    if (list_contains(g_visited_files, abs_path)) return;
    list_add(&g_visited_files, abs_path);

    if (g_verbose) printf("Compiling: %s\n", abs_path);

    // 1. Parse AST to find imports
    ASTNode *ast = NULL;
    if (parse_file(abs_path, &ast) != 0 || !ast) {
        die("Parsing failed: %s", abs_path);
    }
    // A file written in the module.create() block form
    // names no module in its text - the hook implies the
    // file's own module. Attribute it: the group's name
    // for a group file, the file's stem for a lone file.
    // (Codegen otherwise defaults an unnamed AST to the
    // entry module, attributing the hook to "main".)
    if (ast->type == AST_PROGRAM && ast->text[0] == 0) {
        if (group && group->is_group && group->name) {
            strncpy(ast->text, group->name, 255);
        } else {
            char stem[PATH_MAX];
            snprintf(stem, sizeof(stem), "%s", abs_path);
            char *bn = basename(stem);
            char *dot = strrchr(bn, '.');
            if (dot) *dot = 0;
            strncpy(ast->text, bn, 255);
        }
        ast->text[255] = 0;
    }

    // 1b. Export registry (spec 3.4, COIL-127): record this
    // module's exported names so importers' codegen can
    // enforce visibility. This runs at parse time, on every
    // build: registration must not depend on the transpile
    // cache being cold. System modules are exempt - their
    // surface is the runtime's own.
    if (!file_is_system_module(abs_path) && ast->type == AST_PROGRAM) {
        for (int i = 0; i < ast->child_count; i++) {
            ASTNode* ch = ast->children[i];
            if (ch && ch->type == AST_EXPORT) {
                // Past the cap the old code silently
                // dropped the rest of the list (COIL-152);
                // refuse the list instead.
                if (ch->child_count > COME_MAX_EXPORTS)
                    die("module '%s': the export list names %d symbols; the limit is %d",
                        ast->text, ch->child_count, COME_MAX_EXPORTS);
                const char* names[COME_MAX_EXPORTS];
                int n = 0;
                for (int j = 0; j < ch->child_count && n < COME_MAX_EXPORTS; j++) {
                    names[n++] = ch->children[j]->text;
                }
                if (n > 0 && codegen_register_module_exports(ast->text, names, n) != 0)
                    die("module '%s': more than %d exported symbols in total - the export registry is full",
                        ast->text, COME_MAX_EXPORTS);
            }
        }
    }

    // 1c. Module state analysis (COIL-140): single-file
    // modules only - directory groups are analyzed as a
    // unit by compile_group. System modules keep their
    // runtime surface untouched.
    if (!(group && group->is_group) && !file_is_system_module(abs_path) &&
        ast->type == AST_PROGRAM && ast->text[0]) {
        ASTNode *one = ast;
        const char *one_file = abs_path;
        analyze_module(ast->text, &one, &one_file, 1);
    }

    // 2. Scan for imports and recurse
    if (ast->type == AST_PROGRAM) {
        for (int i = 0; i < ast->child_count; i++) {
            if (ast->children[i] && ast->children[i]->type == AST_IMPORT) {
                char *import_name = ast->children[i]->text;
                if (strcmp(import_name, "std") == 0 || strcmp(import_name, "string") == 0 ||
                    strcmp(import_name, "array") == 0 || strcmp(import_name, "map") == 0) {
                    continue;
                }

                compile_import(import_name, abs_path, ast->children[i]->is_extern);
            }
        }
    }

    // 3. Determine Output Paths
    char c_file[PATH_MAX];
    if (forced_o_path) {
        strcpy(c_file, forced_o_path);
    } else {
        char rel_path[PATH_MAX];
        if (strncmp(abs_path, g_project_root, strlen(g_project_root)) == 0) {
            const char *p = abs_path + strlen(g_project_root);
            while (*p == '/') p++;
            strncpy(rel_path, p, sizeof(rel_path));
        } else {
            strcpy(rel_path, basename(abs_path));
        }
        snprintf(c_file, sizeof(c_file), "%s/%s.c", g_ccache_dir, rel_path);
    }
    
    char c_dir[PATH_MAX];
    strcpy(c_dir, c_file);
    // dirname's return value matters: for a slash-less name
    // ("out.c") glibc returns "." without modifying the
    // buffer, and ensure_dir on the unmodified name would
    // mkdir a directory called "out.c" and fail the write.
    char *dir_part = dirname(c_dir);
    if (dir_part != c_dir) strcpy(c_dir, dir_part);
    ensure_dir(c_dir);

    char o_file[PATH_MAX];
    char base_name[PATH_MAX];
    strcpy(base_name, abs_path);
    char *bn = basename(base_name);
    char *dot = strrchr(bn, '.');
    if (dot) *dot = 0;
    snprintf(o_file, sizeof(o_file), "%s/%s.o", g_build_dir, bn);
    
    if (!forced_o_path) ensure_dir(g_build_dir);

    // 4. Incremental Check
    time_t t_src = get_mtime(abs_path);
    time_t t_c = get_mtime(c_file);
    time_t t_o = get_mtime(o_file);

    // The compiler itself is an input to the cache: cached C written by
    // an older compiler build must be regenerated, even when the source
    // is unchanged - otherwise a compiler upgrade (e.g. a renamed method
    // dispatch) leaves stale generated C that no clean rebuild fixes.
    time_t t_exe = 0;
    char self_path[PATH_MAX];
    ssize_t self_len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (self_len != -1) {
        self_path[self_len] = 0;
        t_exe = get_mtime(self_path);
    }

    // Design 2 (redesign): >=, not >, on both comparisons.
    // mtimes have 1-second granularity, so two different files
    // sharing a .ccache basename (every e2e fixture is main.co)
    // built in the same second would otherwise collide: the
    // second build would reuse the first's generated C.
    // An unchanged source is always strictly older than the
    // cache generated from it, so >= only retranspiles when
    // the source or compiler is same-or-newer - possibly
    // changed, or ambiguous - which is always safe.
    int need_transpile = (t_src >= t_c) || (t_exe >= t_c);
    int need_compile = (need_transpile || t_c > t_o || t_src > t_o);
    
    if (get_mtime(o_file) == 0) need_compile = 1;
    if (forced_o_path) need_compile = 0; // If forcing output (genc), we might not compile or we don't care about object

    // 5. Transpile
    if (need_transpile || forced_o_path) {
        if (g_verbose) printf("Transpiling %s -> %s\n", abs_path, c_file);
        // Directory-module group state for codegen (COIL-108): only
        // the group's primary file defines the module singletons,
        // and its init/exit chain uses the group's union imports and
        // hook facts. Set immediately before generating THIS file so
        // nested import compiles are unaffected, reset right after.
        int grouped = (group && group->is_group);
        g_codegen_suppress_singletons = (grouped && !group->is_primary) ? 1 : 0;
        g_codegen_state_home = grouped ? (group->is_state_home ? 1 : 0) : 1;
        g_codegen_group_has_init = (grouped && group->is_primary) ? group->has_init : -1;
        g_codegen_group_has_exit = (grouped && group->is_primary) ? group->has_exit : -1;
        g_codegen_chain_imports = (grouped && group->is_primary) ? group->imports : NULL;
        g_codegen_chain_import_count = (grouped && group->is_primary) ? group->import_count : 0;
        // System module (COIL-111, spec 3.3): the source lives
        // in the compiler's own tree.
        g_codegen_system_module = file_is_system_module(abs_path);
        int gen_rc = generate_c_from_ast(ast, c_file, abs_path, 1);
        g_codegen_suppress_singletons = 0;
        g_codegen_state_home = 1;
        g_codegen_group_has_init = -1;
        g_codegen_group_has_exit = -1;
        g_codegen_chain_imports = NULL;
        g_codegen_chain_import_count = 0;
        g_codegen_system_module = 0;
        if (gen_rc != 0) {
            die("Codegen failed: %s", abs_path);
        }
    }
    ast_free(ast);

    // 6. Compiler (C -> O)
    // Design 6 (redesign): in single-TU mode the
    // per-file objects are never linked, so skip
    // compiling them (the single TU is compiled at
    // link time); the .c files are still generated.
    if (need_compile && !forced_o_path && !g_single_tu) {
        if (g_verbose) printf("Compiling C %s -> %s\n", c_file, o_file);
        
        char exe_path[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path)-1);
        if (len != -1) exe_path[len] = 0;
        char *exe_dir = dirname(exe_path);
        char *build_dir_name = basename(exe_dir); 
        char project_base[PATH_MAX];
        
        strcpy(project_base, exe_dir); 
        if (strcmp(build_dir_name, "build") == 0) {
             dirname(project_base);
        }

        char cmd[65536];
        // Check if we are running from an installed location (e.g. /usr/bin or /usr/local/bin)
        // If "build" is not in the path, we assume installed.
        // Actually, let's check for ../include/come_string.h relative to executable
        char include_dir[PATH_MAX];
        snprintf(include_dir, sizeof(include_dir), "%s/../include", exe_dir);
        
        char check_file[PATH_MAX];
        snprintf(check_file, sizeof(check_file), "%s/come_string.h", include_dir);
        
        
        int is_installed = file_exists(check_file);

        if (is_installed) {
             snprintf(cmd, sizeof(cmd), 
                "gcc -c -Wall -Wno-cpp -Wno-implicit-function-declaration -D__STDC_WANT_LIB_EXT1__=1 "
                "-I\"%s\" "
                "\"%s\" -o \"%s\"%s",
                include_dir,
                c_file, o_file, cc_extra_flags());
        } else {
             snprintf(cmd, sizeof(cmd), 
                "gcc -c -Wall -Wno-cpp -Wno-implicit-function-declaration -D__STDC_WANT_LIB_EXT1__=1 "
                "-I%s/src/include -I%s/src/core/include "
                "\"%s\" -o \"%s\"%s",
                project_base, project_base,
                c_file, o_file, cc_extra_flags());
        }
            
        if (run_cmd(cmd) != 0) {
            die("C Compilation failed: %s", c_file);
        }
    }
    
    if (!forced_o_path) {
        if (!g_single_tu) list_add(&g_object_files, o_file);
        list_add(&g_c_files, c_file);
    }
}

// Collect the .co files of a directory, sorted by file name so the
// module's primary file (the first) is deterministic. Returns the
// count (0 when the directory does not exist or holds no .co files).
static int scan_co_dir(const char *dir, char paths[][PATH_MAX], int max_paths) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    static char names[128][256];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        size_t len = strlen(name);
        if (len > 3 && strcmp(name + len - 3, ".co") == 0 && n < 128) {
            snprintf(names[n], sizeof(names[n]), "%s", name);
            n++;
        }
    }
    closedir(d);
    for (int i = 1; i < n; i++) {
        char tmp[256];
        strcpy(tmp, names[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], tmp) > 0) { strcpy(names[j + 1], names[j]); j--; }
        strcpy(names[j + 1], tmp);
    }
    int out_n = n < max_paths ? n : max_paths;
    for (int i = 0; i < out_n; i++)
        snprintf(paths[i], PATH_MAX, "%s/%s", dir, names[i]);
    return out_n;
}


// === Module state analysis (COIL-140) ===
// One pass over a module's files at parse time: collect its
// module-level variables and its functions, then compute
// statefulness as a fixpoint over the call graph - a
// function is stateful when its body mentions a module
// variable or calls a stateful function. The walk does not
// track shadowing (a local named like a module variable
// marks the function stateful); codegen's own resolution
// decides each individual use, so over-marking only costs
// an instance parameter the body may not need, never a
// wrong call. Each stateful function keeps a witness chain
// ("tick -> bump -> count") for the namespace error.
typedef struct { char name[128]; char type[160]; char file[PATH_MAX]; } AnaVar;
// A struct definition for the COIL-146 registry: fields
// serialized as "name\x1ftype" pairs joined by ';'.
typedef struct { char name[128]; char def[2048]; int packed; int align; } AnaStruct;
// A module-level value constant for the COIL-148 registry.
typedef struct { char name[128]; char ctype[32]; char file[PATH_MAX]; } AnaConst;

// The C type of a constant's initializer, mirroring
// codegen's infer_const_type exactly - the registry type
// must match the definition every file emits.
static const char *ana_const_ctype(ASTNode *init) {
    if (!init) return "int";
    if (init->type == AST_STRING_LITERAL) return "char*";
    if (init->type != AST_NUMBER) return "int";
    char *text = init->text;
    if (strchr(text, '.') || strstr(text, "f") || strstr(text, "F")) return "float";
    int is_unsigned = (strstr(text, "u") != NULL || strstr(text, "U") != NULL);
    int is_long = (strstr(text, "l") != NULL || strstr(text, "L") != NULL);
    int is_long_long = (strstr(text, "LL") != NULL || strstr(text, "ll") != NULL);
    if (is_unsigned) {
        if (is_long_long) return "unsigned long long";
        if (is_long) return "unsigned long";
        return "unsigned int";
    }
    if (is_long_long) return "long long";
    if (is_long) return "long";
    return "int";
}

// Record one plain value constant (COIL-148): enum members
// (a const decl wrapping an enum decl) are not value
// constants and stay with the enum machinery. A second
// constant with the same name anywhere in the module is a
// hard error - one module, one definition.
static void ana_add_const(AnaConst *consts, int *count, const char *module_name,
                          ASTNode *decl, const char *file) {
    if (!decl || decl->type != AST_CONST_DECL || !decl->text[0]) return;
    if (decl->child_count > 0 && decl->children[0] &&
        decl->children[0]->type == AST_ENUM_DECL) return;
    for (int i = 0; i < *count; i++) {
        if (strcmp(consts[i].name, decl->text) == 0)
            die("module '%s': constant '%s' is declared twice (%s and %s) - "
                "a constant has one definition in its module; remove one declaration",
                module_name, decl->text, consts[i].file, file);
    }
    if (*count >= 256) return;
    AnaConst *c = &consts[(*count)++];
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", decl->text);
    snprintf(c->ctype, sizeof(c->ctype), "%s",
             ana_const_ctype(decl->child_count > 0 ? decl->children[0] : NULL));
    snprintf(c->file, sizeof(c->file), "%s", file);
}
typedef struct {
    char name[128];
    char ret[64];
    char params[512];
    ASTNode *body;
    int is_coro, is_method, is_extern, direct;
    char direct_var[128];
    int edges[64], edge_count;
    int stateful;
    char chain[320];
} AnaFn;

static void ana_walk(ASTNode *n, AnaVar *vars, int var_count,
                     AnaFn *fns, int fn_count, AnaFn *cur) {
    if (!n) return;
    if (n->type == AST_IDENTIFIER) {
        for (int i = 0; i < var_count; i++)
            if (strcmp(n->text, vars[i].name) == 0) {
                cur->direct = 1;
                snprintf(cur->direct_var, sizeof(cur->direct_var), "%s", vars[i].name);
            }
    }
    for (int i = 0; i < n->child_count; i++)
        ana_walk(n->children[i], vars, var_count, fns, fn_count, cur);
}

// Edges are gathered in a second walk that knows the
// module's own name (for self-prefixed calls).
static void ana_walk_edges(ASTNode *n, const char *module_name,
                           AnaFn *fns, int fn_count, AnaFn *cur) {
    if (!n) return;
    const char *callee = NULL;
    if (n->type == AST_CALL) callee = n->text;
    else if (n->type == AST_METHOD_CALL && n->child_count > 0 &&
             n->children[0] && n->children[0]->type == AST_IDENTIFIER &&
             strcmp(n->children[0]->text, module_name) == 0)
        callee = n->text;
    if (callee) {
        for (int i = 0; i < fn_count; i++)
            if (strcmp(fns[i].name, callee) == 0 && cur->edge_count < 64) {
                int seen = 0;
                for (int j = 0; j < cur->edge_count; j++)
                    if (cur->edges[j] == i) seen = 1;
                if (!seen) cur->edges[cur->edge_count++] = i;
            }
    }
    for (int i = 0; i < n->child_count; i++)
        ana_walk_edges(n->children[i], module_name, fns, fn_count, cur);
}

static char g_ana_home_file[PATH_MAX];

static void analyze_module(const char *module_name, ASTNode **asts,
                           const char **files, int nast) {
    g_ana_home_file[0] = 0;
    static AnaVar vars[64];
    static AnaFn fns[192];
    static AnaStruct structs[64];
    static AnaConst consts[256];
    int var_count = 0, fn_count = 0, struct_count = 0, const_count = 0;
    for (int a = 0; a < nast; a++) {
        ASTNode *ast = asts[a];
        if (!ast || ast->type != AST_PROGRAM) continue;
        for (int i = 0; i < ast->child_count; i++) {
            ASTNode *ch = ast->children[i];
            if (!ch) continue;
            if (ch->type == AST_CONST_DECL) {
                ana_add_const(consts, &const_count, module_name, ch, files[a]);
            } else if (ch->type == AST_CONST_GROUP) {
                // A group of value constants - unless it is
                // an enum group (its members wrap enum
                // declarations), which stays with the enums.
                int is_enum_group = ch->child_count > 0 && ch->children[0] &&
                    ch->children[0]->child_count > 0 && ch->children[0]->children[0] &&
                    ch->children[0]->children[0]->type == AST_ENUM_DECL;
                if (!is_enum_group)
                    for (int j = 0; j < ch->child_count; j++)
                        ana_add_const(consts, &const_count, module_name, ch->children[j], files[a]);
            } else if (ch->type == AST_STRUCT_DECL && ch->text[0] && struct_count < 64) {
                AnaStruct *st = &structs[struct_count];
                memset(st, 0, sizeof(*st));
                snprintf(st->name, sizeof(st->name), "%s", ch->text);
                st->packed = ch->layout_packed;
                st->align = ch->layout_align;
                st->def[0] = 0;
                for (int j = 0; j < ch->child_count; j++) {
                    ASTNode *fd = ch->children[j];
                    if (!fd || fd->type != AST_VAR_DECL || fd->child_count < 2 || !fd->children[1]) continue;
                    if (st->def[0]) strncat(st->def, ";", sizeof(st->def) - strlen(st->def) - 1);
                    strncat(st->def, fd->text, sizeof(st->def) - strlen(st->def) - 1);
                    strncat(st->def, "\x1f", sizeof(st->def) - strlen(st->def) - 1);
                    strncat(st->def, fd->children[1]->text, sizeof(st->def) - strlen(st->def) - 1);
                }
                struct_count++;
            } else if (ch->type == AST_VAR_DECL && ch->text[0] && var_count < 64) {
                AnaVar *v = &vars[var_count++];
                memset(v, 0, sizeof(*v));
                snprintf(v->name, sizeof(v->name), "%s", ch->text);
                if (ch->child_count > 1 && ch->children[1])
                    snprintf(v->type, sizeof(v->type), "%s", ch->children[1]->text);
                else
                    snprintf(v->type, sizeof(v->type), "int");
                // A `var` module variable registers its
                // inferred type, never the literal text
                // "var" (COIL-154): the instance-struct
                // emitter prints the registered type, and
                // "var" is not a C type. The inference
                // mirrors the declaration-site rules - a
                // create() call makes the variable an
                // instance of that module (recorded under
                // the module's name, like a typed instance
                // declaration); literals give their plain
                // types. Anything else stays "var" and the
                // declaration site reports it.
                if (strcmp(v->type, "var") == 0 && ch->child_count > 0 && ch->children[0]) {
                    ASTNode *init = ch->children[0];
                    if (init->type == AST_METHOD_CALL && strcmp(init->text, "create") == 0 &&
                        init->child_count > 0 && init->children[0] &&
                        init->children[0]->type == AST_IDENTIFIER)
                        snprintf(v->type, sizeof(v->type), "%s", init->children[0]->text);
                    else if (init->type == AST_STRING_LITERAL)
                        snprintf(v->type, sizeof(v->type), "string");
                    else if (init->type == AST_BOOL_LITERAL)
                        snprintf(v->type, sizeof(v->type), "bool");
                    else if (init->type == AST_NUMBER)
                        snprintf(v->type, sizeof(v->type), "%s",
                                 (strchr(init->text, '.') || strchr(init->text, 'e') ||
                                  strchr(init->text, 'E')) ? "double" : "int");
                }
                snprintf(v->file, sizeof(v->file), "%s", files[a]);
            } else if (ch->type == AST_FUNCTION && ch->text[0] && fn_count < 192) {
                AnaFn *fn = &fns[fn_count++];
                memset(fn, 0, sizeof(*fn));
                snprintf(fn->name, sizeof(fn->name), "%s", ch->text);
                fn->is_coro = ch->is_coroutine;
                fn->is_extern = ch->is_extern;
                fn->is_method = strchr(ch->text, '.') != NULL;
                if (ch->child_count > 0 && ch->children[0])
                    snprintf(fn->ret, sizeof(fn->ret), "%s", ch->children[0]->text);
                fn->params[0] = 0;
                int first_param = 1;
                for (int k = 1; k < ch->child_count; k++) {
                    ASTNode *pr = ch->children[k];
                    if (!pr || pr->type == AST_BLOCK) break;
                    if (pr->type != AST_VAR_DECL || pr->child_count < 2) continue;
                    // A struct method (COIL-146): the parser
                    // mangles Struct.method to Struct_method
                    // and injects a first parameter named
                    // "self" of type "Struct*" - the '.' the
                    // check above looks for never survives.
                    // Methods are not module functions: they
                    // stay out of the registry surface.
                    if (first_param && strcmp(pr->text, "self") == 0) {
                        const char *us = strchr(fn->name, '_');
                        if (us) {
                            char want[160];
                            snprintf(want, sizeof(want), "%.*s*",
                                     (int)(us - fn->name), fn->name);
                            if (strcmp(pr->children[1]->text, want) == 0)
                                fn->is_method = 1;
                        }
                    }
                    first_param = 0;
                    if (fn->params[0]) strncat(fn->params, ",", sizeof(fn->params) - strlen(fn->params) - 1);
                    strncat(fn->params, pr->children[1]->text, sizeof(fn->params) - strlen(fn->params) - 1);
                }
                for (int k = 0; k < ch->child_count; k++)
                    if (ch->children[k] && ch->children[k]->type == AST_BLOCK)
                        fn->body = ch->children[k];
            }
        }
    }
    if (var_count > 0) snprintf(g_ana_home_file, sizeof(g_ana_home_file), "%s", vars[0].file);
    int has_main = 0;
    for (int i = 0; i < fn_count; i++)
        if (strcmp(fns[i].name, "main") == 0 && !fns[i].is_coro) has_main = 1;
    // The lifecycle pair (COIL-156): a module is
    // instantiable only when it defines BOTH hooks -
    // create (either spelling) and destroy (either
    // spelling). One without the other is an error: the
    // pair is the module's declaration that it has a
    // lifecycle and can be an instance; a module with
    // neither is namespace-only.
    int hook_create = 0, hook_destroy = 0;
    for (int i = 0; i < fn_count; i++) {
        if (strcmp(fns[i].name, "create") == 0 || strcmp(fns[i].name, "module_create") == 0) hook_create = 1;
        if (strcmp(fns[i].name, "destroy") == 0 || strcmp(fns[i].name, "module_destroy") == 0) hook_destroy = 1;
    }
    if (hook_create != hook_destroy)
        die("module '%s' defines %s() but no %s(): the lifecycle hooks come in a pair - "
            "define both create() and destroy() to make the module instantiable, or "
            "neither (a module without the pair is namespace-only)",
            module_name, hook_create ? "create" : "destroy", hook_create ? "destroy" : "create");
    int can_instance = hook_create && hook_destroy;
    // A variable-less instantiable module still needs a
    // state home: its first file emits the instance
    // runtime (create/destroy with an empty field set).
    if (var_count == 0 && can_instance && nast > 0)
        snprintf(g_ana_home_file, sizeof(g_ana_home_file), "%s", files[0]);
    // One state home: initializers become instance fields,
    // and exactly one file can emit them. (A module with
    // main() is a program and keeps singleton state, so the
    // instance rules below do not apply to it.)
    if (!has_main)
        for (int i = 1; i < var_count; i++)
            if (strcmp(vars[i].file, vars[0].file) != 0)
                die("module '%s': module variables are declared in more than one file (%s and %s) - "
                    "a module with instances keeps its variables in one file (its state home)",
                    module_name, vars[0].file, vars[i].file);
    for (int i = 0; i < fn_count; i++) {
        if (!fns[i].body) continue;
        ana_walk(fns[i].body, vars, var_count, fns, fn_count, &fns[i]);
        ana_walk_edges(fns[i].body, module_name, fns, fn_count, &fns[i]);
    }
    // Fixpoint. Coroutines stay out (their isolation rule
    // already forbids module state); lifecycle hooks are
    // stateful whenever the module has state.
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < fn_count; i++) {
            AnaFn *fn = &fns[i];
            if (fn->stateful || fn->is_coro) continue;
            if (fn->direct) {
                char nm[128], dv[128];
                snprintf(nm, sizeof(nm), "%s", fn->name);
                snprintf(dv, sizeof(dv), "%s", fn->direct_var);
                fn->stateful = 1;
                snprintf(fn->chain, sizeof(fn->chain), "%s -> %s", nm, dv);
                changed = 1;
                continue;
            }
            for (int j = 0; j < fn->edge_count; j++) {
                AnaFn *t = &fns[fn->edges[j]];
                if (t->stateful) {
                    char nm[128], tc[320];
                    snprintf(nm, sizeof(nm), "%s", fn->name);
                    snprintf(tc, sizeof(tc), "%s", t->chain);
                    fn->stateful = 1;
                    snprintf(fn->chain, sizeof(fn->chain), "%s -> %s", nm, tc);
                    changed = 1;
                    break;
                }
            }
        }
    }
    if (var_count > 0) {
        for (int i = 0; i < fn_count; i++) {
            AnaFn *fn = &fns[i];
            if (fn->is_coro) continue;
            if (strcmp(fn->name, "create") == 0 || strcmp(fn->name, "module_create") == 0 ||
                strcmp(fn->name, "destroy") == 0 || strcmp(fn->name, "module_destroy") == 0) {
                if (!fn->stateful) {
                    fn->stateful = 1;
                    char nm[128];
                    snprintf(nm, sizeof(nm), "%s", fn->name);
                    snprintf(fn->chain, sizeof(fn->chain), "%s (module lifecycle)", nm);
                }
            }
            if (fn->stateful && fn->is_method && !has_main && can_instance)
                die("module '%s': method '%s' uses module state (%s) - methods cannot "
                    "use module state in this version; move the state access into a "
                    "plain function of the module", module_name, fn->name, fn->chain);
        }
    }
    const char *vnames[64], *vtypes[64];
    for (int i = 0; i < var_count; i++) { vnames[i] = vars[i].name; vtypes[i] = vars[i].type; }
    const char *sf[192], *sc[192];
    int sn = 0;
    for (int i = 0; i < fn_count; i++)
        if (fns[i].stateful && !fns[i].is_coro) { sf[sn] = fns[i].name; sc[sn] = fns[i].chain; sn++; }
    // A namespace-only module has no instance surface:
    // its functions are plain namespace functions even
    // when they use module variables (those variables
    // are the module's own singleton state), so the
    // registry records no stateful functions for it.
    if (!can_instance) sn = 0;
    const char *fnn[192], *fnr[192], *fnp[192];
    int fnn_count = 0;
    for (int i = 0; i < fn_count; i++) {
        AnaFn *fn = &fns[i];
        if (fn->is_coro || fn->is_method || fn->is_extern) continue;
        if (strcmp(fn->name, "main") == 0) continue;
        if (strcmp(fn->name, "create") == 0 || strcmp(fn->name, "module_create") == 0 ||
            strcmp(fn->name, "destroy") == 0 || strcmp(fn->name, "module_destroy") == 0) continue;
        fnn[fnn_count] = fn->name; fnr[fnn_count] = fn->ret; fnp[fnn_count] = fn->params;
        fnn_count++;
    }
    codegen_register_module_state(module_name, vnames, vtypes, var_count, sf, sc, sn, has_main,
                                  can_instance, fnn, fnr, fnp, fnn_count);
    const char *stn[64], *std_[64];
    int stpk[64], stal[64];
    for (int i = 0; i < struct_count; i++) {
        stn[i] = structs[i].name; std_[i] = structs[i].def;
        stpk[i] = structs[i].packed; stal[i] = structs[i].align;
    }
    codegen_register_module_structs(module_name, stn, std_, stpk, stal, struct_count);
    const char *ctn[256], *ctt[256];
    for (int i = 0; i < const_count; i++) { ctn[i] = consts[i].name; ctt[i] = consts[i].ctype; }
    codegen_register_module_consts(module_name, ctn, ctt, const_count);
    // A5 (perf-experiments): frameless functions for this
    // module, from the same pre-scan ASTs. Names only are
    // registered: directory groups free these ASTs right
    // after this analysis and re-parse per file.
    codegen_analyze_frameless(module_name, asts, nast);
    // A4 (redesign): may-fail functions for this module,
    // from the same pre-scan ASTs.
    codegen_analyze_mayfail(module_name, asts, nast);
    // Design 1 (redesign): may-escape parameters for this module,
    // from the same pre-scan ASTs.
    codegen_analyze_escapes(module_name, asts, nast);
}

// Compile all files of a directory module as one module (COIL-108).
// A pre-scan verifies every file declares the module, finds the
// group's init/exit hooks, and unions its imports for the chain.
static void compile_group(char paths[][PATH_MAX], int count, const char *module_name) {
    int has_init = 0, has_exit = 0;
    const char *imports[64];
    int import_count = 0;
    // Every symbol the group's files declare (COIL-129):
    // registered with codegen so cross-file references are
    // not mistaken for undefined names.
    const char *symbols[1024];
    int symbol_count = 0;
    // The pre-scan ASTs are kept until the state analysis
    // has read them (COIL-140), then freed together.
    ASTNode *asts[64];
    const char *ast_files[64];
    int ast_count = 0;
    for (int i = 0; i < count; i++) {
        ASTNode *ast = NULL;
        if (parse_file(paths[i], &ast) != 0 || !ast) {
            die("Parsing failed: %s", paths[i]);
        }
        if (ast_count < 64) { asts[ast_count] = ast; ast_files[ast_count] = paths[i]; ast_count++; }
        for (int j = 0; j < ast->child_count; j++) {
            ASTNode *ch = ast->children[j];
            if (!ch) continue;
            if ((ch->type == AST_FUNCTION || ch->type == AST_VAR_DECL ||
                 ch->type == AST_CONST_DECL) && ch->text[0] &&
                symbol_count < 1024) {
                symbols[symbol_count++] = strdup(ch->text);
            } else if (ch->type == AST_CONST_GROUP) {
                for (int k = 0; k < ch->child_count && symbol_count < 1024; k++) {
                    if (ch->children[k] && ch->children[k]->text[0])
                        symbols[symbol_count++] = strdup(ch->children[k]->text);
                }
            }
        }
        if (ast->text[0] && strcmp(ast->text, module_name) != 0) {
            die("%s declares module '%s' but sits in the '%s' module directory - "
                "every file of a directory module must declare module %s",
                paths[i], ast->text, module_name, module_name);
        }
        for (int j = 0; j < ast->child_count; j++) {
            ASTNode *ch = ast->children[j];
            if (!ch) continue;
            if (ch->type == AST_IMPORT) {
                int seen = 0;
                for (int k = 0; k < import_count; k++)
                    if (strcmp(imports[k], ch->text) == 0) seen = 1;
                if (!seen && import_count < 64)
                    imports[import_count++] = strdup(ch->text);
            } else if (ch->type == AST_FUNCTION) {
                int has_body = 0;
                for (int k = 0; k < ch->child_count; k++)
                    if (ch->children[k] && ch->children[k]->type == AST_BLOCK)
                        has_body = 1;
                if (has_body && (strcmp(ch->text, "create") == 0 ||
                                 strcmp(ch->text, "module_create") == 0))
                    has_init = 1;
                if (has_body && (strcmp(ch->text, "destroy") == 0 ||
                                 strcmp(ch->text, "module_destroy") == 0))
                    has_exit = 1;
            }
        }
    }
    analyze_module(module_name, asts, ast_files, ast_count);
    for (int i = 0; i < ast_count; i++) ast_free(asts[i]);
    codegen_register_group_symbols(module_name, symbols, symbol_count);
    for (int i = 0; i < count; i++) {
        ModuleGroup g;
        g.is_group = 1;
        g.is_primary = (i == 0);
        g.is_state_home = (g_ana_home_file[0] &&
                           strcmp(paths[i], g_ana_home_file) == 0) ? 1 : 0;
        g.has_init = has_init;
        g.has_exit = has_exit;
        g.imports = imports;
        g.import_count = import_count;
        g.name = module_name;
        compile_file(paths[i], NULL, &g);
    }
}

// Resolve and compile one import, following spec 3.2's order:
// ./X.co, ./X/*.co, ./modules/X.co, ./modules/X/*.co, then the same
// shapes under the project src/ tree, then the system depot. A file
// and a directory providing the same module at one location is an
// error - the module would be ambiguous.
static void compile_import(const char *import_name, const char *current_file, int force_system) {
    char base_dir[PATH_MAX] = {0};
    strncpy(base_dir, current_file, sizeof(base_dir) - 1);
    dirname(base_dir);

    char stems[4][PATH_MAX];
    snprintf(stems[0], PATH_MAX, "%s", base_dir);
    snprintf(stems[1], PATH_MAX, "%s/modules", base_dir);
    snprintf(stems[2], PATH_MAX, "%s/src", g_src_root);
    snprintf(stems[3], PATH_MAX, "%s/src/modules", g_src_root);

    // `import X as ...` (spec 3.2): skip local resolution
    // (stems 0-1) and resolve from the system depot only.
    for (int s = force_system ? 2 : 0; s < 4; s++) {
        char file[PATH_MAX], dir[PATH_MAX];
        snprintf(file, sizeof(file), "%s/%s.co", stems[s], import_name);
        snprintf(dir, sizeof(dir), "%s/%s", stems[s], import_name);
        int has_file = file_exists(file);
        char (*paths)[PATH_MAX] = malloc(64 * sizeof(*paths));
        int n = scan_co_dir(dir, paths, 64);
        if (has_file && n > 0) {
            die("ambiguous module '%s': both %s and the directory %s/ define it - keep only one",
                import_name, file, dir);
        }
        if (has_file) {
            free(paths);
            compile_file(file, NULL, NULL);
            return;
        }
        if (n > 0) {
            compile_group(paths, n, import_name);
            free(paths);
            return;
        }
        free(paths);
    }

    // System depot
    char exe_path_r[PATH_MAX];
    ssize_t len_r = readlink("/proc/self/exe", exe_path_r, sizeof(exe_path_r) - 1);
    if (len_r != -1) {
        exe_path_r[len_r] = 0;
        char *exe_dir_r = dirname(exe_path_r);
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s/../lib/modules/%s.co", exe_dir_r, import_name);
        if (file_exists(candidate)) {
            compile_file(candidate, NULL, NULL);
            return;
        }
    }
    die("Could not resolve import: %s in %s - no module named '%s' was found "
        "(looked for %s.co and a %s/ directory beside the importing file, under "
        "modules/, under src/, and in the system depot; spec 3.2). Check the "
        "spelling, or place the module in one of those locations.",
        import_name, current_file, import_name, import_name, import_name);
}


/* ---------- Developer commands (COIL-138) ---------- */

// Absolute path of the running compiler, for self-invocation
// (check/run/test build through the normal `build` path in a
// child process, so their diagnostics and caching behave
// exactly like a user's build).
static void self_exe(char *out, size_t sz) {
    ssize_t n = readlink("/proc/self/exe", out, sz - 1);
    if (n == -1) { snprintf(out, sz, "come"); return; }
    out[n] = 0;
}

// Single-quote a string for /bin/sh.
static void shell_quote(const char *in, char *out, size_t sz) {
    size_t o = 0;
    if (o + 1 < sz) out[o++] = '\'';
    for (const char *p = in; *p && o + 5 < sz; p++) {
        if (*p == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }
        else out[o++] = *p;
    }
    if (o + 1 < sz) out[o++] = '\'';
    out[o] = 0;
}

static int make_scratch(char *out, size_t sz, const char *tag) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(out, sz, "%s/come-%s-XXXXXX", tmp, tag);
    return mkdtemp(out) != NULL;
}

static void rm_rf(const char *path) {
    char q[PATH_MAX + 8], cmd[PATH_MAX + 32];
    shell_quote(path, q, sizeof(q));
    snprintf(cmd, sizeof(cmd), "rm -rf %s", q);
    if (system(cmd) != 0) { /* best effort */ }
}

static int decode_status(int st) {
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 1;
}

// Print a captured log file to stderr, indented, capped.
static void dump_log(const char *path, int max_lines) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < max_lines) {
        fprintf(stderr, "    %s", line);
        if (line[strlen(line) - 1] != '\n') fputc('\n', stderr);
        n++;
    }
    fclose(f);
}

// Build `input` (a .co file or a project directory) into
// scratch/prog through a child `come build`, capturing the
// child's output to scratch/build.log. Returns 0 on success.
static int build_into_scratch(const char *self, const char *input,
                              const char *scratch, char *bin_out, size_t bin_sz) {
    char qi[PATH_MAX + 8], qb[PATH_MAX + 8], ql[PATH_MAX + 8], cmd[16384];
    snprintf(bin_out, bin_sz, "%s/prog", scratch);
    shell_quote(input, qi, sizeof(qi));
    shell_quote(bin_out, qb, sizeof(qb));
    char logp[PATH_MAX];
    snprintf(logp, sizeof(logp), "%s/build.log", scratch);
    shell_quote(logp, ql, sizeof(ql));
    snprintf(cmd, sizeof(cmd), "\"%s\" build %s -o %s > %s 2>&1", self, qi, qb, ql);
    if (g_verbose) fprintf(stderr, "[CMD] %s\n", cmd);
    int rc = system(cmd);
    return (rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0) ? 0 : 1;
}

/* ----- come check: compile without linking or running ----- */

static int cmd_check(int argc, char *argv[]) {
    const char *input = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) g_verbose = 1;
        else input = argv[i];
    }
    if (!input) {
        fprintf(stderr, "Usage: come check <file.co|project-dir>\n");
        return 1;
    }
    char self[PATH_MAX], scratch[PATH_MAX], bin[PATH_MAX];
    self_exe(self, sizeof(self));
    if (!make_scratch(scratch, sizeof(scratch), "check")) die("cannot create a temp dir");
    int bad = build_into_scratch(self, input, scratch, bin, sizeof(bin));
    if (bad) {
        char logp[PATH_MAX];
        snprintf(logp, sizeof(logp), "%s/build.log", scratch);
        dump_log(logp, 60);
        fprintf(stderr, "check failed: %s\n", input);
        rm_rf(scratch);
        return 1;
    }
    printf("check ok: %s\n", input);
    rm_rf(scratch);
    return 0;
}

/* ----- come run: build to a temp binary, run it, forward args ----- */

static int cmd_run(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: come run <file.co> [--] [args...]\n");
        return 1;
    }
    const char *input = argv[2];
    int arg0 = 3;
    if (arg0 < argc && strcmp(argv[arg0], "--") == 0) arg0++;
    char self[PATH_MAX], scratch[PATH_MAX], bin[PATH_MAX];
    self_exe(self, sizeof(self));
    if (!make_scratch(scratch, sizeof(scratch), "run")) die("cannot create a temp dir");
    if (build_into_scratch(self, input, scratch, bin, sizeof(bin)) != 0) {
        char logp[PATH_MAX];
        snprintf(logp, sizeof(logp), "%s/build.log", scratch);
        dump_log(logp, 60);
        rm_rf(scratch);
        return 1;
    }
    char cmd[16384], q[PATH_MAX + 8];
    shell_quote(bin, q, sizeof(q));
    int pos = snprintf(cmd, sizeof(cmd), "%s", q);
    for (int i = arg0; i < argc; i++) {
        shell_quote(argv[i], q, sizeof(q));
        pos += snprintf(cmd + pos, sizeof(cmd) - pos, " %s", q);
    }
    if (g_verbose) fprintf(stderr, "[CMD] %s\n", cmd);
    int rc = system(cmd);
    rm_rf(scratch);
    if (rc == -1) return 1;
    return decode_status(rc);
}

/* ----- come test: build and run every .co under any t/ dir ----- */

#define TEST_TIMEOUT_S 30
#define MAX_TESTS 4096

static char g_test_paths[MAX_TESTS][PATH_MAX];
static int g_test_count = 0;

static void discover_tests(const char *dir, int depth) {
    if (depth > 24 || g_test_count >= MAX_TESTS) return;
    // If this directory is named "t", its .co files are tests
    // (the Makefile sweep's rule); do not descend further.
    char base[PATH_MAX];
    snprintf(base, sizeof(base), "%s", dir);
    const char *bn = strrchr(base, '/');
    bn = bn ? bn + 1 : base;
    if (strcmp(bn, "t") == 0) {
        static char paths[128][PATH_MAX];
        int n = scan_co_dir(dir, paths, 128);
        for (int i = 0; i < n && g_test_count < MAX_TESTS; i++)
            snprintf(g_test_paths[g_test_count++], PATH_MAX, "%s", paths[i]);
        return;
    }
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        if (name[0] == '.') continue;                 // .git, .ccache, dotdirs
        if (strcmp(name, "build") == 0) continue;
        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", dir, name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode))
            discover_tests(child, depth + 1);
    }
    closedir(d);
}

static int cmp_str(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

// When set (come test --race), test binaries run under
// `setarch -R`: ThreadSanitizer refuses to start when the
// kernel maps the process at an address its shadow layout
// cannot use ("unexpected memory mapping"), which happens
// under full ASLR in some VM/container setups. Disabling
// randomization for the test child is the standard remedy.
static int g_race_wrap = 0;

// Run bin with output captured to logpath. Returns the exit
// code (>= 0), -sig when killed by a signal, or -1000 on
// timeout. elapsed_ms receives the wall time.
static int run_capture(const char *bin, const char *logpath, int timeout_s,
                       double *elapsed_ms) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pid_t pid = fork();
    if (pid == 0) {
        FILE *lf = fopen(logpath, "w");
        if (lf) { dup2(fileno(lf), 1); dup2(fileno(lf), 2); fclose(lf); }
        if (g_race_wrap) {
            char machine[64] = "x86_64";
            struct utsname un;
            if (uname(&un) == 0) snprintf(machine, sizeof(machine), "%s", un.machine);
            execlp("setarch", "setarch", machine, "-R", bin, (char *)NULL);
            // setarch missing after all: fall through to direct exec
        }
        execl(bin, bin, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status = 0, done = 0, timed_out = 0;
    while (!done) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) done = 1;
        else {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            if (el > timeout_s) { kill(pid, SIGKILL); waitpid(pid, &status, 0); timed_out = 1; done = 1; }
            else {
                struct timespec ts = { 0, 2000000 };
                nanosleep(&ts, NULL);
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    *elapsed_ms = ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9) * 1000.0;
    if (timed_out) return -1000;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return -1;
}

static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}

// Line coverage of one test's .co source from the gcov report
// of its generated C (the C carries #line directives pointing
// back at the .co file). Fills exec/hit line counts and a
// compact uncovered-ranges string. Returns 0 when no report.
static int test_coverage(const char *work, const char *test_abs,
                         long *exec_out, long *hit_out,
                         char *uncov, size_t uncov_sz) {
    // basename without .co
    char base[256];
    snprintf(base, sizeof(base), "%s", test_abs);
    char *bn = strrchr(base, '/');
    bn = bn ? bn + 1 : base;
    char stem[256];
    snprintf(stem, sizeof(stem), "%s", bn);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;

    char gcovdir[PATH_MAX];
    snprintf(gcovdir, sizeof(gcovdir), "%s/gcov", work);
    ensure_dir(gcovdir);
    // gcov works from the object file: it finds the notes
    // and data beside it and writes one report per source,
    // including the .co file the #line directives name.
    char ofile[PATH_MAX], objdir[PATH_MAX], cmd[16384];
    snprintf(ofile, sizeof(ofile), "%s/build/%s.o", work, stem);
    snprintf(objdir, sizeof(objdir), "%s/build", work);
    snprintf(cmd, sizeof(cmd), "cd \"%s\" && gcov -o \"%s\" \"%s\" > gcov.log 2>&1",
             gcovdir, objdir, ofile);
    if (system(cmd) != 0) return 0;

    char gcovfile[PATH_MAX];
    snprintf(gcovfile, sizeof(gcovfile), "%s/%s.co.gcov", gcovdir, stem);
    FILE *f = fopen(gcovfile, "r");
    if (!f) return 0;
    long exec = 0, hit = 0;
    int ranges[24][2];
    int nr = 0, open_from = -1, prev = -2;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *c1 = strchr(line, ':');
        if (!c1) continue;
        char cnt[32];
        size_t cl = (size_t)(c1 - line) < 31 ? (size_t)(c1 - line) : 31;
        memcpy(cnt, line, cl); cnt[cl] = 0;
        // trim spaces
        char *cp = cnt;
        while (*cp == ' ') cp++;
        char *ep = cp + strlen(cp);
        while (ep > cp && ep[-1] == ' ') *--ep = 0;
        if (strcmp(cp, "-") == 0 || strcmp(cp, "$") == 0) continue;
        int lno = atoi(c1 + 1);
        if (lno <= 0) continue;
        int is_hit;
        if (strcmp(cp, "#####") == 0 || strcmp(cp, "=====") == 0) is_hit = 0;
        else if (cp[0] >= '0' && cp[0] <= '9') is_hit = 1;
        else continue;
        exec++;
        if (is_hit) { hit++; open_from = -1; prev = -2; }
        else {
            if (open_from == -1 || lno != prev + 1) {
                if (nr < 24) { ranges[nr][0] = lno; ranges[nr][1] = lno; nr++; }
                open_from = lno;
            } else if (nr > 0) ranges[nr - 1][1] = lno;
            prev = lno;
        }
    }
    fclose(f);
    *exec_out = exec;
    *hit_out = hit;
    if (nr == 0) { snprintf(uncov, uncov_sz, "none"); return 1; }
    int pos = 0;
    for (int i = 0; i < nr; i++) {
        if (ranges[i][0] == ranges[i][1])
            pos += snprintf(uncov + pos, uncov_sz - pos, "%s%d", i ? ", " : "", ranges[i][0]);
        else
            pos += snprintf(uncov + pos, uncov_sz - pos, "%s%d-%d", i ? ", " : "", ranges[i][0], ranges[i][1]);
    }
    return 1;
}

static int cmd_test(int argc, char *argv[]) {
    const char *root = ".";
    int bench = 0, race = 0, cover = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--bench") == 0) bench = 1;
        else if (strcmp(argv[i], "--race") == 0) race = 1;
        else if (strcmp(argv[i], "--cover") == 0) cover = 1;
        else if (strcmp(argv[i], "-v") == 0) g_verbose = 1;
        else root = argv[i];
    }
    char root_abs[PATH_MAX];
    if (!realpath(root, root_abs)) {
        fprintf(stderr, "come test: no such directory: %s\n", root);
        return 1;
    }
    g_test_count = 0;
    discover_tests(root_abs, 0);
    if (g_test_count == 0) {
        fprintf(stderr, "come test: no tests found under %s (looked for .co files in t/ directories)\n", root);
        return 1;
    }
    qsort(g_test_paths, g_test_count, PATH_MAX, cmp_str);

    // Instrumented builds go through COME_CFLAGS (read by the
    // child builds at compile and link time). The per-test
    // scratch working directories keep their objects and
    // coverage data out of the project's own build tree.
    if (race && cover) setenv("COME_CFLAGS", "-fsanitize=thread --coverage -g", 1);
    else if (race) setenv("COME_CFLAGS", "-fsanitize=thread -g", 1);
    else if (cover) setenv("COME_CFLAGS", "--coverage", 1);
    // Each child build runs from a scratch working
    // directory (cache isolation); anchor its src/ module
    // lookup on the tested project instead (COIL-150).
    setenv("COME_PROJECT_ROOT", root_abs, 1);
    if (race && system("command -v setarch > /dev/null 2>&1") == 0) g_race_wrap = 1;

    char self[PATH_MAX], scratch[PATH_MAX];
    self_exe(self, sizeof(self));
    if (!make_scratch(scratch, sizeof(scratch), "test")) die("cannot create a temp dir");

    int passed = 0, failed = 0;
    long tot_exec = 0, tot_hit = 0;
    printf("Running %d test%s from %s\n", g_test_count, g_test_count == 1 ? "" : "s", root);
    for (int i = 0; i < g_test_count; i++) {
        const char *path = g_test_paths[i];
        const char *disp = path;
        if (strncmp(path, root_abs, strlen(root_abs)) == 0) {
            disp = path + strlen(root_abs);
            while (*disp == '/') disp++;
        }
        char work[PATH_MAX];
        snprintf(work, sizeof(work), "%s/t%d", scratch, i);
        ensure_dir(work);
        char bin[PATH_MAX];
        // Build with the scratch dir as the child's working
        // directory: the driver roots its caches there.
        char qi[PATH_MAX + 8], qb[PATH_MAX + 8], qw[PATH_MAX + 8], cmd[16384], buildlog[PATH_MAX];
        snprintf(bin, sizeof(bin), "%s/prog", work);
        snprintf(buildlog, sizeof(buildlog), "%s/build.log", work);
        shell_quote(path, qi, sizeof(qi));
        shell_quote(bin, qb, sizeof(qb));
        shell_quote(work, qw, sizeof(qw));
        snprintf(cmd, sizeof(cmd), "cd %s && \"%s\" build %s -o %s > build.log 2>&1",
                 qw, self, qi, qb);
        int brc = system(cmd);
        if (brc == -1 || !WIFEXITED(brc) || WEXITSTATUS(brc) != 0) {
            printf("FAIL %s (build failed)\n", disp);
            dump_log(buildlog, 40);
            failed++;
            continue;
        }
        char runlog[PATH_MAX];
        snprintf(runlog, sizeof(runlog), "%s/run.log", work);
        double ms = 0.0;
        int runs = bench ? 5 : 1;
        double total_ms = 0.0;
        int rc = 0;
        for (int r = 0; r < runs; r++) {
            rc = run_capture(bin, runlog, TEST_TIMEOUT_S, &ms);
            total_ms += ms;
            if (rc != 0) break;
        }
        if (rc != 0) {
            const char *why = "failed";
            char whybuf[64];
            if (rc == -1000) { snprintf(whybuf, sizeof(whybuf), "timeout after %ds", TEST_TIMEOUT_S); why = whybuf; }
            else if (rc < 0) { snprintf(whybuf, sizeof(whybuf), "killed by signal %d", -rc); why = whybuf; }
            else { snprintf(whybuf, sizeof(whybuf), "exit %d", rc); why = whybuf; }
            printf("FAIL %s (%s)\n", disp, why);
            dump_log(runlog, 40);
            failed++;
            continue;
        }
        if (race && file_contains(runlog, "ThreadSanitizer")) {
            printf("FAIL %s (data race reported)\n", disp);
            dump_log(runlog, 40);
            failed++;
            continue;
        }
        passed++;
        if (bench) printf("PASS %s (avg %.1f ms over %d runs)\n", disp, total_ms / runs, runs);
        else printf("PASS %s (%.0f ms)\n", disp, total_ms);
        if (cover) {
            long ex = 0, ht = 0;
            char uncov[512];
            if (test_coverage(work, path, &ex, &ht, uncov, sizeof(uncov)) && ex > 0) {
                tot_exec += ex; tot_hit += ht;
                printf("  coverage: %.0f%% (%ld/%ld lines); uncovered: %s\n",
                       100.0 * ht / ex, ht, ex, uncov);
            } else {
                printf("  coverage: n/a\n");
            }
        }
    }
    printf("Results: %d passed, %d failed\n", passed, failed);
    if (cover && tot_exec > 0)
        printf("Line coverage: %.0f%% (%ld/%ld lines)\n", 100.0 * tot_hit / tot_exec, tot_hit, tot_exec);
    rm_rf(scratch);
    return failed ? 1 : 0;
}

/* ----- come fmt: canonical formatting, in place -----
 *
 * The parser has line-sensitive rules (the postfix
 * same-line rule, the line-leading &(struct T) rule), so
 * fmt never adds or removes a line: each source line maps
 * to exactly one output line. Within a line it canonicalizes
 * the leading indentation (4 spaces per brace level, case
 * bodies one extra) and the spacing between tokens. String
 * and char literals and comments pass through byte-for-byte;
 * only code tokens are re-spaced. Formatting an
 * already-formatted file changes nothing (idempotent).
 */

enum { FK_WORD, FK_STR, FK_COMMENT, FK_OP };

typedef struct { char text[512]; int kind; int prefix; } FTok;

static const char *fmt_ops[] = {
    "<<=", ">>=", "...", "==", "!=", "<=", ">=", "&&", "||",
    "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
    "<<", ">>", "->", NULL
};

static int fmt_is_keyword_paren(const char *w) {
    return strcmp(w, "if") == 0 || strcmp(w, "for") == 0 ||
           strcmp(w, "while") == 0 || strcmp(w, "switch") == 0;
}

// Whether a "{" in this position opens a block (function
// body, if/for/while/switch body, else/do body) rather than
// a literal or a type. Decided by the preceding token (or
// line start): ")" ends a header, else/do introduce blocks.
static int fmt_brace_is_block(const FTok *prev, int have_prev) {
    if (!have_prev) return 1;
    if (prev->kind == FK_OP && strcmp(prev->text, ")") == 0) return 1;
    if (prev->kind == FK_WORD &&
        (strcmp(prev->text, "else") == 0 || strcmp(prev->text, "do") == 0)) return 1;
    if (prev->kind == FK_OP && strcmp(prev->text, "}") == 0) return 1;
    return 0;
}

// Space between token a (already emitted; a_prefix says a was
// a prefix unary) and token b (b_prefix says b is one)?
// a_block says a is a block-opening "{"; b_block says b, when
// a "{", opens a block.
static int fmt_space(const FTok *a, int a_prefix, int a_block,
                     const FTok *b, int b_prefix, int b_block, int b_close_block) {
    const char *x = a->text, *y = b->text;
    if (b->kind == FK_OP) {
        if (strcmp(y, ")") == 0 || strcmp(y, "]") == 0 ||
            strcmp(y, ",") == 0 || strcmp(y, ";") == 0 ||
            strcmp(y, ".") == 0 || strcmp(y, ":") == 0) return 0;
        if (strcmp(y, "(") == 0)
            return (a->kind == FK_WORD && fmt_is_keyword_paren(x)) ? 1 : 0;
        if (strcmp(y, "[") == 0) return 0;
        if (strcmp(y, "{") == 0) {
            if (strcmp(x, "map") == 0 || strcmp(x, "(") == 0) return 0;
            return 1;
        }
        if (strcmp(y, "}") == 0) {
            if (strcmp(x, "{") == 0) return 0;        // {}
            return b_close_block ? 1 : 0;             // block: " }", literal: "}"
        }
        if ((strcmp(y, "++") == 0 || strcmp(y, "--") == 0) && !b_prefix) return 0;
        if (b_prefix && (strcmp(x, "(") == 0 || strcmp(x, "[") == 0 ||
                         strcmp(x, "{") == 0)) return 0;
        return 1;  // binary operators, ?, and the rest
    }
    if (a->kind == FK_OP) {
        if (strcmp(x, "(") == 0 || strcmp(x, "[") == 0 ||
            strcmp(x, ".") == 0) return 0;
        if (strcmp(x, "{") == 0) {
            if (strcmp(y, "}") == 0) return 0;      // {}
            return a_block ? 1 : 0;
        }
        if (a_prefix) return 0;
        return 1;  // after , ; : and binary operators
    }
    return 1;      // word/word, word/literal, literal/word
}

typedef struct {
    int in_block_comment;
    int depth;        // brace nesting before the current line
    int case_extra;   // inside a case body
    int case_depth;   // the depth at which the case body runs
    int brace_kind[128];  // per open brace: 1 = block, 0 = literal/type
    int brace_top;
} FmtState;

// Format one line into out (no trailing newline). Updates st.
static void fmt_line(const char *line, FmtState *st, char *out, size_t out_sz) {
    // --- tokenize ---
    static FTok toks[1024];
    int nt = 0;
    int began_in_block = st->in_block_comment;
    const char *trail_comment = NULL;
    int i = 0, n = (int)strlen(line);
    while (i < n) {
        char c = line[i];
        if (st->in_block_comment) {
            int start = i;
            while (i < n && !(line[i] == '*' && i + 1 < n && line[i + 1] == '/')) i++;
            if (i < n) { i += 2; st->in_block_comment = 0; }
            if (nt < 1024) {
                int len = i - start < 511 ? i - start : 511;
                memcpy(toks[nt].text, line + start, len);
                toks[nt].text[len] = 0; toks[nt].kind = FK_COMMENT; toks[nt].prefix = 0; nt++;
            }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '/' && i + 1 < n && line[i + 1] == '/') { trail_comment = line + i; break; }
        if (c == '/' && i + 1 < n && line[i + 1] == '*') {
            int start = i;
            i += 2;
            while (i < n && !(line[i] == '*' && i + 1 < n && line[i + 1] == '/')) i++;
            if (i < n) i += 2; else st->in_block_comment = 1;
            if (nt < 1024) {
                int len = i - start < 511 ? i - start : 511;
                memcpy(toks[nt].text, line + start, len);
                toks[nt].text[len] = 0; toks[nt].kind = FK_COMMENT; toks[nt].prefix = 0; nt++;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            int start = i;
            i++;
            while (i < n && line[i] != c) { if (line[i] == '\\' && i + 1 < n) i++; i++; }
            if (i < n) i++;
            if (nt < 1024) {
                int len = i - start < 511 ? i - start : 511;
                memcpy(toks[nt].text, line + start, len);
                toks[nt].text[len] = 0; toks[nt].kind = FK_STR; toks[nt].prefix = 0; nt++;
            }
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            int start = i;
            while (i < n && ((line[i] >= 'a' && line[i] <= 'z') || (line[i] >= 'A' && line[i] <= 'Z') ||
                             (line[i] >= '0' && line[i] <= '9') || line[i] == '_')) i++;
            if (nt < 1024) {
                int len = i - start < 511 ? i - start : 511;
                memcpy(toks[nt].text, line + start, len);
                toks[nt].text[len] = 0; toks[nt].kind = FK_WORD; toks[nt].prefix = 0; nt++;
            }
            continue;
        }
        if (c >= '0' && c <= '9') {
            int start = i;
            // Come numbers may carry digit separators
            // (10'000'000): an apostrophe followed by a
            // digit belongs to the number, it does not
            // open a character literal.
            while (i < n && ((line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'z') ||
                             (line[i] >= 'A' && line[i] <= 'Z') || line[i] == '.' || line[i] == '_' ||
                             (line[i] == '\'' && i + 1 < n && line[i + 1] >= '0' && line[i + 1] <= '9'))) i++;
            if (nt < 1024) {
                int len = i - start < 511 ? i - start : 511;
                memcpy(toks[nt].text, line + start, len);
                toks[nt].text[len] = 0; toks[nt].kind = FK_WORD; toks[nt].prefix = 0; nt++;
            }
            continue;
        }
        // operators and punctuation, longest match first
        int matched = 0;
        for (int k = 0; fmt_ops[k]; k++) {
            size_t L = strlen(fmt_ops[k]);
            if (strncmp(line + i, fmt_ops[k], L) == 0) {
                if (nt < 1024) {
                    snprintf(toks[nt].text, sizeof(toks[nt].text), "%s", fmt_ops[k]);
                    toks[nt].kind = FK_OP; toks[nt].prefix = 0; nt++;
                }
                i += (int)L; matched = 1; break;
            }
        }
        if (matched) continue;
        if (strchr("+-*/%=<>&|^~!?:;,.()[]{}", c)) {
            if (nt < 1024) {
                toks[nt].text[0] = c; toks[nt].text[1] = 0;
                toks[nt].kind = FK_OP; toks[nt].prefix = 0; nt++;
            }
            i++;
            continue;
        }
        i++;  // unknown char: skip (invalid Come anyway)
    }

    // A line wholly inside a block comment keeps its bytes.
    if (began_in_block && nt == 1 && toks[0].kind == FK_COMMENT && !trail_comment) {
        snprintf(out, out_sz, "%s", line);
        // rtrim
        size_t L = strlen(out);
        while (L > 0 && (out[L - 1] == ' ' || out[L - 1] == '\t' || out[L - 1] == '\r')) out[--L] = 0;
        return;
    }

    // --- classify prefix operators ---
    for (int k = 0; k < nt; k++) {
        if (toks[k].kind != FK_OP) continue;
        const char *t = toks[k].text;
        if (strcmp(t, "!") && strcmp(t, "~") && strcmp(t, "-") &&
            strcmp(t, "&") && strcmp(t, "*") && strcmp(t, "++") && strcmp(t, "--"))
            continue;
        int prefix = 0;
        if (k == 0) prefix = 1;
        else {
            const FTok *pv = &toks[k - 1];
            if (pv->kind == FK_OP && strcmp(pv->text, ")") && strcmp(pv->text, "]") &&
                !(pv->prefix && (strcmp(pv->text, "++") == 0 || strcmp(pv->text, "--") == 0)))
                prefix = 1;
            else if (pv->kind == FK_WORD &&
                     (strcmp(pv->text, "return") == 0 || strcmp(pv->text, "case") == 0))
                prefix = 1;
        }
        toks[k].prefix = prefix;
    }

    // --- indentation level for this line ---
    int level = st->depth;
    int first_close = (nt > 0 && toks[0].kind == FK_OP && strcmp(toks[0].text, "}") == 0);
    int first_case = (nt > 0 && toks[0].kind == FK_WORD &&
                      (strcmp(toks[0].text, "case") == 0 || strcmp(toks[0].text, "default") == 0));
    if (first_close) level = st->depth - 1;
    else if (!first_case) level = st->depth + st->case_extra;
    if (level < 0) level = 0;

    // --- emit ---
    // Brace blockness is decided left to right on a scratch
    // copy of the kind stack; the real stack is updated in
    // the state pass below.
    int kind_scratch[1100];
    int kstop = st->brace_top;
    for (int k = 0; k < kstop && k < 1100; k++) kind_scratch[k] = st->brace_kind[k];
    int pos = 0;
    for (int k = 0; k < level * 4 && pos < (int)out_sz - 1; k++) out[pos++] = ' ';
    out[pos] = 0;
    for (int k = 0; k < nt; k++) {
        int b_block = 0, a_block = 0, b_close_block = 0;
        if (toks[k].kind == FK_OP && strcmp(toks[k].text, "{") == 0)
            b_block = fmt_brace_is_block(k > 0 ? &toks[k - 1] : NULL, k > 0);
        if (toks[k].kind == FK_OP && strcmp(toks[k].text, "}") == 0)
            b_close_block = (kstop > 0) ? kind_scratch[kstop - 1] : 0;
        if (k > 0 && toks[k - 1].kind == FK_OP && strcmp(toks[k - 1].text, "{") == 0)
            a_block = (kstop > 0) ? kind_scratch[kstop - 1] : 0;
        if (k > 0) {
            int sp = fmt_space(&toks[k - 1], toks[k - 1].prefix, a_block,
                               &toks[k], toks[k].prefix, b_block, b_close_block);
            if (sp && pos < (int)out_sz - 1) out[pos++] = ' ';
            out[pos] = 0;
        }
        int w = snprintf(out + pos, out_sz - pos, "%s", toks[k].text);
        if (w > 0) pos += w;
        if (toks[k].kind == FK_OP && strcmp(toks[k].text, "{") == 0) {
            if (kstop < 1100) kind_scratch[kstop++] = b_block;
        } else if (toks[k].kind == FK_OP && strcmp(toks[k].text, "}") == 0) {
            if (kstop > 0) kstop--;
        }
    }
    if (trail_comment) {
        char tc[1024];
        snprintf(tc, sizeof(tc), "%s", trail_comment);
        size_t L = strlen(tc);
        while (L > 0 && (tc[L - 1] == ' ' || tc[L - 1] == '\t' || tc[L - 1] == '\r')) tc[--L] = 0;
        if (nt > 0) pos += snprintf(out + pos, out_sz - pos, "  %s", tc);
        else {
            // comment-only line: it sits at the current level
            pos = 0;
            for (int k = 0; k < level * 4 && pos < (int)out_sz - 1; k++) out[pos++] = ' ';
            out[pos] = 0;
            pos += snprintf(out + pos, out_sz - pos, "%s", tc);
        }
    }
    out[pos] = 0;

    // --- update state ---
    for (int k = 0; k < nt; k++) {
        if (toks[k].kind != FK_OP) continue;
        if (strcmp(toks[k].text, "{") == 0) {
            st->depth++;
            if (st->brace_top < 128)
                st->brace_kind[st->brace_top++] =
                    fmt_brace_is_block(k > 0 ? &toks[k - 1] : NULL, k > 0);
        } else if (strcmp(toks[k].text, "}") == 0) {
            st->depth--;
            if (st->brace_top > 0) st->brace_top--;
        }
    }
    if (st->depth < 0) st->depth = 0;
    if (first_case) { st->case_extra = 1; st->case_depth = level + 1; }
    else if (first_close) st->case_extra = 0;
    // Leaving the switch (depth back below the case body)
    // ends the case body's extra indentation.
    if (st->case_extra && st->depth + st->case_extra < st->case_depth) st->case_extra = 0;
}

// Format one file in place; returns 1 when it changed.
static int fmt_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "fmt: cannot read %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc(sz + 2);
    size_t rd = fread(src, 1, sz, f);
    (void)rd;
    fclose(f);
    src[sz] = 0;

    // Output bound: reformatting can add spaces but never
    // more than ~2 per source char plus indentation.
    size_t cap = (size_t)sz * 3 + 65536;
    char *dst = malloc(cap);
    size_t dpos = 0;
    FmtState st = { 0, 0, 0 };
    // Split manually: strtok would collapse consecutive
    // newlines and drop blank lines, renumbering the file -
    // fmt must preserve the line structure exactly.
    {
        const char *p = src;
        static char lbuf[65536];
        static char obuf[65536];
        while (1) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (!nl && len == 0) break;
            if (len >= sizeof(lbuf)) len = sizeof(lbuf) - 1;
            memcpy(lbuf, p, len); lbuf[len] = 0;
            fmt_line(lbuf, &st, obuf, sizeof(obuf));
            size_t L = strlen(obuf);
            memcpy(dst + dpos, obuf, L); dpos += L;
            dst[dpos++] = '\n';
            if (!nl) break;
            p = nl + 1;
        }
    }
    dst[dpos] = 0;
    int changed = (strcmp(src, dst) != 0);
    if (changed) {
        FILE *w = fopen(path, "wb");
        if (!w) { fprintf(stderr, "fmt: cannot write %s\n", path); free(src); free(dst); return -1; }
        fwrite(dst, 1, dpos, w);
        fclose(w);
        printf("fmt: %s\n", path);
    }
    free(src); free(dst);
    return changed;
}

static void fmt_collect(const char *path, char list[][PATH_MAX], int *count, int max) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "fmt: no such file or directory: %s\n", path);
        return;
    }
    if (S_ISREG(st.st_mode)) {
        if (*count < max) snprintf(list[(*count)++], PATH_MAX, "%s", path);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            if (strcmp(e->d_name, "build") == 0) continue;
            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            struct stat cst;
            if (stat(child, &cst) != 0) continue;
            if (S_ISDIR(cst.st_mode)) fmt_collect(child, list, count, max);
            else if (S_ISREG(cst.st_mode)) {
                size_t L = strlen(child);
                if (L > 3 && strcmp(child + L - 3, ".co") == 0 && *count < max)
                    snprintf(list[(*count)++], PATH_MAX, "%s", child);
            }
        }
        closedir(d);
    }
}

static int cmd_fmt(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: come fmt <file.co|dir> [...]\n");
        return 1;
    }
    static char list[4096][PATH_MAX];
    int count = 0;
    for (int i = 2; i < argc; i++) fmt_collect(argv[i], list, &count, 4096);
    qsort(list, count, PATH_MAX, cmp_str);
    int changed = 0;
    for (int i = 0; i < count; i++) {
        int r = fmt_file(list[i]);
        if (r < 0) return 1;
        if (r > 0) changed++;
    }
    if (changed == 0) printf("fmt: all files already formatted\n");
    return 0;
}

/* ---------- Main ---------- */

static void print_usage(void) {
    fprintf(stderr,
        "Usage:\n"
        "  come build <file.co|dir> [-o output]   compile and link\n"
        "    [--single-tu]                        compile all user code as one TU\n"
        "                                          (cross-module inlining without LTO)\n"
        "  come genc <file.co> -o <out.c>          emit C only\n"
        "  come check <file.co|dir>                compile, no link or run\n"
        "  come run <file.co> [--] [args...]       build to a temp binary and run it\n"
        "  come test [root] [--cover] [--race] [--bench]\n"
        "                                          build and run the .co tests in t/ dirs\n"
        "  come fmt <file.co|dir> [...]            reformat sources in place\n"
        "  come escapes <file.co|dir>              dump may-escape parameters\n");
}

/* ----- come escapes: dump the may-escape classification ----- */
// Design 1 (redesign): parses a file, or a directory
// module's .co files, runs the module analysis
// (frameless, mayfail, escapes), and prints one line
// per buffer-typed parameter: "module.fn(param):
// escapes|transient". No code is generated and nothing
// is linked, so no C toolchain is needed. Imports are
// not resolved: cross-module calls escape their
// identifier arguments (the analysis's safe default).
static int cmd_escapes(int argc, char *argv[]) {
    const char *input = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) g_verbose = 1;
        else input = argv[i];
    }
    if (!input) {
        fprintf(stderr, "Usage: come escapes <file.co|dir>\n");
        return 1;
    }
    struct stat st;
    if (stat(input, &st) == 0 && S_ISDIR(st.st_mode)) {
        char (*paths)[PATH_MAX] = malloc(64 * sizeof(*paths));
        int n = scan_co_dir(input, paths, 64);
        if (n == 0) {
            fprintf(stderr, "No .co files in %s\n", input);
            free(paths);
            return 1;
        }
        ASTNode *asts[64];
        const char *files[64];
        int ast_count = 0;
        char module[128] = {0};
        for (int i = 0; i < n && ast_count < 64; i++) {
            ASTNode *ast = NULL;
            if (parse_file(paths[i], &ast) != 0 || !ast) {
                fprintf(stderr, "Parsing failed: %s\n", paths[i]);
                for (int j = 0; j < ast_count; j++) ast_free(asts[j]);
                free(paths);
                return 1;
            }
            asts[ast_count] = ast;
            files[ast_count] = paths[i];
            ast_count++;
            if (ast->type == AST_PROGRAM && ast->text[0] && !module[0])
                snprintf(module, sizeof(module), "%s", ast->text);
        }
        if (!module[0]) {
            char tmp[PATH_MAX];
            snprintf(tmp, sizeof(tmp), "%s", input);
            char *bn = basename(tmp);
            snprintf(module, sizeof(module), "%s", bn);
        }
        analyze_module(module, asts, files, ast_count);
        codegen_dump_escapes(stdout);
        for (int i = 0; i < ast_count; i++) ast_free(asts[i]);
        free(paths);
        return 0;
    }
    ASTNode *ast = NULL;
    if (parse_file(input, &ast) != 0 || !ast) {
        fprintf(stderr, "Parsing failed: %s\n", input);
        return 1;
    }
    char modbuf[PATH_MAX];
    const char *mod = NULL;
    if (ast->type == AST_PROGRAM && ast->text[0]) {
        mod = ast->text;
    } else {
        snprintf(modbuf, sizeof(modbuf), "%s", input);
        char *bn = basename(modbuf);
        char *dot = strrchr(bn, '.');
        if (dot) *dot = 0;
        mod = bn;
    }
    ASTNode *one = ast;
    const char *one_file = input;
    analyze_module(mod, &one, &one_file, 1);
    codegen_dump_escapes(stdout);
    ast_free(ast);
    return 0;
}

// Design 6 (redesign): concatenate every user .c file
// into one translation unit, in link order (so global
// initialization order matches a separate build).
// Two constructs cannot simply be repeated in one TU:
//  - come_strerror, a static helper codegen emits in
//    every file (one line, identical everywhere):
//    later copies are skipped.
//  - struct definitions, which the exported surface
//    repeats for every importer: an identical
//    definition (same tag, fields, attributes,
//    compared whitespace-insensitively) is skipped; a
//    DIFFERENT definition of the same tag is refused
//    loudly, because struct tags are plain C names,
//    not module-mangled, so two modules defining e.g.
//    struct Point with different layouts cannot share
//    a TU. Prototypes and typedefs may repeat
//    (identical declarations are legal C) and pass
//    through untouched.
static void build_single_tu(char *out_path, size_t out_size) {
    snprintf(out_path, out_size, "%s/single_tu.c", g_build_dir);
    FILE *out = fopen(out_path, "w");
    if (!out) die("Cannot write single-TU file: %s", out_path);

    // Seen struct definitions, keyed by tag (named
    // structs) or typedef name (anonymous structs).
    char *seen_key[1024];
    char *seen_norm[1024];
    char *seen_file[1024];
    int nseen = 0;
    int seen_strerror = 0;

    StringList *node = g_c_files;
    while (node) {
        FILE *in = fopen(node->value, "r");
        if (!in) die("Single-TU: cannot read %s", node->value);
        char *line = NULL;
        size_t cap = 0;
        ssize_t n;
        while ((n = getline(&line, &cap, in)) != -1) {
            const char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            // come_strerror: skip later identical copies.
            // (Codegen emits it as one line; if that ever
            // changes, the build fails loudly below, not
            // silently.)
            if (strncmp(p, "static", 6) == 0 && strstr(line, "come_strerror") && strchr(line, '{')) {
                if (!seen_strerror) {
                    seen_strerror = 1;
                    fwrite(line, 1, (size_t)n, out);
                }
                continue;
            }
            // Struct definition opening, precisely: after
            // "struct" or "typedef struct", optional
            // __attribute__s, the next token is either
            // '{' (anonymous) or a tag followed by '{'.
            // This excludes functions returning structs
            // (tag followed by a name, then '(') and
            // variables of struct type (tag followed by
            // a name, then '='), and it covers
            // single-line definitions (tuple returns are
            // "typedef struct { ... } Name;" on one line)
            // as well as multi-line ones.
            int is_struct = 0;
            {
                const char *q = p;
                if (strncmp(q, "typedef struct", 14) == 0 && (q[14] == ' ' || q[14] == '\t')) q += 14;
                else if (strncmp(q, "struct", 6) == 0 && (q[6] == ' ' || q[6] == '\t')) q += 6;
                else q = NULL;
                if (q) {
                    while (*q == ' ' || *q == '\t') q++;
                    while (strncmp(q, "__attribute__", 13) == 0) {
                        q += 13;
                        while (*q == ' ' || *q == '\t') q++;
                        if (*q == '(') {
                            int ad = 0;
                            while (*q) {
                                if (*q == '(') ad++;
                                else if (*q == ')') { ad--; if (ad == 0) { q++; break; } }
                                q++;
                            }
                        }
                        while (*q == ' ' || *q == '\t') q++;
                    }
                    if (*q == '{') {
                        is_struct = 1;
                    } else if ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || *q == '_') {
                        while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '_') q++;
                        while (*q == ' ' || *q == '\t') q++;
                        if (*q == '{') is_struct = 1;
                    }
                }
            }
            if (is_struct) {
                    // Capture the whole block (brace count;
                    // struct fields never contain braces).
                    char *block = NULL;
                    size_t blen = 0, bcap = 0;
                    int depth = 0;
                    for (ssize_t k = 0; k < n; k++) {
                        if (line[k] == '{') depth++;
                        else if (line[k] == '}') depth--;
                    }
                    if (blen + (size_t)n + 1 > bcap) {
                        bcap = (blen + (size_t)n + 1) * 2;
                        block = realloc(block, bcap);
                    }
                    memcpy(block + blen, line, (size_t)n);
                    blen += (size_t)n;
                    block[blen] = 0;
                    while (depth > 0) {
                        n = getline(&line, &cap, in);
                        if (n == -1) die("Single-TU: unterminated struct in %s", node->value);
                        for (ssize_t k = 0; k < n; k++) {
                            if (line[k] == '{') depth++;
                            else if (line[k] == '}') depth--;
                        }
                        if (blen + (size_t)n + 1 > bcap) {
                            bcap = (blen + (size_t)n + 1) * 2;
                            block = realloc(block, bcap);
                        }
                        memcpy(block + blen, line, (size_t)n);
                        blen += (size_t)n;
                        block[blen] = 0;
                    }
                    // Tag: last token before the first '{'
                    // in the first line. If it is "struct"
                    // itself, this is an anonymous struct.
                    char *first_nl = strchr(block, '\n');
                    size_t flen = first_nl ? (size_t)(first_nl - block) : blen;
                    ssize_t brace = -1;
                    for (size_t k = 0; k < flen; k++) {
                        if (block[k] == '{') { brace = (ssize_t)k; break; }
                    }
                    ssize_t te = brace;
                    while (te > 0 && (block[te-1] == ' ' || block[te-1] == '\t')) te--;
                    ssize_t ts = te;
                    while (ts > 0 && block[ts-1] != ' ' && block[ts-1] != '\t') ts--;
                    int anonymous = (te - ts == 6 && strncmp(block + ts, "struct", 6) == 0);
                    char tagbuf[256];
                    if (!anonymous) {
                        size_t tl = (size_t)(te - ts);
                        if (tl >= sizeof(tagbuf)) die("Single-TU: struct tag too long in %s", node->value);
                        memcpy(tagbuf, block + ts, tl);
                        tagbuf[tl] = 0;
                    }
                    // Typedef name: token after the last
                    // '}' in the block (closing line), if
                    // any; NULL for plain structs.
                    ssize_t last_brace = -1;
                    for (size_t k = 0; k < blen; k++) {
                        if (block[k] == '}') last_brace = (ssize_t)k;
                    }
                    char tdbuf[256];
                    char *tdname = NULL;
                    if (last_brace >= 0) {
                        ssize_t q = last_brace + 1;
                        while (q < (ssize_t)blen && (block[q] == ' ' || block[q] == '\t')) q++;
                        ssize_t qs = q;
                        while (q < (ssize_t)blen && block[q] != ';' && block[q] != ' ' && block[q] != '\t' && block[q] != '\n' && block[q] != '\r') q++;
                        if (q > qs) {
                            size_t tl = (size_t)(q - qs);
                            if (tl >= sizeof(tdbuf)) die("Single-TU: typedef name too long in %s", node->value);
                            memcpy(tdbuf, block + qs, tl);
                            tdbuf[tl] = 0;
                            tdname = tdbuf;
                        }
                    }
                    // Key: tag for named structs, typedef
                    // name for anonymous ones.
                    const char *key;
                    if (anonymous) {
                        if (!tdname) die("Single-TU: anonymous struct without typedef in %s", node->value);
                        key = tdname;
                    } else {
                        key = tagbuf;
                    }
                    // Body for comparison: block through
                    // the closing '}', excluding any
                    // typedef name, whitespace-stripped.
                    // (So the same struct body typedef'd
                    // under two names still dedups.)
                    size_t body_len = (size_t)(last_brace + 1);
                    char *norm = malloc(body_len + 1);
                    size_t nl = 0;
                    for (size_t k = 0; k < body_len; k++) {
                        char c = block[k];
                        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\v' && c != '\f') norm[nl++] = c;
                    }
                    norm[nl] = 0;
                    int found = -1;
                    for (int i = 0; i < nseen; i++) {
                        if (strcmp(seen_key[i], key) == 0) { found = i; break; }
                    }
                    if (found >= 0) {
                        if (strcmp(seen_norm[found], norm) == 0) {
                            // Identical body: skip, but a
                            // named typedef-struct still
                            // needs its typedef name
                            // defined (forward typedef
                            // to the already-defined tag).
                            if (!anonymous && tdname) {
                                fprintf(out, "typedef struct %s %s;\n", tagbuf, tdname);
                            }
                            free(block);
                            free(norm);
                        } else {
                            die("Single-TU refused: struct/typedef '%s' is defined differently in %s and %s (plain struct tags collide across modules; use distinct struct names, or build without --single-tu)",
                                key, seen_file[found], node->value);
                        }
                    } else {
                        if (nseen >= 1024) die("Single-TU: too many struct definitions");
                        seen_key[nseen] = strdup(key);
                        seen_norm[nseen] = norm;
                        seen_file[nseen] = strdup(node->value);
                        nseen++;
                        fwrite(block, 1, blen, out);
                        free(block);
                    }
                    continue;
            }
            fwrite(line, 1, (size_t)n, out);
        }
        free(line);
        fclose(in);
        node = node->next;
    }
    for (int i = 0; i < nseen; i++) {
        free(seen_key[i]);
        free(seen_norm[i]);
        free(seen_file[i]);
    }
    fclose(out);
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);

    // Developer commands (COIL-138). fmt needs no C toolchain.
    if (argc >= 2 && strcmp(argv[1], "fmt") == 0) return cmd_fmt(argc, argv);

    // escapes is pure analysis: parse and classify, no C toolchain.
    if (argc >= 2 && strcmp(argv[1], "escapes") == 0) return cmd_escapes(argc, argv);

    // Ensure we have gcc before doing anything else
    check_build_essentials();

    if (argc >= 2 && strcmp(argv[1], "check") == 0) return cmd_check(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "run") == 0) return cmd_run(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "test") == 0) return cmd_test(argc, argv);

    if (argc < 3) {
        print_usage();
        return 1;
    }

    const char *cmd = argv[1];
    if (strcmp(cmd, "build") != 0 && strcmp(cmd, "genc") != 0) {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        print_usage();
        return 1;
    }
    
    int build_mode = (strcmp(cmd, "build") == 0);

    const char *input = NULL;
    const char *output = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 < argc) output = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "--single-tu") == 0) {
            g_single_tu = 1;
        } else {
            input = argv[i];
        }
    }

    if (!input) die("No input file specified");

    // Setup Directries
    detect_project_root(g_project_root, sizeof(g_project_root));
    // The src/ anchor defaults to the working directory;
    // COME_PROJECT_ROOT overrides it (set by `come test` for
    // its scratch builds - COIL-150). Caches and outputs
    // stay rooted at the working directory either way.
    const char *proot = getenv("COME_PROJECT_ROOT");
    if (!proot || !*proot || !realpath(proot, g_src_root))
        snprintf(g_src_root, sizeof(g_src_root), "%s", g_project_root);

    // Only setup ccache/build if generic build. 
    // If output is forced in genc, we might not need them, but setup anyway.
    snprintf(g_ccache_dir, sizeof(g_ccache_dir), "%s/.ccache", g_project_root);
    ensure_dir(g_ccache_dir);
    
    snprintf(g_build_dir, sizeof(g_build_dir), "%s/build", g_project_root);
    
    if (build_mode) {
        ensure_dir(g_build_dir);
    }

    // Identify Entry Point
    char entry_file[PATH_MAX];
    if (strcmp(input, ".") == 0) {
        snprintf(entry_file, sizeof(entry_file), "%s/main.co", g_project_root);
        if (!file_exists(entry_file)) {
            die("No main.co found in %s", g_project_root);
        }
    } else {
        // Check if input is a directory
        struct stat st;
        if (stat(input, &st) == 0 && S_ISDIR(st.st_mode)) {
             snprintf(entry_file, sizeof(entry_file), "%s/main.co", input);
             if (!file_exists(entry_file)) {
                 die("No main.co found in directory %s", input);
             }
        } else {
             strcpy(entry_file, input);
        }
    }

    // Compile everything
    // Pass output if genc mode (forced output)
    compile_file(entry_file, (!build_mode && output) ? output : NULL, NULL);

    if (!build_mode) {
        printf("Genc finished.\n");
        return 0;
    }

    // Link
    char out_bin[PATH_MAX];
    if (output) {
        strcpy(out_bin, output);
    } else {
        // ... Logic for default name ...
        if (strcmp(input, ".") == 0) {
             // Use directory name as binary name
             // g_project_root dirname
             char tmp[PATH_MAX]; strcpy(tmp, g_project_root);
             char *p = basename(tmp);
             // if project root is dot, use 'main'
             if (strcmp(p, ".") == 0) strcpy(p, "main");
             snprintf(out_bin, sizeof(out_bin), "%s/build/%s", g_project_root, p);
        } else {
             // specific file
             char tmp[PATH_MAX]; strcpy(tmp, input);
             // remove extension
             char *last_dot = strrchr(tmp, '.');
             char *last_slash = strrchr(tmp, '/');
             if (last_dot && (!last_slash || last_dot > last_slash)) {
                 *last_dot = 0;
             }
             strcpy(out_bin, tmp);
        }
    }

    // Construct Link Command
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path)-1);
    if (len != -1) exe_path[len] = 0;
    char *exe_dir = dirname(exe_path);
    char *build_dir_name = basename(exe_dir); 
    char project_base[PATH_MAX];
    strcpy(project_base, exe_dir); 
    if (strcmp(build_dir_name, "build") == 0) {
         dirname(project_base);
    }


    char libcome[PATH_MAX];
    // Check installed lib location first: ../lib/libcome.a from bin
    snprintf(libcome, sizeof(libcome), "%s/../lib/libcome.a", exe_dir);
    if (!file_exists(libcome)) {
         // Fallback to dev layout
         snprintf(libcome, sizeof(libcome), "%s/lib/libcome.a", project_base);
    }
    int use_lib = file_exists(libcome);

    char link_cmd[65536];
    int pos;
    if (g_single_tu) {
        // Design 6 (redesign): compile all user code as
        // one translation unit, then link that object
        // with the runtime (which stays separate).
        char single_c[PATH_MAX];
        build_single_tu(single_c, sizeof(single_c));
        char single_o[PATH_MAX];
        snprintf(single_o, sizeof(single_o), "%s/single_tu.o", g_build_dir);
        char include_dir[PATH_MAX];
        snprintf(include_dir, sizeof(include_dir), "%s/../include", exe_dir);
        char check_file[PATH_MAX];
        snprintf(check_file, sizeof(check_file), "%s/come_string.h", include_dir);
        char cmd[65536];
        if (file_exists(check_file)) {
            snprintf(cmd, sizeof(cmd),
                "gcc -c -Wall -Wno-cpp -Wno-implicit-function-declaration -D__STDC_WANT_LIB_EXT1__=1 "
                "-I\"%s\" \"%s\" -o \"%s\"%s",
                include_dir, single_c, single_o, cc_extra_flags());
        } else {
            snprintf(cmd, sizeof(cmd),
                "gcc -c -Wall -Wno-cpp -Wno-implicit-function-declaration -D__STDC_WANT_LIB_EXT1__=1 "
                "-I%s/src/include -I%s/src/core/include \"%s\" -o \"%s\"%s",
                project_base, project_base, single_c, single_o, cc_extra_flags());
        }
        if (run_cmd(cmd) != 0) die("Single-TU compilation failed: %s", single_c);
        pos = snprintf(link_cmd, sizeof(link_cmd), "gcc -o \"%s\" \"%s\"", out_bin, single_o);
    } else {
        pos = snprintf(link_cmd, sizeof(link_cmd), "gcc -o \"%s\"", out_bin);

        // Add user objects
        StringList *node = g_object_files;
        while (node) {
            pos += snprintf(link_cmd + pos, sizeof(link_cmd) - pos, " \"%s\"", node->value);
            node = node->next;
        }
    }

    // Add std libs
    // In dev mode, we need specific .o files from the build dir of the compiler repo
    if (use_lib) {
        pos += snprintf(link_cmd + pos, sizeof(link_cmd) - pos, " \"%s\"", libcome);
    } else {
        const char *std_objs[] = {"std.o", "string.o", "array.o", "map.o", "memory.o", "coro.o", "link.o"};
        for (int i=0; i<7; i++) {
            pos += snprintf(link_cmd + pos, sizeof(link_cmd) - pos, " \"%s/build/%s\"", project_base, std_objs[i]);
        }
    }

    pos += snprintf(link_cmd + pos, sizeof(link_cmd) - pos, "%s -ldl", cc_extra_flags());

    if (run_cmd(link_cmd) != 0) {
        die("Linking failed");
    }

    printf("Built: %s\n", out_bin);
    
    list_free(g_object_files);
    list_free(g_c_files);
    list_free(g_visited_files);
    
    return 0;
}
