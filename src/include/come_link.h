#ifndef COME_LINK_H
#define COME_LINK_H

// Links: the two ends of a coroutine channel (spec 9.2, 6.7).
// A link carries headered buffers (strings, arrays) from one
// coroutine to another, with ownership attached: write moves
// the buffer into the channel, read takes it out into the
// reader's context. Nothing is copied or shared.

#include "come_coro.h"

typedef struct come_link come_link_t;

// Create a channel and its two ends.
void come_colink(come_link_t** a, come_link_t** b);

// Move buf into the channel for the peer to read. Returns 1 on
// success; on a closed peer the buffer stays with the caller
// and ERR is set (spec 9.2).
int come_link_write(come_link_t* end, void* buf);

// Take the next buffer written by the peer, waiting until one
// arrives. Once the peer has closed and the queue is drained,
// returns an empty string: end of data, never null.
void* come_link_read(come_link_t* end, TALLOC_CTX* reader_ctx);

// Close this end. The peer's reads drain what is left, then
// see end-of-data. A finished coroutine's ends close on their
// own (spec 9.3).
void come_link_close(come_link_t* end);

// Record that a coroutine now owns this end (spawn does this
// for link arguments): its end closes when it finishes.
void come_link_adopt(come_link_t* end, come_coro_t* co);

// 1 when a read would not wait: data queued, or the peer has
// closed. Used by wait() (spec 9.2).
int come_link_ready(come_link_t* end);

#endif
