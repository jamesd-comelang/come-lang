// src/come_compiler.c
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
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
static char g_ccache_dir[PATH_MAX];
static char g_build_dir[PATH_MAX];

/* ---------- Data Structures ---------- */

typedef struct StringList {
    char *value;
    struct StringList *next;
} StringList;

static StringList *g_object_files = NULL;
static StringList *g_visited_files = NULL;

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
} ModuleGroup;

static void compile_file(const char *source_path, const char *forced_o_path,
                         const ModuleGroup *group);
static void compile_import(const char *import_name, const char *current_file);

/* ---------- Compilation ---------- */

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

    // 2. Scan for imports and recurse
    if (ast->type == AST_PROGRAM) {
        for (int i = 0; i < ast->child_count; i++) {
            if (ast->children[i] && ast->children[i]->type == AST_IMPORT) {
                char *import_name = ast->children[i]->text;
                if (strcmp(import_name, "std") == 0 || strcmp(import_name, "string") == 0 ||
                    strcmp(import_name, "array") == 0 || strcmp(import_name, "map") == 0) {
                    continue;
                }

                compile_import(import_name, abs_path);
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
    dirname(c_dir);
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

    int need_transpile = (t_src > t_c) || (t_exe > t_c);
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
        g_codegen_group_has_init = (grouped && group->is_primary) ? group->has_init : -1;
        g_codegen_group_has_exit = (grouped && group->is_primary) ? group->has_exit : -1;
        g_codegen_chain_imports = (grouped && group->is_primary) ? group->imports : NULL;
        g_codegen_chain_import_count = (grouped && group->is_primary) ? group->import_count : 0;
        // System module (COIL-111, spec 3.3): the source lives in
        // the compiler's own tree - <root>/src/ or <root>/lib/modules
        // where <root> is the executable's grandparent directory.
        g_codegen_system_module = 0;
        {
            char exe_path_s[PATH_MAX];
            ssize_t len_s = readlink("/proc/self/exe", exe_path_s, sizeof(exe_path_s) - 1);
            if (len_s != -1) {
                exe_path_s[len_s] = 0;
                char root[PATH_MAX];
                snprintf(root, sizeof(root), "%s", exe_path_s);
                dirname(root); // bin/build dir
                dirname(root); // the compiler tree root
                char prefix[PATH_MAX];
                snprintf(prefix, sizeof(prefix), "%s/src/", root);
                if (strncmp(abs_path, prefix, strlen(prefix)) == 0)
                    g_codegen_system_module = 1;
                snprintf(prefix, sizeof(prefix), "%s/lib/modules/", root);
                if (strncmp(abs_path, prefix, strlen(prefix)) == 0)
                    g_codegen_system_module = 1;
            }
        }
        int gen_rc = generate_c_from_ast(ast, c_file, abs_path, 1);
        g_codegen_suppress_singletons = 0;
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
    if (need_compile && !forced_o_path) {
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
                "\"%s\" -o \"%s\"",
                include_dir,
                c_file, o_file);
        } else {
             snprintf(cmd, sizeof(cmd), 
                "gcc -c -Wall -Wno-cpp -Wno-implicit-function-declaration -D__STDC_WANT_LIB_EXT1__=1 "
                "-I%s/src/include -I%s/src/core/include "
                "\"%s\" -o \"%s\"",
                project_base, project_base,
                c_file, o_file);
        }
            
        if (run_cmd(cmd) != 0) {
            die("C Compilation failed: %s", c_file);
        }
    }
    
    if (!forced_o_path) list_add(&g_object_files, o_file);
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

// Compile all files of a directory module as one module (COIL-108).
// A pre-scan verifies every file declares the module, finds the
// group's init/exit hooks, and unions its imports for the chain.
static void compile_group(char paths[][PATH_MAX], int count, const char *module_name) {
    int has_init = 0, has_exit = 0;
    const char *imports[64];
    int import_count = 0;
    for (int i = 0; i < count; i++) {
        ASTNode *ast = NULL;
        if (parse_file(paths[i], &ast) != 0 || !ast) {
            die("Parsing failed: %s", paths[i]);
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
                if (has_body && (strcmp(ch->text, "init") == 0 ||
                                 strcmp(ch->text, "module_init") == 0))
                    has_init = 1;
                if (has_body && (strcmp(ch->text, "exit") == 0 ||
                                 strcmp(ch->text, "module_exit") == 0))
                    has_exit = 1;
            }
        }
        ast_free(ast);
    }
    for (int i = 0; i < count; i++) {
        ModuleGroup g;
        g.is_group = 1;
        g.is_primary = (i == 0);
        g.has_init = has_init;
        g.has_exit = has_exit;
        g.imports = imports;
        g.import_count = import_count;
        compile_file(paths[i], NULL, &g);
    }
}

// Resolve and compile one import, following spec 3.2's order:
// ./X.co, ./X/*.co, ./modules/X.co, ./modules/X/*.co, then the same
// shapes under the project src/ tree, then the system depot. A file
// and a directory providing the same module at one location is an
// error - the module would be ambiguous.
static void compile_import(const char *import_name, const char *current_file) {
    char base_dir[PATH_MAX] = {0};
    strncpy(base_dir, current_file, sizeof(base_dir) - 1);
    dirname(base_dir);

    char stems[4][PATH_MAX];
    snprintf(stems[0], PATH_MAX, "%s", base_dir);
    snprintf(stems[1], PATH_MAX, "%s/modules", base_dir);
    snprintf(stems[2], PATH_MAX, "%s/src", g_project_root);
    snprintf(stems[3], PATH_MAX, "%s/src/modules", g_project_root);

    for (int s = 0; s < 4; s++) {
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
    die("Could not resolve import: %s in %s", import_name, current_file);
}


/* ---------- Main ---------- */

int main(int argc, char *argv[]) {
    // Ensure we have gcc before doing anything
    check_build_essentials();

    setbuf(stdout, NULL);
    if (argc < 3) {
        fprintf(stderr, "Usage: come build <file.co|.> [-o output]\n");
        return 1;
    }

    const char *cmd = argv[1];
    if (strcmp(cmd, "build") != 0 && strcmp(cmd, "genc") != 0) {
        fprintf(stderr, "Unknown command: %s\n", cmd);
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
        } else {
            input = argv[i];
        }
    }

    if (!input) die("No input file specified");

    // Setup Directries
    detect_project_root(g_project_root, sizeof(g_project_root));
    
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
    int pos = snprintf(link_cmd, sizeof(link_cmd), "gcc -o \"%s\"", out_bin);

    // Add user objects
    StringList *node = g_object_files;
    while (node) {
        pos += snprintf(link_cmd + pos, sizeof(link_cmd) - pos, " \"%s\"", node->value);
        node = node->next;
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

    pos += snprintf(link_cmd + pos, sizeof(link_cmd) - pos, " -ldl");

    if (run_cmd(link_cmd) != 0) {
        die("Linking failed");
    }

    printf("Built: %s\n", out_bin);
    
    list_free(g_object_files);
    list_free(g_visited_files);
    
    return 0;
}
