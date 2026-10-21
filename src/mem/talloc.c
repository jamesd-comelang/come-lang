#include "mem/talloc.h"
#include <stdio.h>

#ifdef _WIN32
#include <stdlib.h>
#include <stdint.h>

typedef struct mem_record {
    void* ptr;
    void* parent;
    struct mem_record* next;
} mem_record_t;

static mem_record_t* mem_records = NULL;

static mem_record_t* mem_find_record(void* ptr) {
    for (mem_record_t* rec = mem_records; rec; rec = rec->next) {
        if (rec->ptr == ptr) return rec;
    }
    return NULL;
}

static void mem_track(void* ptr, void* parent) {
    if (!ptr) return;
    mem_record_t* rec = mem_find_record(ptr);
    if (!rec) {
        rec = (mem_record_t*)calloc(1, sizeof(mem_record_t));
        if (!rec) return;
        rec->ptr = ptr;
        rec->next = mem_records;
        mem_records = rec;
    }
    rec->parent = parent;
}

static void mem_untrack(void* ptr) {
    mem_record_t** link = &mem_records;
    while (*link) {
        if ((*link)->ptr == ptr) {
            mem_record_t* victim = *link;
            *link = victim->next;
            free(victim);
            return;
        }
        link = &(*link)->next;
    }
}

void mem_talloc_module_init(void) {
}

void mem_talloc_module_shutdown(void) {
    while (mem_records) {
        mem_record_t* next = mem_records->next;
        free(mem_records);
        mem_records = next;
    }
}

void* mem_talloc_alloc(void* ctx, size_t size) {
    void* ptr = calloc(1, size);
    mem_track(ptr, ctx);
    return ptr;
}

void* mem_talloc_realloc(void* ctx, void* ptr, size_t size) {
    void* parent = ctx;
    mem_record_t* rec = mem_find_record(ptr);
    if (!parent && rec) parent = rec->parent;
    uintptr_t old_addr = (uintptr_t)ptr;
    void* new_ptr = realloc(ptr, size);
    if (!new_ptr) return NULL;
    if (rec) {
        rec->ptr = new_ptr;
        rec->parent = parent;
    } else {
        mem_track(new_ptr, parent);
    }
    if ((uintptr_t)new_ptr != old_addr) {
        for (mem_record_t* child = mem_records; child; child = child->next) {
            if ((uintptr_t)child->parent == old_addr) child->parent = new_ptr;
        }
    }
    return new_ptr;
}

void mem_talloc_free(void* ptr) {
    if (!ptr) return;
    for (;;) {
        void* child = NULL;
        for (mem_record_t* rec = mem_records; rec; rec = rec->next) {
            if (rec->parent == ptr) {
                child = rec->ptr;
                break;
            }
        }
        if (!child) break;
        mem_talloc_free(child);
    }
    mem_untrack(ptr);
    free(ptr);
}

void* mem_talloc_new_ctx(void* parent) {
    void* ptr = calloc(1, 1);
    mem_track(ptr, parent);
    return ptr;
}

void* mem_talloc_steal(void* new_ctx, void* ptr) {
    mem_track(ptr, new_ctx);
    return ptr;
}

void* mem_talloc_parent(void* ptr) {
    mem_record_t* rec = mem_find_record(ptr);
    return rec ? rec->parent : NULL;
}

#else
#include "talloc.h"   // from src/external/talloc/include

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

void* mem_talloc_parent(void* ptr) {
    if (!ptr) return NULL;
    return talloc_parent(ptr);
}
#endif
