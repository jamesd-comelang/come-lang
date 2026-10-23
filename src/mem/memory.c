/*
 * Come memory framework — hierarchical ownership contexts.
 *
 * Every allocation is a node in a tree: a chunk has one parent (its
 * owner) and any number of children. Freeing a chunk frees its whole
 * subtree. Stealing moves a chunk (with its subtree) under a new
 * owner. Reallocating a chunk keeps its children attached.
 *
 * This is the memory model of the Come language (spec section 7)
 * implemented directly. Written from scratch for Come and licensed
 * under the project's Apache-2.0 license; it replaces the previously
 * vendored LGPL talloc library. No third-party code.
 *
 * The public API is the mem_talloc_* wrapper declared in
 * include/mem/talloc.h. Its signatures and semantics are unchanged,
 * so all call sites (compiler, runtime, generated code, tests) work
 * exactly as before.
 */

#include "mem/talloc.h"
#include "come_err.h"

#include <stddef.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

/* ---- chunk layout -------------------------------------------------- */

typedef struct co_chunk {
    struct co_chunk *parent;
    struct co_chunk *first_child;
    struct co_chunk *next;  /* next sibling */
    struct co_chunk *prev;  /* previous sibling */
    size_t size;            /* user bytes following the header */
} co_chunk;

/* Header size, rounded up so the user pointer keeps maximum alignment. */
#define CO_HDR_SIZE \
    ((sizeof(co_chunk) + alignof(max_align_t) - 1) / alignof(max_align_t) \
     * alignof(max_align_t))

static co_chunk *co_from_ptr(const void *ptr) {
    return (co_chunk *)((const char *)ptr - CO_HDR_SIZE);
}

static void *co_to_ptr(const co_chunk *chunk) {
    return (void *)((const char *)chunk + CO_HDR_SIZE);
}

/* ---- tree primitives ------------------------------------------------ */

static void co_link(co_chunk *parent, co_chunk *chunk) {
    chunk->parent = parent;
    chunk->prev = NULL;
    chunk->next = parent ? parent->first_child : NULL;
    if (chunk->next)
        chunk->next->prev = chunk;
    if (parent)
        parent->first_child = chunk;
}

static void co_unlink(co_chunk *chunk) {
    if (chunk->prev)
        chunk->prev->next = chunk->next;
    if (chunk->next)
        chunk->next->prev = chunk->prev;
    if (chunk->parent && chunk->parent->first_child == chunk)
        chunk->parent->first_child = chunk->next;
    chunk->parent = NULL;
    chunk->next = NULL;
    chunk->prev = NULL;
}

static co_chunk *co_alloc_chunk(co_chunk *parent, size_t size) {
    if (size > SIZE_MAX - CO_HDR_SIZE)
        return NULL;
    co_chunk *chunk = malloc(CO_HDR_SIZE + size);
    if (!chunk)
        return NULL;
    chunk->first_child = NULL;
    chunk->size = size;
    co_link(parent, chunk);
    return chunk;
}

static void co_free_chunk(co_chunk *chunk) {
    /* Children first; each child unlinks itself from this chunk. */
    while (chunk->first_child)
        co_free_chunk(chunk->first_child);
    co_unlink(chunk);
    free(chunk);
}

static void *co_realloc_chunk(co_chunk *chunk, size_t size) {
    if (size > SIZE_MAX - CO_HDR_SIZE)
        return NULL;
    co_chunk *parent = chunk->parent;
    co_chunk *prev = chunk->prev;
    co_chunk *next = chunk->next;
    co_chunk *moved = realloc(chunk, CO_HDR_SIZE + size);
    if (!moved)
        return NULL;  /* old chunk untouched, as with realloc */
    if (moved != chunk) {
        /* Rewire every pointer that referenced the old address. */
        if (parent && parent->first_child == chunk)
            parent->first_child = moved;
        if (prev)
            prev->next = moved;
        if (next)
            next->prev = moved;
        for (co_chunk *c = moved->first_child; c; c = c->next)
            c->parent = moved;
    }
    moved->size = size;
    return co_to_ptr(moved);
}

/* ---- public wrapper API (include/mem/talloc.h) ---------------------- */

static void *co_mem_root = NULL;

static co_chunk *co_root_chunk(void) {
    return co_mem_root ? co_from_ptr(co_mem_root) : NULL;
}

void mem_talloc_module_init(void) {
    if (co_mem_root)
        return;
    co_chunk *root = co_alloc_chunk(NULL, 0);
    if (!root) {
        fprintf(stderr, "come mem: failed to create root context\n");
        return;
    }
    co_mem_root = co_to_ptr(root);
}

/* Module exit registry (spec 3.5, COIL-108). A module's generated
 * __init registers its __exit here when it first initializes, so
 * the registration order IS the initialization order. Running the
 * registry pops handlers from the end: exits run in the exact
 * reverse order of initialization, which guarantees no module
 * outlives its dependencies. (The previous model recursed from the
 * entry module through reverse import lists, which could exit a
 * dependency before a dependent that imported it directly.) */
#define COME_MAX_EXIT_HANDLERS 256
static void (*g_exit_handlers[COME_MAX_EXIT_HANDLERS])(void);
static int g_exit_handler_count = 0;

void come_register_exit(void (*fn)(void)) {
    if (fn && g_exit_handler_count < COME_MAX_EXIT_HANDLERS)
        g_exit_handlers[g_exit_handler_count++] = fn;
}

void come_run_exits(void) {
    while (g_exit_handler_count > 0) {
        g_exit_handler_count--;
        g_exit_handlers[g_exit_handler_count]();
    }
}

void mem_talloc_module_shutdown(void) {
    if (co_mem_root)
        co_free_chunk(co_from_ptr(co_mem_root));
    co_mem_root = NULL;
}

/* ---- error reporting (spec 12, COIL-118 + COIL-139) ----------
 * errno carries the code; a specific message may ride along for
 * the failures whose strerror text misleads (a missing map key
 * is not "No such file or directory"). Since COIL-139 every set
 * is also recorded in a fixed-size chain (come_err.h): code,
 * message, and the source location - module, function, file,
 * line - where the error was set. Index 0 is the root cause.
 * The record is per execution context (see come_err_activate);
 * the coroutine scheduler swaps it together with errno. */
static ComeErrRecord g_err_main;
static ComeErrRecord *g_err_active = &g_err_main;

ComeErrRecord *come_err_active(void) { return g_err_active; }

void come_err_activate(ComeErrRecord *rec) {
    if (!rec || rec == g_err_active) return;
    g_err_active->saved_errno = errno;
    g_err_active = rec;
    errno = rec->saved_errno;
}

void come_err_arm(const char *module, const char *func,
                  const char *file, int line) {
    ComeErrRecord *r = g_err_active;
    r->armed = 1;
    r->arm_line = line;
    snprintf(r->arm_module, sizeof(r->arm_module), "%s", module ? module : "");
    snprintf(r->arm_func, sizeof(r->arm_func), "%s", func ? func : "");
    snprintf(r->arm_file, sizeof(r->arm_file), "%s", file ? file : "");
}

// Append one entry. When the chain is full the newest entry
// replaces the last one, so the root cause at index 0 and the
// most recent failure both survive.
static void err_append(int code, const char *msg,
                       const char *module, const char *func,
                       const char *file, int line) {
    ComeErrRecord *r = g_err_active;
    int idx;
    if (r->depth < COME_ERR_CHAIN_MAX) {
        idx = r->depth++;
    } else {
        idx = COME_ERR_CHAIN_MAX - 1;
    }
    ComeErrEntry *e = &r->entries[idx];
    e->code = code;
    e->line = line;
    snprintf(e->msg, sizeof(e->msg), "%s", msg ? msg : "");
    snprintf(e->module, sizeof(e->module), "%s", module ? module : "");
    snprintf(e->func, sizeof(e->func), "%s", func ? func : "");
    snprintf(e->file, sizeof(e->file), "%s", file ? file : "");
}

static void err_set_legacy(int code, const char *msg) {
    errno = code;
    ComeErrRecord *r = g_err_active;
    if (msg && *msg) {
        r->custom_code = code;
        strncpy(r->custom, msg, sizeof(r->custom) - 1);
        r->custom[sizeof(r->custom) - 1] = 0;
    } else {
        r->custom_code = 0;
        r->custom[0] = 0;
    }
}

void come_ERR_set(int code, const char *msg) {
    ComeErrRecord *r = g_err_active;
    if (code != 0) {
        if (r->armed) {
            err_append(code, msg, r->arm_module, r->arm_func,
                       r->arm_file, r->arm_line);
        } else {
            err_append(code, msg, "", "", "", 0);
        }
    }
    r->armed = 0;
    err_set_legacy(code, msg);
}

void come_err_record_at(int code, const char *msg,
                        const char *file, int line,
                        const char *cfunc) {
    // Demangle come_<module>__<func> when the shape matches.
    char mod[48] = "";
    char fn[64] = "";
    if (cfunc && strncmp(cfunc, "come_", 5) == 0) {
        const char *sep = strstr(cfunc + 5, "__");
        if (sep && sep != cfunc + 5) {
            size_t ml = (size_t)(sep - (cfunc + 5));
            if (ml >= sizeof(mod)) ml = sizeof(mod) - 1;
            memcpy(mod, cfunc + 5, ml);
            mod[ml] = 0;
            snprintf(fn, sizeof(fn), "%s", sep + 2);
        }
    }
    if (!fn[0] && cfunc) snprintf(fn, sizeof(fn), "%s", cfunc);
    err_append(code, msg, mod, fn, file ? file : "", line);
    g_err_active->armed = 0;
    err_set_legacy(code, msg);
}

const char *come_ERR_message_for(int code) {
    ComeErrRecord *r = g_err_active;
    if (r->custom_code != 0 && r->custom_code == code && r->custom[0])
        return r->custom;
    return NULL;
}

void come_ERR_clear_custom(void) {
    g_err_active->custom_code = 0;
    g_err_active->custom[0] = 0;
}

void come_err_reset(void) {
    ComeErrRecord *r = g_err_active;
    r->depth = 0;
    r->armed = 0;
    r->custom_code = 0;
    r->custom[0] = 0;
    r->saved_errno = 0;
}

int come_err_depth(void) { return g_err_active->depth; }

// Validate an accessor index. i == 0 on an empty record is
// the legacy read (errno-backed) and never records; any
// other out-of-range index appends an ERANGE entry located
// at the access site and reports failure.
static int err_index_ok(int i, const char *module, const char *func,
                        const char *file, int line) {
    ComeErrRecord *r = g_err_active;
    if (i >= 0 && i < r->depth) return 1;
    if (i == 0 && r->depth == 0) return 0;   // legacy read
    err_append(ERANGE, "error index out of range", module, func, file, line);
    errno = ERANGE;
    return -1;
}

int come_err_code_at(int i, const char *module, const char *func,
                     const char *file, int line) {
    int ok = err_index_ok(i, module, func, file, line);
    if (ok == 1) return g_err_active->entries[i].code;
    if (ok == 0) return errno;
    return 0;
}

const char *come_err_msg_at(int i, const char *module, const char *func,
                            const char *file, int line) {
    int ok = err_index_ok(i, module, func, file, line);
    if (ok == 1) {
        ComeErrEntry *e = &g_err_active->entries[i];
        if (e->msg[0]) return e->msg;
        const char *t = strerror(e->code);
        return t ? t : "Unknown error";
    }
    if (ok == 0) {
        const char *m = come_ERR_message_for(errno);
        if (m) return m;
        const char *t = strerror(errno);
        return t ? t : "Unknown error";
    }
    return "";
}

const char *come_err_where_at(int i, char *buf, unsigned long bufsz,
                              const char *module, const char *func,
                              const char *file, int line) {
    int ok = err_index_ok(i, module, func, file, line);
    if (ok == 1) {
        ComeErrEntry *e = &g_err_active->entries[i];
        if (e->file[0]) {
            snprintf(buf, bufsz, "%s:%d", e->file, e->line);
            return buf;
        }
    }
    if (bufsz) buf[0] = 0;
    return buf;
}

const char *come_err_describe_at(int i, char *buf, unsigned long bufsz) {
    ComeErrRecord *r = g_err_active;
    if (i < 0 || i >= r->depth) {
        if (bufsz) buf[0] = 0;
        return buf;
    }
    ComeErrEntry *e = &r->entries[i];
    if (e->file[0] && e->module[0]) {
        snprintf(buf, bufsz, "%s.%s at %s:%d", e->module, e->func, e->file, e->line);
    } else if (e->file[0]) {
        snprintf(buf, bufsz, "%s:%d", e->file, e->line);
    } else if (bufsz) {
        buf[0] = 0;
    }
    return buf;
}

void *mem_talloc_alloc(void *ctx, size_t size) {
    if (!co_mem_root)
        mem_talloc_module_init();
    co_chunk *parent = ctx ? co_from_ptr(ctx) : co_root_chunk();
    co_chunk *chunk = co_alloc_chunk(parent, size);
    if (!chunk)
        errno = ENOMEM;
    return chunk ? co_to_ptr(chunk) : NULL;
}

void *mem_talloc_realloc(void *ctx, void *ptr, size_t size) {
    if (!ptr)
        return mem_talloc_alloc(ctx, size);
    if (size == 0) {
        co_free_chunk(co_from_ptr(ptr));
        return NULL;
    }
    void *np = co_realloc_chunk(co_from_ptr(ptr), size);
    if (!np)
        errno = ENOMEM;
    return np;
}

void mem_talloc_free(void *ptr) {
    if (ptr)
        co_free_chunk(co_from_ptr(ptr));
}

void *mem_talloc_new_ctx(void *parent) {
    if (!co_mem_root)
        mem_talloc_module_init();
    co_chunk *p = parent ? co_from_ptr(parent) : co_root_chunk();
    co_chunk *chunk = co_alloc_chunk(p, 0);
    if (!chunk) {
        fprintf(stderr, "come mem: failed to create new context\n");
        return NULL;
    }
    return co_to_ptr(chunk);
}

void *mem_talloc_steal(void *new_ctx, void *ptr) {
    if (!ptr)
        return NULL;
    if (!co_mem_root)
        mem_talloc_module_init();
    co_chunk *chunk = co_from_ptr(ptr);
    co_chunk *parent = new_ctx ? co_from_ptr(new_ctx) : co_root_chunk();
    /* Refuse to create a cycle: the new owner must not be the chunk
       itself or one of its descendants. */
    for (co_chunk *c = parent; c; c = c->parent) {
        if (c == chunk)
            return ptr;
    }
    co_unlink(chunk);
    co_link(parent, chunk);
    return ptr;
}
