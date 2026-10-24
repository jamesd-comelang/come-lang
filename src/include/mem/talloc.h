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

// Scope contexts (COIL-157, spec 7): generated code runs
// inside a stack of scope contexts - one frame per function
// body (a marker) and per nested block, loop iterations
// included. COME_CTX resolves to the top of the active
// stack, so a buffer a scope creates dies when the scope
// exits instead of living until program end. The stack is
// per coroutine: the scheduler swaps the active stack on
// a switch, so a coroutine suspended inside a scope keeps
// its frames intact while another runs.
void* come_scope_current(void);
void come_scope_push(void);
void come_scope_push_checkpoint(void);
void come_scope_push_marker(void);
void come_scope_pop(void);
void come_scope_pop_to_marker(void);
void come_scope_pop_to_depth(int lexical_depth);
void* come_scope_caller_ctx(void);
void* come_scope_ctx_for_depth(int lexical_depth);
// Move a buffer's block to dst when dst outlives the
// buffer's current scope; otherwise leave it alone.
// Returns 1 when the block moved (the caller then updates
// the header's ctx), 0 when nothing changed.
int come_scope_adopt(void* dst, void* data);
// Bump arenas (Design 2, redesign): temporary values that
// die with their scope frame. Allocations never move, so a
// pointer held by an earlier temporary in the same statement
// stays valid across later allocations.
void* come_arena_alloc(size_t size);
int come_arena_contains(const void* ptr);
// Opaque per-coroutine scope stacks, owned by the
// coroutine runtime (coro.c): create, activate, and reap.
void* come_scope_stack_new(void);
void come_scope_stack_activate(void* stack);
void* come_scope_stack_active(void);
void come_scope_stack_free(void* stack);

// Error reporting (spec 12, COIL-118): the runtime records
// failures as an errno code plus, where the generic strerror
// text would mislead, a specific message. ERR.str() prefers
// the specific message when it matches the current code.
// Implemented in memory.c because every runtime links it.
void come_ERR_set(int code, const char* msg);
const char* come_ERR_message_for(int code);
void come_ERR_clear_custom(void);

#ifdef __cplusplus
}
#endif

#endif
