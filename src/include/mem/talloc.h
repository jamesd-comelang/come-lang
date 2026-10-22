#ifndef MEM_TALLOC_H
#define MEM_TALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void mem_talloc_module_init(void);
void mem_talloc_module_shutdown(void);

// Module lifecycle registry (spec 3.5): __init registers __exit;
// the entry point runs them in exact reverse init order.
void come_register_exit(void (*fn)(void));
void come_run_exits(void);

void* mem_talloc_alloc(void* ctx, size_t size);
void* mem_talloc_realloc(void* ctx, void* ptr, size_t size);
void mem_talloc_free(void* ptr);
void* mem_talloc_new_ctx(void* parent);
void* mem_talloc_steal(void* new_ctx, void* ptr);

#ifdef __cplusplus
}
#endif

#endif
