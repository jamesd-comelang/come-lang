// src/come_compiler.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "codegen.h"
#include "common.h"

int g_verbose = 0;


/* ---------- small utilities (local, no external deps) ---------- */

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static int ends_with(const char *s, const char *suffix) {
    size_t ls = strlen(s), lt = strlen(suffix);
    return (ls >= lt) && memcmp(s + ls - lt, suffix, lt) == 0;
}

static void strip_suffix(char *s, const char *suffix) {
    size_t ls = strlen(s), lt = strlen(suffix);
    if (ls >= lt && memcmp(s + ls - lt, suffix, lt) == 0) {
        s[ls - lt] = '\0';
    }
}

/* dirname-like helper: copy directory part into out (no trailing slash unless root). */
static void path_dirname(const char *path, char *out, size_t outsz) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, outsz, ".");
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}


/* create directory tree best-effort for POSIX. On Windows, no-op fallback. */
static void mkdir_p_for_file(const char *filepath) {
    char dir[1024];
    path_dirname(filepath, dir, sizeof(dir));
    if (strcmp(dir, ".") == 0 || strcmp(dir, "/") == 0) return;

    // Build path piece by piece
    char tmp[1024];
    tmp[0] = '\0';
    const char *p = dir;
    while (*p) {
        // copy up to next '/'
        const char *q = strchr(p, '/');
        size_t chunk = q ? (size_t)(q - p) : strlen(p);
        if (strlen(tmp) + (tmp[0] ? 1 : 0) + chunk + 1 >= sizeof(tmp)) break;

        if (tmp[0]) strcat(tmp, "/");
        strncat(tmp, p, chunk);

        if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
            // best effort only; ignore errors
            break;
        }
        if (!q) break;
        p = q + 1;
    }
}

/* Find project root by looking for build/come executable */
static void get_project_root(char *out, size_t outsz) {
    // Try to find the compiler executable
    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
        // exe_path is like /path/to/project/build/come
        // Get directory of exe
        char *last_slash = strrchr(exe_path, '/');
        if (last_slash) {
            *last_slash = '\0'; // Now exe_path is /path/to/project/build
            last_slash = strrchr(exe_path, '/');
            if (last_slash) {
                *last_slash = '\0'; // Now exe_path is /path/to/project
                snprintf(out, outsz, "%s", exe_path);
                return;
            }
        }
    }
    // Fallback: assume current directory
    snprintf(out, outsz, ".");
}

/* run a shell command; return non-zero if failed */
static int run_cmd(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_verbose) fprintf(stderr, "[CMD] %s\n", buf);
    int rc = system(buf);
    return rc;
}

/* ---------- CLI parsing ---------- */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s build <file.co> [-o <bin_path>]  - Full build: generate C and link to binary\n"
        "  %s genc  <file.co> [-o <c_path>]    - Generate C code only\n",
        prog, prog);
}

/* ---------- main ---------- */

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    // printf("COME compiler starting...\n");
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];
    int build_mode = 0;
    if (strcmp(cmd, "build") == 0) {
        build_mode = 1;
    } else if (strcmp(cmd, "genc") == 0) {
        build_mode = 0;
    } else {
        usage(argv[0]);
        return 1;
    }

    const char *co_file = NULL;
    const char *out_path = NULL;

    // Parse options: come build/genc <file.co> [-o out]
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                die("Error: -o requires an output path");
            }
            out_path = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else if (argv[i][0] == '-') {
            die("Unknown option: %s", argv[i]);
        } else {
            co_file = argv[i];
        }
    }

    if (g_verbose) printf("COME compiler starting...\n");

    if (!co_file) {
        usage(argv[0]);
        return 1;
    }
    if (!ends_with(co_file, ".co")) {
        die("Input must be a .co file: %s", co_file);
    }

    /* Prepare filenames */
    char c_file[1024];
    char bin_file[1024];

    if (build_mode) {
        // Build mode: binary path
        if (out_path) {
            snprintf(bin_file, sizeof(bin_file), "%s", out_path);
        } else {
            snprintf(bin_file, sizeof(bin_file), "%s", co_file);
            strip_suffix(bin_file, ".co");
        }
        // Intermediate C file
        snprintf(c_file, sizeof(c_file), "%s.c", co_file);
    } else {
        // genc mode: C file path
        if (out_path) {
            snprintf(c_file, sizeof(c_file), "%s", out_path);
        } else {
            snprintf(c_file, sizeof(c_file), "%s.c", co_file);
        }
    }

    ASTNode *ast = NULL;
    if (g_verbose) printf("Parsing file: %s\n", co_file);
    if (parse_file(co_file, &ast) != 0 || !ast) {
        die("Parsing failed: %s", co_file);
    }

    /* Codegen -> C file */
    if (generate_c_from_ast(ast, c_file, co_file, build_mode) != 0) {
        ast_free(ast);
        die("Code generation failed: %s", c_file);
    }

    if (!build_mode) {
        if (g_verbose) printf("Generated C code: %s\n", c_file);
        ast_free(ast);
        return 0;
    }

    /* Compile C -> executable */
    if (out_path) {
        mkdir_p_for_file(out_path);
    }

    char project_root[1024];
    get_project_root(project_root, sizeof(project_root));

    if (run_cmd("gcc -Wall -Werror -Wno-cpp -g -D__STDC_WANT_LIB_EXT1__=1 "
                "-I%s/src/include -I%s/src/core/include -I%s/external/talloc/lib/talloc -I%s/external/talloc/lib/replace "
                "\"%s\" %s/build/std.o %s/build/string.o %s/build/array.o %s/build/map.o %s/build/talloc.o %s/build/talloc_lib.o -o \"%s\" -ldl", 
                project_root, project_root, project_root, project_root,
                c_file, project_root, project_root, project_root, project_root, project_root, project_root, bin_file) != 0) {
        ast_free(ast);
        die("GCC compilation failed");
    }

    /* Cleanup intermediate C file */
    remove(c_file);

    ast_free(ast);
    printf("Built executable: %s\n", bin_file);
    return 0;
}

