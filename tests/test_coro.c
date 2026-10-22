// COIL-120: the coroutine scheduler in isolation (C level).
// Two coroutines interleave deterministically under yield,
// each runs with its own memory context installed, and both
// run to completion.
#include <stdio.h>
#include <string.h>
#include "come_coro.h"
#include "mem/talloc.h"

static char trace[256];
static int trace_len = 0;
static void note(char c) { trace[trace_len++] = c; trace[trace_len] = 0; }

// A stand-in for a module's context global (come_<module>__ctx).
static TALLOC_CTX* fake_ctx = NULL;
static void* seen_ctx[3];
static int seen_count[3];

typedef struct { int id; char tag; int turns; } job_t;

static void worker(void* p) {
    job_t* j = (job_t*)p;
    for (int i = 0; i < j->turns; i++) {
        note(j->tag);
        // While this coroutine runs, the registered slot must
        // hold THIS coroutine's context, not another's.
        seen_ctx[j->id] = fake_ctx;
        seen_count[j->id]++;
        come_coro_yield();
    }
}

int main(void) {
    fake_ctx = mem_talloc_new_ctx(NULL);
    come_coro_init();
    come_coro_register_ctx(&fake_ctx);
    void* main_ctx = fake_ctx;

    come_coro_t* ca = come_coro_create(worker, &fake_ctx);
    come_coro_t* cb = come_coro_create(worker, &fake_ctx);
    // Fill the args blocks the way generated code does.
    job_t* ja = (job_t*)come_coro_args(ca, sizeof(job_t));
    job_t* jb = (job_t*)come_coro_args(cb, sizeof(job_t));
    if (ja) { ja->id = 1; ja->tag = 'A'; ja->turns = 3; }
    if (jb) { jb->id = 2; jb->tag = 'B'; jb->turns = 3; }
    if (!ca || !cb) { printf("FAIL: create\n"); return 1; }
    // Created coroutines count as alive: they have work to do
    // whether or not they have been started yet.
    if (come_coro_alive() != 2) { printf("FAIL: alive after create = %d\n", come_coro_alive()); return 1; }
    come_coro_start(ca);
    come_coro_start(cb);
    if (come_coro_alive() != 2) { printf("FAIL: alive after start = %d\n", come_coro_alive()); return 1; }

    // Deterministic round-robin: main, A, B, main, A, B, ...
    for (int i = 0; i < 3; i++) {
        note('M');
        come_coro_yield();
    }
    if (strcmp(trace, "MABMABMAB") != 0) {
        printf("FAIL: trace = %s, want MABMABMAB\n", trace);
        return 1;
    }
    // One more yield lets the last coroutine return and be reaped.
    come_coro_yield();
    if (come_coro_alive() != 0) { printf("FAIL: alive at end = %d\n", come_coro_alive()); return 1; }

    // Each coroutine saw its own context in the slot, every turn,
    // and the two contexts differ from each other and main's.
    if (seen_count[1] != 3 || seen_count[2] != 3) { printf("FAIL: turn counts\n"); return 1; }
    if (!seen_ctx[1] || !seen_ctx[2]) { printf("FAIL: ctx not installed\n"); return 1; }
    if (seen_ctx[1] == seen_ctx[2]) { printf("FAIL: coroutines share a context\n"); return 1; }
    if (seen_ctx[1] == main_ctx || seen_ctx[2] == main_ctx) { printf("FAIL: coroutine uses main's context\n"); return 1; }
    if (fake_ctx != main_ctx) { printf("FAIL: main's context not restored\n"); return 1; }

    // A yield with nothing else to run is a no-op.
    come_coro_yield();

    printf("COIL-120 coro runtime test passed!\n");
    return 0;
}
