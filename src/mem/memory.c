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

#include <stddef.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
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

void mem_talloc_module_shutdown(void) {
    if (co_mem_root)
        co_free_chunk(co_from_ptr(co_mem_root));
    co_mem_root = NULL;
}

void *mem_talloc_alloc(void *ctx, size_t size) {
    if (!co_mem_root)
        mem_talloc_module_init();
    co_chunk *parent = ctx ? co_from_ptr(ctx) : co_root_chunk();
    co_chunk *chunk = co_alloc_chunk(parent, size);
    return chunk ? co_to_ptr(chunk) : NULL;
}

void *mem_talloc_realloc(void *ctx, void *ptr, size_t size) {
    if (!ptr)
        return mem_talloc_alloc(ctx, size);
    if (size == 0) {
        co_free_chunk(co_from_ptr(ptr));
        return NULL;
    }
    return co_realloc_chunk(co_from_ptr(ptr), size);
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
