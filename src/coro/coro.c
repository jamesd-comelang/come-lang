// Coroutine runtime (spec 9, COIL-120).
//
// One OS thread, stackful coroutines via ucontext: each
// coroutine gets its own stack and its own set of memory
// contexts. Scheduling is cooperative and round-robin: a
// coroutine runs until it yields, blocks, or returns.
//
// Memory isolation: generated code allocates through per-module
// context globals (come_<module>__ctx). Modules register those
// globals at init; on every context switch the scheduler
// installs the incoming coroutine's contexts, so allocations
// made while a coroutine runs belong to that coroutine and are
// freed when it ends (spec 9.3).

#include "come_coro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

#define COME_CORO_MAX 256
#define COME_CORO_MAX_CTX 32
#define COME_CORO_STACK_SIZE (256 * 1024)

enum {
    CS_NEW,     // created, not yet started
    CS_READY,   // runnable
    CS_RUNNING, // on the CPU now (exactly one)
    CS_BLOCKED, // waiting to be woken (a channel read)
    CS_DONE,    // returned; stack/contexts not yet reaped
    CS_FREE     // reaped
};

struct come_coro {
    ucontext_t uc;
    char* stack;
    int state;
    come_coro_fn fn;
    void* args;
    int home_slot; // module slot the argument block lives in
    // This coroutine's context for each registered module slot.
    TALLOC_CTX* own[COME_CORO_MAX_CTX];
};

static come_coro_t g_coros[COME_CORO_MAX];
static int g_coro_count = 0;   // slot 0 is main
static int g_current = 0;
static int g_inited = 0;
static int g_main_ctx_known = 0;

static TALLOC_CTX** g_ctx_slots[COME_CORO_MAX_CTX];
static TALLOC_CTX* g_ctx_base[COME_CORO_MAX_CTX];
static int g_ctx_slot_count = 0;

void come_coro_register_ctx(TALLOC_CTX** slot) {
    for (int i = 0; i < g_ctx_slot_count; i++) {
        if (g_ctx_slots[i] == slot) return;
    }
    if (g_ctx_slot_count >= COME_CORO_MAX_CTX) return;
    g_ctx_slots[g_ctx_slot_count] = slot;
    g_ctx_base[g_ctx_slot_count] = *slot;
    g_ctx_slot_count++;
}

void come_coro_init(void) {
    if (g_inited) return;
    g_inited = 1;
    memset(g_coros, 0, sizeof(g_coros));
    g_coro_count = 1;
    g_current = 0;
    g_coros[0].state = CS_RUNNING;
}

// Install coroutine `id`'s contexts into the registered slots.
// Main's contexts are the registered base values, captured the
// first time main is switched out.
static void install_ctxs(int id) {
    come_coro_t* co = &g_coros[id];
    if (id == 0 && !g_main_ctx_known) {
        for (int i = 0; i < g_ctx_slot_count; i++) co->own[i] = g_ctx_base[i];
        g_main_ctx_known = 1;
    }
    for (int i = 0; i < g_ctx_slot_count; i++) {
        if (co->own[i]) *g_ctx_slots[i] = co->own[i];
    }
}

// Free finished coroutines: their stacks and contexts. Never
// frees the currently running coroutine (a coroutine cannot
// free the stack it is standing on; the next one reaps it).
static void reap_done(void) {
    for (int i = 1; i < g_coro_count; i++) {
        come_coro_t* co = &g_coros[i];
        if (co->state != CS_DONE) continue;
        co->state = CS_FREE;
        free(co->stack);
        co->stack = NULL;
        for (int s = 0; s < g_ctx_slot_count; s++) {
            if (co->own[s]) {
                mem_talloc_free(co->own[s]);
                co->own[s] = NULL;
            }
        }
        co->args = NULL;
    }
}

int come_coro_alive(void) {
    int n = 0;
    for (int i = 1; i < g_coro_count; i++) {
        int st = g_coros[i].state;
        if (st == CS_NEW || st == CS_READY || st == CS_RUNNING) n++;
    }
    return n;
}

// Switch from the current coroutine to `next` (already chosen).
static void switch_to(int next) {
    int prev = g_current;
    if (next == prev) return;
    g_coros[prev].state = (g_coros[prev].state == CS_RUNNING) ? CS_READY : g_coros[prev].state;
    g_coros[next].state = CS_RUNNING;
    g_current = next;
    install_ctxs(next);
    swapcontext(&g_coros[prev].uc, &g_coros[next].uc);
    // The previous coroutine resumes here when it is switched
    // back in; reap anything that finished meanwhile.
    reap_done();
}

// Next runnable coroutine after `from`, round-robin; `from`
// itself if no other coroutine can run.
static int next_ready(int from) {
    for (int step = 1; step <= g_coro_count; step++) {
        int idx = (from + step) % g_coro_count;
        if (idx == from) continue;
        if (g_coros[idx].state == CS_READY) return idx;
    }
    return from;
}

void come_coro_yield(void) {
    if (!g_inited) return;
    int next = next_ready(g_current);
    if (next == g_current) {
        reap_done();
        return;
    }
    switch_to(next);
}

come_coro_t* come_coro_current(void) {
    if (!g_inited) come_coro_init();
    return &g_coros[g_current];
}

// No coroutine can ever run again: every unfinished one is
// blocked waiting for data nobody will send. Report it and
// stop instead of hanging (spec 9.3; COIL-125).
static void coro_deadlock(void) {
    int blocked = 0;
    for (int i = 0; i < g_coro_count; i++) {
        if (g_coros[i].state == CS_BLOCKED) blocked++;
    }
    fprintf(stderr, "deadlock: no coroutine can run (%d blocked)\n", blocked);
    exit(1);
}

void come_coro_block(void) {
    if (!g_inited) return;
    int self = g_current;
    g_coros[self].state = CS_BLOCKED;
    int next = next_ready(self);
    if (next == self) coro_deadlock();
    switch_to(next);
}

void come_coro_wake(come_coro_t* co) {
    if (co && co->state == CS_BLOCKED) co->state = CS_READY;
}

#define COME_CORO_MAX_HOOKS 8
static come_coro_finish_hook g_hooks[COME_CORO_MAX_HOOKS];
static int g_hook_count = 0;

void come_coro_add_finish_hook(come_coro_finish_hook hook) {
    if (g_hook_count < COME_CORO_MAX_HOOKS) g_hooks[g_hook_count++] = hook;
}

static void coro_trampoline(int id) {
    come_coro_t* co = &g_coros[id];
    co->fn(co->args);
    // Finish hooks (a coroutine's links close here, spec 9.3)
    // run while this coroutine is still the current one.
    for (int i = 0; i < g_hook_count; i++) g_hooks[i](co);
    co->state = CS_DONE;
    // A finished coroutine never runs again: switch away and
    // let the coroutine we land on reap this one's stack.
    int next = next_ready(id);
    if (next == id) coro_deadlock();
    switch_to(next);
    // Unreachable: DONE coroutines are never scheduled.
    fprintf(stderr, "coroutine: finished coroutine resumed\n");
    abort();
}

come_coro_t* come_coro_create(come_coro_fn fn, TALLOC_CTX** home_slot) {
    if (!g_inited) come_coro_init();
    if (g_coro_count >= COME_CORO_MAX) {
        fprintf(stderr, "coroutine: too many coroutines (max %d)\n", COME_CORO_MAX);
        return NULL;
    }
    int id = g_coro_count++;
    come_coro_t* co = &g_coros[id];
    memset(co, 0, sizeof(*co));
    co->state = CS_NEW;
    co->fn = fn;
    co->home_slot = 0;
    for (int i = 0; i < g_ctx_slot_count; i++) {
        if (g_ctx_slots[i] == home_slot) { co->home_slot = i; break; }
    }
    for (int i = 0; i < g_ctx_slot_count; i++) {
        co->own[i] = mem_talloc_new_ctx(g_ctx_base[i]);
    }
    co->stack = (char*)malloc(COME_CORO_STACK_SIZE);
    if (!co->stack) {
        fprintf(stderr, "coroutine: out of memory\n");
        co->state = CS_FREE;
        return NULL;
    }
    getcontext(&co->uc);
    co->uc.uc_stack.ss_sp = co->stack;
    co->uc.uc_stack.ss_size = COME_CORO_STACK_SIZE;
    co->uc.uc_stack.ss_flags = 0;
    co->uc.uc_link = NULL;
    makecontext(&co->uc, (void (*)(void))coro_trampoline, 1, id);
    return co;
}

void* come_coro_args(come_coro_t* co, size_t size) {
    if (!co) return NULL;
    // The argument block belongs to the coroutine, in its
    // home module's context.
    TALLOC_CTX* parent = (co->home_slot < g_ctx_slot_count)
                             ? co->own[co->home_slot]
                             : NULL;
    void* block = mem_talloc_alloc(parent, size);
    if (block) memset(block, 0, size);
    co->args = block;
    return block;
}

void come_coro_start(come_coro_t* co) {
    if (co && co->state == CS_NEW) co->state = CS_READY;
}
