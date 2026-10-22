// Link runtime (spec 9.2, COIL-123).
//
// A channel is two message queues (one per direction) with a
// context of its own: a written buffer is stolen into the
// channel's context, and stolen out again into the reader's
// context on read. Ownership therefore travels with the data
// and no buffer is ever shared between coroutines.
//
// A read on an empty queue blocks the reading coroutine until
// the peer writes or closes. When a coroutine finishes, the
// scheduler's finish hook closes every end it owns, so a
// reader on the other side sees end-of-data instead of
// waiting forever (spec 9.3).

#include "come_link.h"
#include "come_string.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

typedef struct come_msg {
    void* buf;
    struct come_msg* next;
} come_msg_t;

struct come_link {
    struct come_chan* chan;
    int side;              // 0 or 1
    int closed;            // this end is closed
    come_coro_t* owner;    // coroutine that closes it by finishing
};

typedef struct come_chan {
    come_msg_t* qhead[2];  // messages waiting FOR side i
    come_msg_t* qtail[2];
    int end_closed[2];
    come_coro_t* waiter[2]; // coroutine blocked reading side i
    come_coro_t* any_waiter[2]; // coroutine blocked in wait() on side i
    struct come_link ends[2];
    TALLOC_CTX* ctx;
    struct come_chan* next_all;
} come_chan_t;

// Every channel ever made, for the finish hook. Channel
// contexts live for the program's duration in v1 - bounded by
// the number of colink() calls a program makes.
static come_chan_t* g_all_chans = NULL;
static int g_hook_registered = 0;

static void link_finish_hook(come_coro_t* co) {
    for (come_chan_t* c = g_all_chans; c; c = c->next_all) {
        for (int i = 0; i < 2; i++) {
            if (c->ends[i].owner == co && !c->ends[i].closed) {
                come_link_close(&c->ends[i]);
            }
        }
    }
}

void come_colink(come_link_t** a, come_link_t** b) {
    if (!g_hook_registered) {
        g_hook_registered = 1;
        come_coro_add_finish_hook(link_finish_hook);
    }
    TALLOC_CTX* ctx = mem_talloc_new_ctx(NULL);
    come_chan_t* c = (come_chan_t*)mem_talloc_alloc(ctx, sizeof(come_chan_t));
    memset(c, 0, sizeof(*c));
    c->ctx = ctx;
    for (int i = 0; i < 2; i++) {
        c->ends[i].chan = c;
        c->ends[i].side = i;
        c->ends[i].closed = 0;
        c->ends[i].owner = come_coro_current();
    }
    c->next_all = g_all_chans;
    g_all_chans = c;
    *a = &c->ends[0];
    *b = &c->ends[1];
}

void come_link_adopt(come_link_t* end, come_coro_t* co) {
    if (end) end->owner = co;
}

// 1 when p is one of the channel ends in the registry.
static int come_link_is_end(void* p) {
    for (come_chan_t* c = g_all_chans; c; c = c->next_all) {
        if (p == (void*)&c->ends[0] || p == (void*)&c->ends[1]) return 1;
    }
    return 0;
}

int come_link_write(come_link_t* end, void* buf) {
    if (!end) { come_ERR_set(EBADF, "write on a null link"); return 0; }
    come_chan_t* c = end->chan;
    int peer = 1 - end->side;
    if (end->closed) {
        come_ERR_set(EPIPE, "link is closed");
        return 0;
    }
    if (c->end_closed[peer]) {
        come_ERR_set(EPIPE, "link peer closed");
        return 0;
    }
    // The buffer's ownership moves into the channel. A link
    // end sent through a channel is the one exception: it is
    // not a heap buffer but a handle inside its own channel,
    // so there is nothing to steal - the handle just travels.
    if (buf && !come_link_is_end(buf)) mem_talloc_steal(c->ctx, buf);
    come_msg_t* m = (come_msg_t*)mem_talloc_alloc(c->ctx, sizeof(come_msg_t));
    m->buf = buf;
    m->next = NULL;
    if (c->qtail[peer]) c->qtail[peer]->next = m;
    else c->qhead[peer] = m;
    c->qtail[peer] = m;
    if (c->waiter[peer]) come_coro_wake(c->waiter[peer]);
    if (c->any_waiter[peer]) come_coro_wake(c->any_waiter[peer]);
    return 1;
}

void* come_link_read(come_link_t* end, TALLOC_CTX* reader_ctx) {
    if (!end) { come_ERR_set(EBADF, "read on a null link"); return come_string_new(reader_ctx, ""); }
    come_chan_t* c = end->chan;
    int me = end->side;
    int peer = 1 - me;
    for (;;) {
        if (c->qhead[me]) {
            come_msg_t* m = c->qhead[me];
            c->qhead[me] = m->next;
            if (!c->qhead[me]) c->qtail[me] = NULL;
            void* buf = m->buf;
            // Ownership passes to the reader (link ends
            // excepted, as in come_link_write).
            if (buf && !come_link_is_end(buf)) mem_talloc_steal(reader_ctx, buf);
            return buf;
        }
        if (c->end_closed[peer]) {
            // End of data: an empty buffer, never null.
            return come_string_new(reader_ctx, "");
        }
        c->waiter[me] = come_coro_current();
        come_coro_block();
        c->waiter[me] = NULL;
    }
}

void come_link_close(come_link_t* end) {
    if (!end || end->closed) return;
    come_chan_t* c = end->chan;
    end->closed = 1;
    c->end_closed[end->side] = 1;
    int peer = 1 - end->side;
    if (c->waiter[peer]) come_coro_wake(c->waiter[peer]);
    if (c->any_waiter[peer]) come_coro_wake(c->any_waiter[peer]);
}

int come_link_ready(come_link_t* end) {
    if (!end) return 1;
    come_chan_t* c = end->chan;
    return c->qhead[end->side] != NULL || c->end_closed[1 - end->side];
}

come_link_t* come_link_wait(come_link_t** ends, int n) {
    if (n <= 0) return NULL;
    for (;;) {
        for (int i = 0; i < n; i++) {
            if (come_link_ready(ends[i])) return ends[i];
        }
        // Nothing ready: register on every channel, then
        // block. A write or a close on any of them wakes us;
        // the loop then re-scans from the top (spec 9.2: if a
        // link is already readable, wait returns immediately -
        // the scan above is that fast path).
        come_coro_t* me = come_coro_current();
        for (int i = 0; i < n; i++) {
            ends[i]->chan->any_waiter[ends[i]->side] = me;
        }
        come_coro_block();
        for (int i = 0; i < n; i++) {
            if (ends[i]->chan->any_waiter[ends[i]->side] == me) {
                ends[i]->chan->any_waiter[ends[i]->side] = NULL;
            }
        }
    }
}
