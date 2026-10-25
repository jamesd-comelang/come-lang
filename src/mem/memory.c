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
ComeErrRecord *g_err_active = &g_err_main;

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
    // Pointers first, armed last: the record is only read
    // once armed, and every arming site passes literals.
    r->arm_module = module ? module : "";
    r->arm_func = func ? func : "";
    r->arm_file = file ? file : "";
    r->arm_line = line;
    r->armed = 1;
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

/* ---- scope contexts (COIL-157, spec 7) ------------------------------- *
 * A scope context is a talloc chunk like any other; the stack only
 * records which chunk is current. Popping a frame frees its chunk,
 * which frees every buffer allocated in that scope - including
 * blocks a nested scope stole nothing from. A buffer that must
 * outlive its scope is adopted first (come_scope_adopt), which
 * re-parents its block under a longer-lived context.
 *
 * Frames are plain malloc memory, never talloc children: the stack
 * itself must survive the scopes it tracks. The active stack is
 * per coroutine; the scheduler swaps it on a switch (coro.c). */
/* ---- bump arenas (Design 2, redesign) ----------------------- */
/* A chunked bump allocator for temporary values. Allocations
   bump a pointer within the current block; blocks are never
   moved (a realloc would stale pointers held by earlier
   temporaries in the same statement), so a new block is
   linked when the current one fills. Each scope frame records
   the (block, offset) it started in; popping the frame frees
   every block allocated after that point and rewinds the
   checkpoint block - bulk free for all temporaries in the
   frame, with no per-value bookkeeping. The arena lives in
   the scope stack, so each coroutine has its own, and it
   swaps with the stack. */
#define CO_ARENA_BLOCK_SIZE 4096
typedef struct co_arena_block {
    struct co_arena_block *next;
    size_t used;
    size_t cap; /* bytes of data[] */
    char data[];
} co_arena_block;

typedef struct {
    void *ctx;
    int is_marker;
    /* Arena checkpoint at push: the block live then (NULL
       if the arena was empty) and its used offset. */
    co_arena_block *arena_block;
    size_t arena_off;
} co_scope_frame;

typedef struct {
    co_scope_frame *frames;
    int top; /* number of frames on the stack */
    int cap;
    co_arena_block *arena_head; /* first block, in order */
    co_arena_block *arena_cur;  /* block being bumped */
} co_scope_stack;

static co_scope_stack co_scope_main = { NULL, 0, 0, NULL, NULL };
co_scope_stack *co_scope_active = &co_scope_main;

void co_arena_free_blocks(co_arena_block *head) {
    while (head) {
        co_arena_block *nx = head->next;
        free(head);
        head = nx;
    }
}

/* Rewind the arena to a frame's checkpoint: free every block
   allocated after the checkpoint block, rewind the checkpoint
   block itself. A NULL checkpoint means the arena was empty
   at push, so free everything. */
void co_arena_reset_to(co_scope_stack *s,
                              co_arena_block *blk, size_t off) {
    if (!blk) {
        /* Arena was empty at the checkpoint: rewind to
           empty but keep the first standard block for
           reuse. A loop whose iterations each start
           from an empty arena would otherwise malloc
           and free a block per iteration - slower than
           the talloc small-block cache it replaces.
           Free any further blocks (overflow/oversize,
           rare) and an oversize first block (don't pin
           a huge one). The stack-empty pop below frees
           the kept block at the end. */
        if (s->arena_head && s->arena_head->cap <= CO_ARENA_BLOCK_SIZE) {
            co_arena_free_blocks(s->arena_head->next);
            s->arena_head->next = NULL;
            s->arena_head->used = 0;
            s->arena_cur = s->arena_head;
        } else {
            co_arena_free_blocks(s->arena_head);
            s->arena_head = s->arena_cur = NULL;
        }
        return;
    }
    co_arena_free_blocks(blk->next);
    blk->next = NULL;
    blk->used = off;
    s->arena_cur = blk;
}

void *come_arena_alloc(size_t size) {
    co_scope_stack *s = co_scope_active;
    size = (size + 7) & ~(size_t)7; /* 8-byte align */
    if (size == 0) size = 8;
    co_arena_block *b = s->arena_cur;
    if (!b || b->used + size > b->cap) {
        size_t cap = size > CO_ARENA_BLOCK_SIZE ? size : CO_ARENA_BLOCK_SIZE;
        co_arena_block *nb = malloc(sizeof(co_arena_block) + cap);
        if (!nb) return NULL;
        nb->next = NULL;
        nb->used = 0;
        nb->cap = cap;
        if (!s->arena_head) s->arena_head = nb;
        else s->arena_cur->next = nb;
        s->arena_cur = nb;
        b = nb;
    }
    void *p = b->data + b->used;
    b->used += size;
    return p;
}

int come_arena_contains(const void *ptr) {
    co_scope_stack *s = co_scope_active;
    for (co_arena_block *b = s->arena_head; b; b = b->next) {
        if ((const char *)ptr >= b->data &&
            (const char *)ptr < b->data + b->used)
            return 1;
    }
    return 0;
}

int co_scope_grow(co_scope_stack *s) {
    if (s->top < s->cap) return 1;
    int ncap = s->cap ? s->cap * 2 : 16;
    co_scope_frame *nf = realloc(s->frames, sizeof(co_scope_frame) * (size_t)ncap);
    if (!nf) return 0;
    s->frames = nf;
    s->cap = ncap;
    return 1;
}

int co_scope_marker_base(co_scope_stack *s) {
    for (int i = s->top - 1; i >= 0; i--)
        if (s->frames[i].is_marker) return i;
    return -1;
}

static void *co_scope_materialize(co_scope_stack *s, int idx);

void *come_scope_current(void) {
    co_scope_stack *s = co_scope_active;
    if (s->top > 0) return co_scope_materialize(s, s->top - 1);
    if (!co_mem_root) mem_talloc_module_init();
    return co_mem_root;
}

/* ---- scope-frame chunk freelist (COIL-160) ----------------------- */
/* A scope frame's context is a zero-payload chunk: every frame
   chunk is the same size, so popped frames can be recycled
   instead of malloc/free'd. The bench paid ~21 ns per push/pop
   pair (malloc + link + unlink + free); recycling keeps only
   the list manipulation, a few ns. Only frame chunks ever
   enter the list - data chunks are still freed normally, so a
   use-after-free on data is still caught. Children are always
   freed on pop; only the emptied frame chunk itself is kept.
   The list is capped: past 1024 idle frames, extras are freed
   as before, so deep recursion cannot pin unbounded memory.
   Coroutines cooperate, so no locking is needed; a chunk on
   the list is unreferenced by construction (unlinked, children
   freed). Its next field is free after unlink, so the list
   needs no extra memory. */
static co_chunk *co_frame_free_list = NULL;
static size_t co_frame_free_count = 0;
#define CO_FRAME_FREE_MAX 1024

static co_chunk *co_frame_alloc(co_chunk *parent) {
    co_chunk *chunk = co_frame_free_list;
    if (chunk) {
        co_frame_free_list = chunk->next;
        co_frame_free_count--;
        chunk->first_child = NULL;
        chunk->size = 0;
        co_link(parent, chunk);
        return chunk;
    }
    return co_alloc_chunk(parent, 0);
}

static void co_frame_free(co_chunk *chunk) {
    while (chunk->first_child)
        co_free_chunk(chunk->first_child);
    co_unlink(chunk);
    if (co_frame_free_count < CO_FRAME_FREE_MAX) {
        chunk->next = co_frame_free_list;
        co_frame_free_list = chunk;
        co_frame_free_count++;
    } else {
        free(chunk);
    }
}

static void *co_frame_new_ctx(void *parent) {
    if (!co_mem_root) mem_talloc_module_init();
    co_chunk *p = parent ? co_from_ptr(parent) : co_root_chunk();
    co_chunk *chunk = co_frame_alloc(p);
    if (!chunk) return NULL;
    return co_to_ptr(chunk);
}

void co_frame_free_ctx(void *ctx) {
    if (ctx) co_frame_free(co_from_ptr(ctx));
}

// perf2 lazy markers: a frame's talloc chunk is allocated only on
// first use (allocation, adoption, or nesting). Push records only
// the slot (NULL ctx + arena checkpoint); materialization happens
// in co_scope_materialize, called by come_scope_current(),
// come_scope_caller_ctx() and come_scope_ctx_for_depth(). A frame
// that never allocates, adopts, or nests pays no freelist and no
// talloc link/unlink — only the slot stores and top++. Pop already
// tolerates a NULL ctx (co_frame_free_ctx is a no-op for NULL),
// the same path checkpoint frames use.
static void *co_scope_materialize(co_scope_stack *s, int idx) {
    if (idx < 0) {
        if (!co_mem_root) mem_talloc_module_init();
        return co_mem_root;
    }
    if (s->frames[idx].ctx) return s->frames[idx].ctx;
    void *parent = co_scope_materialize(s, idx - 1);
    void *ctx = co_frame_new_ctx(parent);
    if (!ctx) return NULL;
    s->frames[idx].ctx = ctx;
    return ctx;
}

void come_scope_push(void) {
    co_scope_stack *s = co_scope_active;
    if (!co_scope_grow(s)) return;
    s->frames[s->top].ctx = NULL;
    s->frames[s->top].is_marker = 0;
    /* Design 2 (redesign): arena checkpoint for this frame. */
    s->frames[s->top].arena_block = s->arena_cur;
    s->frames[s->top].arena_off = s->arena_cur ? s->arena_cur->used : 0;
    s->top++;
}

// Step 4d (parity): a checkpoint frame records only
// the arena checkpoint, with no talloc frame context
// to allocate or free. It is sound only for blocks
// whose allocations are all arena (see
// block_is_checkpoint in codegen): there is nothing
// talloc to free at block end, and the arena rewind
// on pop frees the block's arena values. Pop is the
// ordinary come_scope_pop (its frame-free is a no-op
// for a NULL ctx).
void come_scope_push_checkpoint(void) {
    co_scope_stack *s = co_scope_active;
    if (!co_scope_grow(s)) return;
    s->frames[s->top].ctx = NULL;
    s->frames[s->top].is_marker = 0;
    s->frames[s->top].arena_block = s->arena_cur;
    s->frames[s->top].arena_off = s->arena_cur ? s->arena_cur->used : 0;
    s->top++;
}

void come_scope_push_marker(void) {
    co_scope_stack *s = co_scope_active;
    if (!co_scope_grow(s)) return;
    s->frames[s->top].ctx = NULL;
    s->frames[s->top].is_marker = 1;
    /* Design 2 (redesign): arena checkpoint for this frame. */
    s->frames[s->top].arena_block = s->arena_cur;
    s->frames[s->top].arena_off = s->arena_cur ? s->arena_cur->used : 0;
    s->top++;
}

void come_scope_pop(void) {
    co_scope_stack *s = co_scope_active;
    if (s->top <= 0) return;
    s->top--;
    /* Design 2 (redesign): bulk-free this frame's arena
       temporaries. If the stack is now empty, no arena
       value can be live, so free the arena entirely. */
    co_arena_reset_to(s, s->frames[s->top].arena_block,
                      s->frames[s->top].arena_off);
    if (s->top == 0) {
        co_arena_free_blocks(s->arena_head);
        s->arena_head = s->arena_cur = NULL;
    }
    co_frame_free_ctx(s->frames[s->top].ctx);
}

void come_scope_pop_to_marker(void) {
    co_scope_stack *s = co_scope_active;
    while (s->top > 0) {
        int marker = s->frames[s->top - 1].is_marker;
        come_scope_pop();
        if (marker) break;
    }
}

void come_scope_pop_to_depth(int lexical_depth) {
    co_scope_stack *s = co_scope_active;
    int base = co_scope_marker_base(s);
    if (base < 0) {
        while (s->top > 0) come_scope_pop();
        return;
    }
    while (s->top - 1 > base + lexical_depth) come_scope_pop();
}

void *come_scope_caller_ctx(void) {
    co_scope_stack *s = co_scope_active;
    int base = co_scope_marker_base(s);
    if (base <= 0) {
        if (!co_mem_root) mem_talloc_module_init();
        return co_mem_root;
    }
    return co_scope_materialize(s, base - 1);
}

void *come_scope_ctx_for_depth(int lexical_depth) {
    co_scope_stack *s = co_scope_active;
    int base = co_scope_marker_base(s);
    if (base < 0) {
        if (!co_mem_root) mem_talloc_module_init();
        return co_mem_root;
    }
    int idx = base + lexical_depth;
    if (idx >= s->top) return come_scope_current();
    return co_scope_materialize(s, idx);
}

int come_scope_adopt(void *dst, void *data) {
    if (!data) return 0;
    co_scope_stack *s = co_scope_active;
    co_chunk *chunk = co_from_ptr(data);
    co_chunk *parent = chunk->parent;
    if (!parent) return 0;
    if (!dst) {
        /* A NULL destination is the root context (a bare
           module map's ctx, for instance): it outlives
           every scope, so the steal always moves. */
        mem_talloc_steal(NULL, data);
        return 1;
    }
    co_chunk *dst_chunk = co_from_ptr(dst);
    /* Refuse a cycle: dst must not be the block itself or
       one of its descendants (mirrors mem_talloc_steal). */
    for (co_chunk *c = dst_chunk; c; c = c->parent)
        if (c == chunk) return 0;
    /* Where do parent and dst sit on the scope stack, if
       at all? A parent that is not a scope - a module or
       instance context, or another buffer's chunk - already
       outlives every scope, so there is nothing to move. */
    int k = -1, j = -1;
    for (int i = 0; i < s->top; i++) {
        if (!s->frames[i].ctx) continue;  /* lazy frame owns nothing */
        if (co_from_ptr(s->frames[i].ctx) == parent) k = i;
        if (co_from_ptr(s->frames[i].ctx) == dst_chunk) j = i;
    }
    if (k < 0) return 0;
    if (j < 0 || j < k) {
        mem_talloc_steal(dst, data);
        return 1;
    }
    return 0;
}

void *come_scope_stack_new(void) {
    co_scope_stack *s = malloc(sizeof(co_scope_stack));
    if (!s) return NULL;
    s->frames = NULL;
    s->top = 0;
    s->cap = 0;
    return s;
}

void come_scope_stack_activate(void *stack) {
    co_scope_active = stack ? (co_scope_stack *)stack : &co_scope_main;
}

void *come_scope_stack_active(void) {
    return co_scope_active;
}

void come_scope_stack_free(void *stack) {
    co_scope_stack *s = (co_scope_stack *)stack;
    if (!s || s == &co_scope_main) return;
    /* A finished coroutine never resumes, so frames left
       on its stack are dead: free their contexts too. */
    co_scope_stack *saved = co_scope_active;
    co_scope_active = s;
    while (s->top > 0) come_scope_pop();
    co_scope_active = saved;
    free(s->frames);
    free(s);
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
