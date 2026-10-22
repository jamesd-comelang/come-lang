#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "come_array.h"
#include "come_string.h"
#include "mem/talloc.h"

// Out-of-bounds scratch slots (COIL-106). Every call zeroes the slot
// and records the error, so an OOB read sees the zero value and an
// OOB write is discarded (the next call wipes it).
int* come_oob_slot_int(void) {
    static int slot;
    slot = 0;
    come_ERR_set(ERANGE, "array index out of bounds");
    return &slot;
}

uint8_t* come_oob_slot_byte(void) {
    static uint8_t slot;
    slot = 0;
    come_ERR_set(ERANGE, "array index out of bounds");
    return &slot;
}

void* come_oob_slot_bytes(void) {
    static uint8_t slot[4096];
    memset(slot, 0, sizeof(slot));
    come_ERR_set(ERANGE, "array index out of bounds");
    return slot;
}

/* ---- Uniform header operations (COIL-135) -----------------------
 * Every buffer kind shares the header layout {data, count, cap,
 * ctx}, kept inline in the variable. Mutating operations take the
 * address of the header slot. Growth allocates a FRESH data block in
 * the header's owner context and never frees or moves the old one:
 * an alias that copied the header earlier keeps pointing at the old
 * block, which stays valid until the owner context dies. */

// Ensure room for at least mincap elements, growing by doubling.
void come_array_ensure(come_hdr_t* h, size_t esz, uint32_t mincap) {
    if (!h || h->cap >= mincap) return;
    uint32_t ncap = h->cap ? h->cap : 4;
    while (ncap < mincap) ncap *= 2;
    void* nd = mem_talloc_alloc(h->ctx, esz * ncap);
    if (!nd) return;
    if (h->data && h->count)
        memcpy(nd, h->data, (size_t)h->count * esz);
    memset((char*)nd + (size_t)h->count * esz, 0,
           (size_t)(ncap - h->count) * esz);
    h->data = nd;
    h->cap = ncap;
}

void come_array_resize_g(come_hdr_t* h, size_t esz, uint32_t n) {
    if (!h) return;
    if (n > h->cap) come_array_ensure(h, esz, n);
    if (n > h->count && h->data)
        memset((char*)h->data + (size_t)h->count * esz, 0,
               (size_t)(n - h->count) * esz);
    h->count = n;
}

void come_array_append_g(come_hdr_t* h, size_t esz, const void* elem) {
    if (!h || !elem) return;
    come_array_ensure(h, esz, h->count + 1);
    if (!h->data) return;
    memcpy((char*)h->data + (size_t)h->count * esz, elem, esz);
    h->count++;
}

void come_array_insert_g(come_hdr_t* h, size_t esz, uint32_t idx, const void* elem) {
    if (!h || !elem) return;
    if (idx > h->count) { come_ERR_set(ERANGE, "array index out of bounds"); return; }
    come_array_ensure(h, esz, h->count + 1);
    if (!h->data) return;
    char* items = (char*)h->data;
    memmove(items + (size_t)(idx + 1) * esz, items + (size_t)idx * esz,
            (size_t)(h->count - idx) * esz);
    memcpy(items + (size_t)idx * esz, elem, esz);
    h->count++;
}

void come_array_remove_g(come_hdr_t* h, size_t esz, uint32_t idx) {
    if (!h || !h->data) return;
    if (idx >= h->count) { come_ERR_set(ERANGE, "array index out of bounds"); return; }
    char* items = (char*)h->data;
    memmove(items + (size_t)idx * esz, items + (size_t)(idx + 1) * esz,
            (size_t)(h->count - idx - 1) * esz);
    h->count--;
}

void come_array_clear_g(come_hdr_t* h) {
    if (h) h->count = 0;
}

void come_array_reverse_g(come_hdr_t* h, size_t esz) {
    if (!h || !h->data || esz == 0) return;
    char* items = (char*)h->data;
    char tmp[64];
    if (esz > sizeof(tmp)) return;
    for (uint32_t i = 0; i < h->count / 2; i++) {
        char* lo = items + (size_t)i * esz;
        char* hi = items + (size_t)(h->count - 1 - i) * esz;
        memcpy(tmp, lo, esz);
        memcpy(lo, hi, esz);
        memcpy(hi, tmp, esz);
    }
}

void come_array_set_g(come_hdr_t* h, size_t esz, const void* value) {
    if (!h || !h->data || !value) return;
    char* items = (char*)h->data;
    for (uint32_t i = 0; i < h->count; i++)
        memcpy(items + (size_t)i * esz, value, esz);
}

void come_array_cpy_g(come_hdr_t* dst, size_t esz, come_hdr_t src) {
    if (!dst) return;
    come_array_ensure(dst, esz, src.count);
    if (src.count && !dst->data) return;
    if (src.count)
        memcpy(dst->data, src.data, (size_t)src.count * esz);
    dst->count = src.count;
}

come_hdr_t come_array_dup_g(void* ctx, come_hdr_t a, size_t esz) {
    come_hdr_t res = { NULL, 0, 0, ctx };
    res.count = a.count;
    res.cap = a.count;
    if (a.count) {
        res.data = mem_talloc_alloc(ctx, esz * a.count);
        if (res.data) memcpy(res.data, a.data, (size_t)a.count * esz);
    }
    return res;
}

long come_array_index_g(come_hdr_t a, size_t esz, const void* elem) {
    if (!a.data || !elem) return -1;
    const char* items = (const char*)a.data;
    for (uint32_t i = 0; i < a.count; i++)
        if (memcmp(items + (size_t)i * esz, elem, esz) == 0) return (long)i;
    return -1;
}

bool come_array_contains_g(come_hdr_t a, size_t esz, const void* elem) {
    return come_array_index_g(a, esz, elem) >= 0;
}

// String elements compare by content, not by header bytes:
// two distinct buffers with the same text are the same value.
// (Byte-wise over the counted length, so embedded NULs count;
// kept local so array.o needs no string.o symbols.)
static int hdr_string_eq(come_string_t a, come_string_t b) {
    if (a.count != b.count) return 0;
    if (a.count == 0) return 1;
    if (!a.data || !b.data) return 0;
    return memcmp(a.data, b.data, a.count) == 0;
}

long come_array_index_string(come_hdr_t a, come_string_t v) {
    if (!a.data) return -1;
    const come_string_t* e = (const come_string_t*)a.data;
    for (uint32_t i = 0; i < a.count; i++)
        if (hdr_string_eq(e[i], v)) return (long)i;
    return -1;
}

bool come_array_contains_string(come_hdr_t a, come_string_t v) {
    return come_array_index_string(a, v) >= 0;
}

come_hdr_t come_array_slice_g(come_hdr_t a, size_t esz, uint32_t start, uint32_t end) {
    come_hdr_t res = { NULL, 0, 0, a.ctx };
    if (!a.data || start >= a.count || start >= end) return res;
    if (end > a.count) end = a.count;
    uint32_t n = end - start;
    res.data = mem_talloc_alloc(a.ctx, esz * n);
    if (!res.data) return res;
    memcpy(res.data, (const char*)a.data + (size_t)start * esz, (size_t)n * esz);
    res.count = n;
    res.cap = n;
    return res;
}

/* Typed wrappers: in-place sorts with per-element comparators. */

void come_int_array_sort(come_int_array_t* a, int (*cmp)(int, int)) {
    if (!a || !a->data || !cmp) return;
    int* items = (int*)a->data;
    for (uint32_t i = 1; i < a->count; i++) {
        int key = items[i];
        uint32_t j = i;
        while (j > 0 && cmp(items[j - 1], key) > 0) {
            items[j] = items[j - 1];
            j--;
        }
        items[j] = key;
    }
}

void come_byte_array_sort(come_byte_array_t* a, int (*cmp)(int8_t, int8_t)) {
    if (!a || !a->data || !cmp) return;
    uint8_t* items = (uint8_t*)a->data;
    for (uint32_t i = 1; i < a->count; i++) {
        uint8_t key = items[i];
        uint32_t j = i;
        while (j > 0 && cmp((int8_t)items[j - 1], (int8_t)key) > 0) {
            items[j] = items[j - 1];
            j--;
        }
        items[j] = key;
    }
}

void come_string_list_sort(come_string_list_t* a, int (*cmp)(come_string_t, come_string_t)) {
    if (!a || !a->data || !cmp) return;
    come_string_t* items = (come_string_t*)a->data;
    for (uint32_t i = 1; i < a->count; i++) {
        come_string_t key = items[i];
        uint32_t j = i;
        while (j > 0 && cmp(items[j - 1], key) > 0) {
            items[j] = items[j - 1];
            j--;
        }
        items[j] = key;
    }
}

// Ownership helpers on the shared layout.
void come_hdr_chown_ctx(come_hdr_t* h, void* new_ctx) {
    if (!h) return;
    if (h->data) mem_talloc_steal(new_ctx, h->data);
    h->ctx = new_ctx;
}

void come_hdr_chown_to(come_hdr_t* h, const come_hdr_t* owner) {
    if (!h || !owner) return;
    if (h->data) {
        if (owner->data) mem_talloc_steal(owner->data, h->data);
        else if (owner->ctx) mem_talloc_steal(owner->ctx, h->data);
    }
    if (owner->ctx) h->ctx = owner->ctx;
}

void come_hdr_free(come_hdr_t* h) {
    if (!h) return;
    mem_talloc_free(h->data);
    h->data = NULL;
    h->count = 0;
    h->cap = 0;
}
