#ifndef COME_CORO_H
#define COME_CORO_H

// Coroutine runtime (spec 9): cooperative, single-threaded,
// stackful coroutines. Coroutines share nothing: each one runs
// with its own stack and its own memory contexts (one per
// registered module context slot), which are freed when the
// coroutine ends.

#include <stddef.h>
#include "mem/talloc.h"

typedef void TALLOC_CTX;

typedef struct come_coro come_coro_t;
typedef void (*come_coro_fn)(void* args);

// Called once by the generated C main before module init.
void come_coro_init(void);

// Each module registers its context global at init time; the
// scheduler swaps every registered slot on a context switch so
// a coroutine's allocations land in its own contexts.
void come_coro_register_ctx(TALLOC_CTX** slot);

// Create a coroutine for fn. home_slot identifies the module
// the coroutine belongs to (its context slot), used as the
// parent for the argument block. The coroutine does not run
// until come_coro_start and a later yield/block by someone else.
come_coro_t* come_coro_create(come_coro_fn fn, TALLOC_CTX** home_slot);

// The argument block for a created coroutine, allocated in the
// coroutine's own context; generated code fills it in before
// come_coro_start.
void* come_coro_args(come_coro_t* co, size_t size);

// Make a created coroutine runnable.
void come_coro_start(come_coro_t* co);

// Give up the rest of this turn: the next runnable coroutine
// (round-robin) runs. A no-op when no other coroutine can run.
void come_coro_yield(void);

// Number of coroutines not yet finished (excluding main).
int come_coro_alive(void);

#endif
