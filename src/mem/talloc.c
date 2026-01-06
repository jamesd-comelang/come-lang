#include "mem/talloc.h"
#include "talloc.h"   // from external/talloc/include
#include <stdio.h>

static void* co_mem_root = NULL;

void mem_talloc_module_init(void) {
    co_mem_root = talloc_new(NULL);
    if (!co_mem_root) {
        fprintf(stderr, "talloc: failed to create root context\n");
    }
}

void mem_talloc_module_shutdown(void) {
    if (co_mem_root)
        talloc_free(co_mem_root);
    co_mem_root = NULL;
}

void* mem_talloc_alloc(void* ctx, size_t size) {
    if (!co_mem_root) mem_talloc_module_init();
    if (!ctx) ctx = co_mem_root;
    return talloc_size(ctx, size);
}

void* mem_talloc_realloc(void* ctx, void* ptr, size_t size) {
    if (!co_mem_root) mem_talloc_module_init();
    if (!ctx) ctx = co_mem_root;
    return talloc_realloc_size(ctx, ptr, size);
}

void mem_talloc_free(void* ptr) {
    if (ptr)
        talloc_free(ptr);
}

void* mem_talloc_new_ctx(void* parent) {
    if (!co_mem_root) mem_talloc_module_init();
    if (!parent) parent = co_mem_root;  // default parent is global root
    void* ctx = talloc_new(parent);
    if (!ctx) {
        fprintf(stderr, "talloc: failed to create new context\n");
    }
    return ctx;
}

void* mem_talloc_steal(void* new_ctx, void* ptr) {
    if (!co_mem_root) mem_talloc_module_init();
    if (!new_ctx) new_ctx = co_mem_root;
    return talloc_steal(new_ctx, ptr);
}
