#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "come_array.h"
#include "mem/talloc.h"

// Out-of-bounds scratch slots (COIL-106). Every call zeroes the slot
// and records the error, so an OOB read sees the zero value and an
// OOB write is discarded (the next call wipes it).
int* come_oob_slot_int(void) {
    static int slot;
    slot = 0;
    errno = ERANGE;
    return &slot;
}

uint8_t* come_oob_slot_byte(void) {
    static uint8_t slot;
    slot = 0;
    errno = ERANGE;
    return &slot;
}

struct come_string_t** come_oob_slot_string(void) {
    static struct come_string_t* slot;
    static struct { uint32_t size; uint32_t count; char data[1]; } empty = { 9, 0, { 0 } };
    slot = (struct come_string_t*)&empty;
    errno = ERANGE;
    return &slot;
}

void* come_oob_slot_bytes(void) {
    static uint8_t slot[4096];
    memset(slot, 0, sizeof(slot));
    errno = ERANGE;
    return slot;
}

// Generic allocation
void* come_array_alloc(TALLOC_CTX* ctx, size_t elem_size, uint32_t count) {
    size_t header_size = sizeof(uint32_t) * 2;
    void* arr = mem_talloc_alloc(ctx, header_size + elem_size * count);
    if (!arr) return NULL;
    
    uint32_t* h = (uint32_t*)arr;
    h[0] = count; // size
    h[1] = count; // count
    
    // Clear items
    memset((char*)arr + header_size, 0, elem_size * count);
    
    return arr;
}

void* come_array_realloc(void* arr, size_t elem_size, uint32_t new_size) {
    size_t header_size = sizeof(uint32_t) * 2;
    uint32_t old_size = arr ? ((uint32_t*)arr)[0] : 0;
    
    void* new_arr = mem_talloc_realloc(NULL, arr, header_size + elem_size * new_size);
    if (!new_arr) return NULL;
    
    uint32_t* h = (uint32_t*)new_arr;
    h[0] = new_size; // size
    h[1] = new_size; // count - resize implies changing used count
    
    // Zero init new items if growing
    if (new_size > old_size) {
        memset((char*)new_arr + header_size + old_size * elem_size, 
               0, 
               (new_size - old_size) * elem_size);
    }
    
    return new_arr;
}

void* come_int_array_resize(come_int_array_t* a, uint32_t n) {
    return (come_int_array_t*)come_array_realloc(a, sizeof(int), n);
}

void* come_byte_array_resize(come_byte_array_t* a, uint32_t n) {
    return (come_byte_array_t*)come_array_realloc(a, sizeof(uint8_t), n);
}

void* come_string_list_resize(come_string_list_t* a, uint32_t n) {
    return (come_string_list_t*)come_array_realloc(a, sizeof(void*), n);
}

come_int_array_t* come_int_array_slice(come_int_array_t* a, uint32_t start, uint32_t end) {
    if (!a) {
        return (come_int_array_t*)come_array_alloc(NULL, sizeof(int), 0);
    }
    
    if (start >= a->count || start >= end) {
        return (come_int_array_t*)come_array_alloc((void*)a, sizeof(int), 0);
    }
    if (end > a->count) end = a->count;
    uint32_t n = end - start;
    come_int_array_t* res = (come_int_array_t*)come_array_alloc((void*)a, sizeof(int), n);
    if (res) {
        memcpy(res->items, &a->items[start], n * sizeof(int));
    }
    return res;
}

come_byte_array_t* come_byte_array_slice(come_byte_array_t* a, uint32_t start, uint32_t end) {
    if (!a || start >= a->count || start >= end) {
        return (come_byte_array_t*)come_array_alloc((void*)a, sizeof(uint8_t), 0);
    }
    if (end > a->count) end = a->count;
    uint32_t n = end - start;
    come_byte_array_t* res = (come_byte_array_t*)come_array_alloc((void*)a, sizeof(uint8_t), n);
    if (res) {
        memcpy(res->items, &a->items[start], n * sizeof(uint8_t));
    }
    return res;
}

come_string_list_t* come_string_list_slice(come_string_list_t* a, uint32_t start, uint32_t end) {
    if (!a || start >= a->count || start >= end) {
        return (come_string_list_t*)come_array_alloc((void*)a, sizeof(void*), 0);
    }
    if (end > a->count) end = a->count;
    uint32_t n = end - start;
    come_string_list_t* res = (come_string_list_t*)come_array_alloc((void*)a, sizeof(void*), n);
    if (res) {
        memcpy(res->items, &a->items[start], n * sizeof(void*));
    }
    return res;
}

/* ---- Toolbox (Come_array.md section 4, COIL-116) ----------------
 * All arrays share the header layout [uint32 capacity][uint32
 * count][items...]. The generic functions take the element size;
 * typed wrappers below adapt them. Growing operations may move
 * the array and return the new pointer. */

static uint32_t* arr_hdr(void* a) { return (uint32_t*)a; }
static char* arr_items(void* a) { return (char*)a + sizeof(uint32_t) * 2; }

static void* arr_grow(void* a, size_t esz, uint32_t need) {
    uint32_t cap = a ? arr_hdr(a)[0] : 0;
    uint32_t cnt = a ? arr_hdr(a)[1] : 0;
    if (a && cap >= need) return a;
    uint32_t ncap = cap ? cap : 4;
    while (ncap < need) ncap *= 2;
    void* na = mem_talloc_realloc(NULL, a, sizeof(uint32_t) * 2 + esz * ncap);
    if (!na) return a;
    arr_hdr(na)[0] = ncap;
    arr_hdr(na)[1] = cnt;
    return na;
}

void* come_array_append_g(void* a, size_t esz, const void* elem) {
    if (!elem) return a;
    uint32_t cnt = a ? arr_hdr(a)[1] : 0;
    a = arr_grow(a, esz, cnt + 1);
    if (!a) return NULL;
    memcpy(arr_items(a) + cnt * esz, elem, esz);
    arr_hdr(a)[1] = cnt + 1;
    return a;
}

void* come_array_insert_g(void* a, size_t esz, uint32_t idx, const void* elem) {
    if (!elem) return a;
    uint32_t cnt = a ? arr_hdr(a)[1] : 0;
    if (idx > cnt) { errno = ERANGE; return a; }
    a = arr_grow(a, esz, cnt + 1);
    if (!a) return NULL;
    char* items = arr_items(a);
    memmove(items + (idx + 1) * esz, items + idx * esz, (cnt - idx) * esz);
    memcpy(items + idx * esz, elem, esz);
    arr_hdr(a)[1] = cnt + 1;
    return a;
}

void* come_array_remove_g(void* a, size_t esz, uint32_t idx) {
    if (!a) return NULL;
    uint32_t cnt = arr_hdr(a)[1];
    if (idx >= cnt) { errno = ERANGE; return a; }
    char* items = arr_items(a);
    memmove(items + idx * esz, items + (idx + 1) * esz, (cnt - idx - 1) * esz);
    arr_hdr(a)[1] = cnt - 1;
    return a;
}

void come_array_clear_g(void* a) {
    if (a) arr_hdr(a)[1] = 0;
}

void come_array_reverse_g(void* a, size_t esz) {
    if (!a || esz == 0) return;
    uint32_t cnt = arr_hdr(a)[1];
    char* items = arr_items(a);
    char tmp[64];
    if (esz > sizeof(tmp)) return;
    for (uint32_t i = 0; i < cnt / 2; i++) {
        char* lo = items + i * esz;
        char* hi = items + (cnt - 1 - i) * esz;
        memcpy(tmp, lo, esz);
        memcpy(lo, hi, esz);
        memcpy(hi, tmp, esz);
    }
}

void come_array_set_g(void* a, size_t esz, const void* value) {
    if (!a || !value) return;
    uint32_t cnt = arr_hdr(a)[1];
    char* items = arr_items(a);
    for (uint32_t i = 0; i < cnt; i++) memcpy(items + i * esz, value, esz);
}

void* come_array_cpy_g(void* dst, size_t esz, const void* src) {
    if (!src) return dst;
    uint32_t scnt = ((const uint32_t*)src)[1];
    dst = arr_grow(dst, esz, scnt);
    if (!dst) return NULL;
    memcpy(arr_items(dst), (const char*)src + sizeof(uint32_t) * 2, scnt * esz);
    arr_hdr(dst)[1] = scnt;
    return dst;
}

void* come_array_dup_g(void* ctx, const void* a, size_t esz) {
    if (!a) return NULL;
    uint32_t cnt = ((const uint32_t*)a)[1];
    void* res = come_array_alloc(ctx, esz, cnt);
    if (res) memcpy(arr_items(res), (const char*)a + sizeof(uint32_t) * 2, cnt * esz);
    return res;
}

long come_array_index_g(const void* a, size_t esz, const void* elem) {
    if (!a || !elem) return -1;
    uint32_t cnt = ((const uint32_t*)a)[1];
    const char* items = (const char*)a + sizeof(uint32_t) * 2;
    for (uint32_t i = 0; i < cnt; i++)
        if (memcmp(items + i * esz, elem, esz) == 0) return (long)i;
    return -1;
}

bool come_array_contains_g(const void* a, size_t esz, const void* elem) {
    return come_array_index_g(a, esz, elem) >= 0;
}

/* Typed wrappers: value semantics per element type. */

come_int_array_t* come_int_array_append(come_int_array_t* a, int elem) {
    return (come_int_array_t*)come_array_append_g(a, sizeof(int), &elem);
}
come_int_array_t* come_int_array_insert(come_int_array_t* a, uint32_t idx, int elem) {
    return (come_int_array_t*)come_array_insert_g(a, sizeof(int), idx, &elem);
}
void come_int_array_set(come_int_array_t* a, int value) { come_array_set_g(a, sizeof(int), &value); }
come_int_array_t* come_int_array_cpy(come_int_array_t* dst, const come_int_array_t* src) {
    return (come_int_array_t*)come_array_cpy_g(dst, sizeof(int), src);
}
come_int_array_t* come_int_array_dup(void* ctx, const come_int_array_t* a) {
    return (come_int_array_t*)come_array_dup_g(ctx, a, sizeof(int));
}
long come_int_array_index(const come_int_array_t* a, int elem) {
    return come_array_index_g(a, sizeof(int), &elem);
}
bool come_int_array_contains(const come_int_array_t* a, int elem) {
    return come_array_index_g(a, sizeof(int), &elem) >= 0;
}
void come_int_array_sort(come_int_array_t* a, int (*cmp)(int, int)) {
    if (!a || !cmp) return;
    // Insertion sort: simple, stable, and comparator-true.
    for (uint32_t i = 1; i < a->count; i++) {
        int key = a->items[i];
        uint32_t j = i;
        while (j > 0 && cmp(a->items[j - 1], key) > 0) {
            a->items[j] = a->items[j - 1];
            j--;
        }
        a->items[j] = key;
    }
}

come_byte_array_t* come_byte_array_append(come_byte_array_t* a, uint8_t elem) {
    return (come_byte_array_t*)come_array_append_g(a, sizeof(uint8_t), &elem);
}
come_byte_array_t* come_byte_array_insert(come_byte_array_t* a, uint32_t idx, uint8_t elem) {
    return (come_byte_array_t*)come_array_insert_g(a, sizeof(uint8_t), idx, &elem);
}
void come_byte_array_set(come_byte_array_t* a, uint8_t value) { come_array_set_g(a, sizeof(uint8_t), &value); }
come_byte_array_t* come_byte_array_cpy(come_byte_array_t* dst, const come_byte_array_t* src) {
    return (come_byte_array_t*)come_array_cpy_g(dst, sizeof(uint8_t), src);
}
come_byte_array_t* come_byte_array_dup(void* ctx, const come_byte_array_t* a) {
    return (come_byte_array_t*)come_array_dup_g(ctx, a, sizeof(uint8_t));
}
long come_byte_array_index(const come_byte_array_t* a, uint8_t elem) {
    return come_array_index_g(a, sizeof(uint8_t), &elem);
}
bool come_byte_array_contains(const come_byte_array_t* a, uint8_t elem) {
    return come_array_index_g(a, sizeof(uint8_t), &elem) >= 0;
}
void come_byte_array_sort(come_byte_array_t* a, int (*cmp)(int8_t, int8_t)) {
    if (!a || !cmp) return;
    for (uint32_t i = 1; i < a->count; i++) {
        uint8_t key = a->items[i];
        uint32_t j = i;
        while (j > 0 && cmp((int8_t)a->items[j - 1], (int8_t)key) > 0) {
            a->items[j] = a->items[j - 1];
            j--;
        }
        a->items[j] = key;
    }
}

come_string_list_t* come_string_list_append(come_string_list_t* a, struct come_string_t* elem) {
    return (come_string_list_t*)come_array_append_g(a, sizeof(void*), &elem);
}
come_string_list_t* come_string_list_insert(come_string_list_t* a, uint32_t idx, struct come_string_t* elem) {
    return (come_string_list_t*)come_array_insert_g(a, sizeof(void*), idx, &elem);
}
void come_string_list_set(come_string_list_t* a, struct come_string_t* value) {
    come_array_set_g(a, sizeof(void*), &value);
}
come_string_list_t* come_string_list_cpy(come_string_list_t* dst, const come_string_list_t* src) {
    return (come_string_list_t*)come_array_cpy_g(dst, sizeof(void*), src);
}
come_string_list_t* come_string_list_dup(void* ctx, const come_string_list_t* a) {
    return (come_string_list_t*)come_array_dup_g(ctx, a, sizeof(void*));
}
long come_string_list_index(const come_string_list_t* a, const struct come_string_t* elem) {
    if (!a || !elem) return -1;
    const char* needle = (const char*)elem + sizeof(uint32_t) * 2;
    for (uint32_t i = 0; i < a->count; i++) {
        const struct come_string_t* s = a->items[i];
        const char* data = s ? (const char*)s + sizeof(uint32_t) * 2 : "";
        if (strcmp(data, needle) == 0) return (long)i;
    }
    return -1;
}
bool come_string_list_contains(const come_string_list_t* a, const struct come_string_t* elem) {
    return come_string_list_index(a, elem) >= 0;
}
void come_string_list_sort(come_string_list_t* a, int (*cmp)(struct come_string_t*, struct come_string_t*)) {
    if (!a || !cmp) return;
    for (uint32_t i = 1; i < a->count; i++) {
        struct come_string_t* key = a->items[i];
        uint32_t j = i;
        while (j > 0 && cmp(a->items[j - 1], key) > 0) {
            a->items[j] = a->items[j - 1];
            j--;
        }
        a->items[j] = key;
    }
}
