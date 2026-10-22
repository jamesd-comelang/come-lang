// COIL-120: the coroutine scheduler in isolation (C level).
// Two coroutines interleave deterministically under yield,
// each runs with its own memory context installed, and both
// run to completion.
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "come_coro.h"
#include "come_link.h"
#include "come_string.h"
#include "mem/talloc.h"

static char trace[256];
static int trace_len = 0;
static void note(char c) { trace[trace_len++] = c; trace[trace_len] = 0; }

// A stand-in for a module's context global (come_<module>__ctx).
static TALLOC_CTX* fake_ctx = NULL;
static void* seen_ctx[3];
static int seen_count[3];

static void child_worker(void* p);
static void parent_worker(void* p);
static void check_worker(void* p);
static int check_seen_ok;
static void link_worker(void* p);
static void late_writer(void* p);
typedef struct { come_link_t* s; } link_job_t;
static char link_seen[32];

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

    // COIL-121: a coroutine spawning a coroutine - the whole
    // tree runs to completion.
    trace_len = 0; trace[0] = 0;
    come_coro_t* cp = come_coro_create(parent_worker, &fake_ctx);
    if (!cp) { printf("FAIL: create parent\n"); return 1; }
    come_coro_start(cp);
    for (int i = 0; i < 6 && come_coro_alive() > 0; i++) {
        come_coro_yield();
    }
    if (come_coro_alive() != 0) { printf("FAIL: tree alive = %d\n", come_coro_alive()); return 1; }
    if (strcmp(trace, "pcP") != 0) {
        printf("FAIL: tree trace = %s, want pcP\n", trace);
        return 1;
    }

    // COIL-122: ownership transfer on spawn. The buffer starts
    // owned by the spawner; the spawn sequence steals it into
    // the coroutine's args block; freeing the spawner's context
    // afterwards must not harm it, and the coroutine reads the
    // intact contents.
    {
        void* spawner_ctx = mem_talloc_new_ctx(NULL);
        come_string_t* payload = come_string_new(spawner_ctx, "owned-by-coroutine");
        come_coro_t* cw = come_coro_create(check_worker, &fake_ctx);
        char** slot = (char**)come_coro_args(cw, sizeof(char*));
        *slot = (char*)payload;
        mem_talloc_steal(slot, payload); // what spawn emits
        mem_talloc_free(spawner_ctx);    // spawner lets go
        come_coro_start(cw);
        for (int i = 0; i < 4 && come_coro_alive() > 0; i++) {
            come_coro_yield();
        }
        if (come_coro_alive() != 0) { printf("FAIL: check worker alive\n"); return 1; }
        if (!check_seen_ok) { printf("FAIL: transferred buffer corrupted\n"); return 1; }
    }

    // COIL-123: a link moves data with ownership attached, and
    // writing after the peer finished fails with EPIPE.
    {
        come_link_t *la, *lb;
        come_colink(&la, &lb);
        come_coro_t* cw2 = come_coro_create(link_worker, &fake_ctx);
        link_job_t* lj = (link_job_t*)come_coro_args(cw2, sizeof(link_job_t));
        if (lj) lj->s = la;
        come_link_adopt(la, cw2);
        come_coro_start(cw2);
        come_link_write(lb, come_string_new(fake_ctx, "ping"));
        come_string_t* rep = (come_string_t*)come_link_read(lb, fake_ctx);
        if (!rep || strcmp(rep->data, "pong") != 0) { printf("FAIL: link reply\n"); return 1; }
        if (strcmp(link_seen, "ping") != 0) { printf("FAIL: link request '%s'\n", link_seen); return 1; }
        // The worker has finished; its end closed automatically.
        errno = 0;
        int ok = come_link_write(lb, come_string_new(fake_ctx, "late"));
        if (ok != 0 || errno != EPIPE) { printf("FAIL: write-after-close ok=%d errno=%d\n", ok, errno); return 1; }
        // Reads now report end-of-data: an empty buffer.
        come_string_t* eof = (come_string_t*)come_link_read(lb, fake_ctx);
        if (!eof || eof->count != 0) { printf("FAIL: link EOF\n"); return 1; }
    }

    // COIL-124: wait returns the ready end at once when data is
    // already queued, and blocks until a writer arrives
    // otherwise.
    {
        come_link_t *wa, *wb, *wc, *wd;
        come_colink(&wa, &wb);
        come_colink(&wc, &wd);
        come_link_write(wa, come_string_new(fake_ctx, "early"));
        come_link_t* set1[2] = { wb, wd };
        if (come_link_wait(set1, 2) != wb) { printf("FAIL: wait immediate identity\n"); return 1; }
        come_string_t* early = (come_string_t*)come_link_read(wb, fake_ctx);
        if (!early || strcmp(early->data, "early") != 0) { printf("FAIL: wait immediate data\n"); return 1; }
        come_coro_t* ww = come_coro_create(late_writer, &fake_ctx);
        link_job_t* lj2 = (link_job_t*)come_coro_args(ww, sizeof(link_job_t));
        if (lj2) lj2->s = wc;
        come_link_adopt(wc, ww);
        come_coro_start(ww);
        come_link_t* set2[2] = { wb, wd };
        if (come_link_wait(set2, 2) != wd) { printf("FAIL: wait blocking identity\n"); return 1; }
        come_string_t* late = (come_string_t*)come_link_read(wd, fake_ctx);
        if (!late || strcmp(late->data, "late") != 0) { printf("FAIL: wait blocking data\n"); return 1; }
    }

    printf("COIL-120/121 coro runtime test passed!\n");
    return 0;
}

static void late_writer(void* p) {
    come_link_t* s = ((link_job_t*)p)->s;
    come_coro_yield();
    come_link_write(s, come_string_new(fake_ctx, "late"));
}

static void link_worker(void* p) {
    come_link_t* s = ((link_job_t*)p)->s;
    come_string_t* req = (come_string_t*)come_link_read(s, fake_ctx);
    if (req) {
        strncpy(link_seen, req->data, sizeof(link_seen) - 1);
    }
    come_link_write(s, come_string_new(fake_ctx, "pong"));
}

static int check_seen_ok = 0;

static void check_worker(void* p) {
    // p is the args block; its first field is the payload,
    // exactly as a generated entry trampoline unpacks it.
    char** slot = (char**)p;
    come_string_t* s = (come_string_t*)*slot;
    if (s && strcmp(s->data, "owned-by-coroutine") == 0) check_seen_ok = 1;
}

static void child_worker(void* p) {
    (void)p;
    note('c');
}

static void parent_worker(void* p) {
    (void)p;
    note('p');
    come_coro_t* cc = come_coro_create(child_worker, &fake_ctx);
    if (cc) come_coro_start(cc);
    come_coro_yield();
    note('P');
}
