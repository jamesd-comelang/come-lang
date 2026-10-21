// src/come_compiler.c
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define access _access
#define getcwd _getcwd
#ifndef F_OK
#define F_OK 0
#endif
extern unsigned long __stdcall GetModuleFileNameA(void *module, char *filename, unsigned long size);
#else
#include <unistd.h>
#include <libgen.h>
#endif

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "codegen.h"
#include "common.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// Silence truncation warnings for path operations
#pragma GCC diagnostic ignored "-Wformat-truncation"

int g_verbose = 0;
static char g_project_root[PATH_MAX];
static char g_ccache_dir[PATH_MAX];
static char g_build_dir[PATH_MAX];

#ifdef _WIN32
#define NULL_DEVICE "NUL"
#else
#define NULL_DEVICE "/dev/null"
#endif

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

static int is_path_sep(char c) {
    return c == '/' || c == '\\';
}

static const char *last_path_sep(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (!slash) return backslash;
    if (!backslash) return slash;
    return slash > backslash ? slash : backslash;
}

static const char *path_basename_ptr(const char *path) {
    const char *sep = last_path_sep(path);
    return sep ? sep + 1 : path;
}

#ifdef _WIN32
static void append_exe_suffix_if_needed(char *path, size_t sz) {
    const char *base = path_basename_ptr(path);
    const char *dot = strrchr(base, '.');
    if (dot) return;
    if (strlen(path) + 4 < sz) strcat(path, ".exe");
}
#endif

static void copy_path_dirname(const char *path, char *out, size_t sz) {
    size_t len = strlen(path);
    while (len > 0 && is_path_sep(path[len - 1])) len--;

    size_t dir_len = len;
    while (dir_len > 0 && !is_path_sep(path[dir_len - 1])) dir_len--;

    if (dir_len == 0) {
        snprintf(out, sz, ".");
        return;
    }

#ifdef _WIN32
    if (dir_len == 3 && path[1] == ':' && is_path_sep(path[2])) {
        snprintf(out, sz, "%.*s", (int)dir_len, path);
        return;
    }
#endif

    while (dir_len > 1 && is_path_sep(path[dir_len - 1])) dir_len--;
    snprintf(out, sz, "%.*s", (int)dir_len, path);
}

static const char *skip_path_seps(const char *path) {
    while (is_path_sep(*path)) path++;
    return path;
}

static int make_dir(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static int canonical_path(const char *path, char *out, size_t sz) {
#ifdef _WIN32
    return _fullpath(out, path, sz) != NULL;
#else
    return realpath(path, out) != NULL;
#endif
}

static int executable_path(char *out, size_t sz) {
#ifdef _WIN32
    unsigned long len = GetModuleFileNameA(NULL, out, (unsigned long)sz);
    if (len == 0 || len >= sz) return 0;
    out[len] = 0;
    return 1;
#else
    ssize_t len = readlink("/proc/self/exe", out, sz - 1);
    if (len == -1) return 0;
    out[len] = 0;
    return 1;
#endif
}

// Create directory if not exists (non-recursive for now, used with built paths)
static void ensure_dir(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);

    char *start = tmp + 1;
#ifdef _WIN32
    if (tmp[0] && tmp[1] == ':') {
        start = tmp + 2;
        if (is_path_sep(*start)) start++;
    } else if (is_path_sep(tmp[0]) && is_path_sep(tmp[1])) {
        start = tmp + 2;
        while (*start && !is_path_sep(*start)) start++;
        if (*start) start++;
        while (*start && !is_path_sep(*start)) start++;
        if (*start) start++;
    }
#endif
    
    // Iterate string to create parents
    for (char *p = start; *p; p++) {
        if (is_path_sep(*p)) {
            char sep = *p;
            *p = 0;
            // Ignore error if exists
            make_dir(tmp);
            *p = sep;
        }
    }
    make_dir(tmp);
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
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "gcc --version > %s 2>&1", NULL_DEVICE);
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "Error: Build essentials (gcc) not found.\n");
#ifdef _WIN32
        fprintf(stderr, "Please install MinGW-w64 and ensure gcc.exe is on PATH.\n");
#else
        fprintf(stderr, "Please install gcc/build-essential (e.g. apt install build-essential)\n");
#endif
        exit(1);
    }
}

// Find project root based on .git or build/come presence, or fallback to cwd
static void detect_project_root(char *out, size_t sz) {
    if (getcwd(out, sz) == NULL) die("getcwd failed");
}

/* ---------- Module Resolution ---------- */

// Resolve import "abc" to a .co file path
// Returns 1 if found (writes to out), 0 otherwise
static int resolve_import(const char *import_name, const char *current_file, char *out, size_t sz) {
    char base_dir[PATH_MAX] = {0};
    char candidate[PATH_MAX] = {0};
    
    // Get directory of current file
    strncpy(base_dir, current_file, sizeof(base_dir) - 1);
    copy_path_dirname(current_file, base_dir, sizeof(base_dir));

    // If base_dir is '.', use g_project_root? 
    // realpath handles relative, but let's be safe.
    // If current_file was absolute, base_dir is absolute.
    
    /* Phase 1: Local Context Search */
    // 1. ./abc.co
    snprintf(candidate, sizeof(candidate), "%s/%s.co", base_dir, import_name);
    if (file_exists(candidate)) { canonical_path(candidate, out, sz); return 1; }
    
    // 2. ./abc/abc.co
    snprintf(candidate, sizeof(candidate), "%s/%s/%s.co", base_dir, import_name, import_name);
    if (file_exists(candidate)) { canonical_path(candidate, out, sz); return 1; }

    // 3. ./modules/abc.co
    snprintf(candidate, sizeof(candidate), "%s/modules/%s.co", base_dir, import_name);
    if (file_exists(candidate)) { canonical_path(candidate, out, sz); return 1; }

    /* Phase 2: Source Directory Search (./src) */
    // 1. ./src/abc.co
    snprintf(candidate, sizeof(candidate), "%s/src/%s.co", g_project_root, import_name);
    if (file_exists(candidate)) { canonical_path(candidate, out, sz); return 1; }

    // 2. ./src/abc/abc.co
    snprintf(candidate, sizeof(candidate), "%s/src/%s/%s.co", g_project_root, import_name, import_name);
    if (file_exists(candidate)) { canonical_path(candidate, out, sz); return 1; }

    // 3. ./src/modules/abc.co
    snprintf(candidate, sizeof(candidate), "%s/src/modules/%s.co", g_project_root, import_name);
    if (file_exists(candidate)) { canonical_path(candidate, out, sz); return 1; }

    /* Phase 3: System Repository Search */
    // Use relative path from executable to find modules
    char exe_path_r[PATH_MAX];
    if (executable_path(exe_path_r, sizeof(exe_path_r))) {
        char exe_dir_r[PATH_MAX];
        copy_path_dirname(exe_path_r, exe_dir_r, sizeof(exe_dir_r));
        snprintf(candidate, sizeof(candidate), "%s/../lib/modules/%s.co", exe_dir_r, import_name);
        if (file_exists(candidate)) { canonical_path(candidate, out, sz); return 1; }
    }

    return 0;
}

/* ---------- Compilation ---------- */

// Compile a single file, recursing on imports
static void compile_file(const char *source_path, const char *forced_o_path) {
    char abs_path[PATH_MAX];
    if (!canonical_path(source_path, abs_path, sizeof(abs_path))) {
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

                char import_path[PATH_MAX];
                if (resolve_import(import_name, abs_path, import_path, sizeof(import_path))) {
                    compile_file(import_path, NULL);
                } else {
                    die("Could not resolve import: %s in %s", import_name, abs_path);
                }
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
            p = skip_path_seps(p);
            strncpy(rel_path, p, sizeof(rel_path));
        } else {
            strcpy(rel_path, path_basename_ptr(abs_path));
        }
        snprintf(c_file, sizeof(c_file), "%s/%s.c", g_ccache_dir, rel_path);
    }
    
    char c_dir[PATH_MAX];
    strcpy(c_dir, c_file);
    copy_path_dirname(c_file, c_dir, sizeof(c_dir));
    ensure_dir(c_dir);

    char o_file[PATH_MAX];
    char base_name[PATH_MAX];
    strcpy(base_name, abs_path);
    char *bn = (char *)path_basename_ptr(base_name);
    char *dot = strrchr(bn, '.');
    if (dot) *dot = 0;
    snprintf(o_file, sizeof(o_file), "%s/%s.o", g_build_dir, bn);
    
    if (!forced_o_path) ensure_dir(g_build_dir);

    // 4. Incremental Check
    time_t t_src = get_mtime(abs_path);
    time_t t_c = get_mtime(c_file);
    time_t t_o = get_mtime(o_file);
    time_t t_compiler = 0;
    char compiler_exe_for_cache[PATH_MAX];
    if (executable_path(compiler_exe_for_cache, sizeof(compiler_exe_for_cache))) {
        t_compiler = get_mtime(compiler_exe_for_cache);
    }

    int need_transpile = (t_src > t_c || t_compiler > t_c);
    int need_compile = (need_transpile || t_c > t_o || t_src > t_o || t_compiler > t_o);
    
    if (get_mtime(o_file) == 0) need_compile = 1;
    if (forced_o_path) need_compile = 0; // If forcing output (genc), we might not compile or we don't care about object

    // 5. Transpile
    if (need_transpile || forced_o_path) {
        if (g_verbose) printf("Transpiling %s -> %s\n", abs_path, c_file);
        if (generate_c_from_ast(ast, c_file, abs_path, 1) != 0) {
            die("Codegen failed: %s", abs_path);
        }
    }
    ast_free(ast);

    // 6. Compiler (C -> O)
    if (need_compile && !forced_o_path) {
        if (g_verbose) printf("Compiling C %s -> %s\n", c_file, o_file);
        
        char exe_path[PATH_MAX];
        if (!executable_path(exe_path, sizeof(exe_path))) {
            die("Could not determine compiler executable path");
        }
        char exe_dir[PATH_MAX];
        copy_path_dirname(exe_path, exe_dir, sizeof(exe_dir));
        const char *build_dir_name = path_basename_ptr(exe_dir);
        char project_base[PATH_MAX];
        
        strcpy(project_base, exe_dir); 
        if (strcmp(build_dir_name, "build") == 0) {
             copy_path_dirname(exe_dir, project_base, sizeof(project_base));
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
                "-I\"%s\" -I\"%s/talloc\" "
                "\"%s\" -o \"%s\"",
                include_dir, include_dir,
                c_file, o_file);
        } else {
             snprintf(cmd, sizeof(cmd), 
                "gcc -c -Wall -Wno-cpp -Wno-implicit-function-declaration -D__STDC_WANT_LIB_EXT1__=1 "
                "-I%s/src/include -I%s/src/core/include "
                "-I%s/src/external/talloc/lib/talloc -I%s/src/external/talloc/lib/replace "
                "\"%s\" -o \"%s\"",
                project_base, project_base, project_base, project_base,
                c_file, o_file);
        }
            
        if (run_cmd(cmd) != 0) {
            die("C Compilation failed: %s", c_file);
        }
    }
    
    if (!forced_o_path) list_add(&g_object_files, o_file);
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
    compile_file(entry_file, (!build_mode && output) ? output : NULL);

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
             const char *p = path_basename_ptr(tmp);
             // if project root is dot, use 'main'
             if (strcmp(p, ".") == 0 || strcmp(p, "") == 0) p = "main";
             snprintf(out_bin, sizeof(out_bin), "%s/build/%s", g_project_root, p);
        } else {
             // specific file
             char tmp[PATH_MAX]; strcpy(tmp, input);
             // remove extension
             char *last_dot = strrchr(tmp, '.');
             const char *last_sep = last_path_sep(tmp);
             if (last_dot && (!last_sep || last_dot > last_sep)) {
                 *last_dot = 0;
             }
             strcpy(out_bin, tmp);
        }
    }

#ifdef _WIN32
    append_exe_suffix_if_needed(out_bin, sizeof(out_bin));
#endif

    // Construct Link Command
    char exe_path[PATH_MAX];
    if (!executable_path(exe_path, sizeof(exe_path))) {
        die("Could not determine compiler executable path");
    }
    char exe_dir[PATH_MAX];
    copy_path_dirname(exe_path, exe_dir, sizeof(exe_dir));
    const char *build_dir_name = path_basename_ptr(exe_dir);
    char project_base[PATH_MAX];
    strcpy(project_base, exe_dir); 
    if (strcmp(build_dir_name, "build") == 0) {
         copy_path_dirname(exe_dir, project_base, sizeof(project_base));
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
#ifdef _WIN32
        const char *std_objs[] = {"std.o", "string.o", "array.o", "map.o", "talloc.o"};
#else
        const char *std_objs[] = {"std.o", "string.o", "array.o", "map.o", "talloc.o", "talloc_lib.o"};
#endif
        int std_obj_count = (int)(sizeof(std_objs) / sizeof(std_objs[0]));
        for (int i=0; i<std_obj_count; i++) {
            pos += snprintf(link_cmd + pos, sizeof(link_cmd) - pos, " \"%s/build/%s\"", project_base, std_objs[i]);
        }
    }

#ifndef _WIN32
    pos += snprintf(link_cmd + pos, sizeof(link_cmd) - pos, " -ldl");
#endif

    if (run_cmd(link_cmd) != 0) {
        die("Linking failed");
    }

    printf("Built: %s\n", out_bin);
    
    list_free(g_object_files);
    list_free(g_visited_files);
    
    return 0;
}
