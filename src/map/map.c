#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "come_map.h"
#include "mem/talloc.h"

#define INITIAL_CAPACITY 16
#define LOAD_FACTOR_THRESHOLD 0.75

// DJB2 hash for strings
static uint32_t hash_string(const char* str) {
    uint32_t hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

static const char* kdata(const come_string_t* k) {
    return (k && k->data) ? k->data : "";
}

// COIL-135: the map is a header value; only the entries block is a
// heap allocation, owned by the header's context.
come_map_t come_map_new(TALLOC_CTX* ctx) {
    come_map_t m = { NULL, 0, 0, ctx };
    return m;
}

// Grow the entries block: allocate a fresh block in the owner
// context, rehash into it, and leave the old block for the context
// to reclaim, so an alias keeps a valid older snapshot.
static void map_grow(come_map_t* m, uint32_t new_cap) {
    come_map_entry_t* old = m->data;
    uint32_t old_cap = m->cap;
    come_map_entry_t* nd = (come_map_entry_t*)mem_talloc_alloc(
        m->ctx, sizeof(come_map_entry_t) * new_cap);
    if (!nd) return;
    memset(nd, 0, sizeof(come_map_entry_t) * new_cap);
    m->data = nd;
    m->cap = new_cap;
    m->count = 0;
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old[i].occupied) {
            uint32_t idx = old[i].hash % new_cap;
            while (nd[idx].occupied) idx = (idx + 1) % new_cap;
            nd[idx] = old[i];
            m->count++;
        }
    }
}

static void map_ensure(come_map_t* m) {
    if (!m->data) { map_grow(m, INITIAL_CAPACITY); return; }
    if ((float)(m->count + 1) / (float)m->cap > LOAD_FACTOR_THRESHOLD)
        map_grow(m, m->cap * 2);
}

void come_map_put(come_map_t* m, come_string_t key, void* value) {
    if (!m) return;
    map_ensure(m);
    if (!m->data) return;

    uint32_t hash = hash_string(kdata(&key));
    uint32_t idx = hash % m->cap;

    while (m->data[idx].occupied) {
        if (strcmp(kdata(&m->data[idx].key), kdata(&key)) == 0) {
            m->data[idx].value = value;
            return;
        }
        idx = (idx + 1) % m->cap;
    }

    m->data[idx].key = key;
    m->data[idx].value = value;
    m->data[idx].hash = hash;
    m->data[idx].occupied = true;
    m->count++;
}

static come_map_entry_t* map_find(const come_map_t* m, const come_string_t* key) {
    if (!m || !m->data) return NULL;
    uint32_t hash = hash_string(kdata(key));
    uint32_t idx = hash % m->cap;
    uint32_t start_idx = idx;
    while (m->data[idx].occupied) {
        if (strcmp(kdata(&m->data[idx].key), kdata(key)) == 0) {
            return &m->data[idx];
        }
        idx = (idx + 1) % m->cap;
        if (idx == start_idx) break;
    }
    return NULL;
}

void* come_map_get(come_map_t m, come_string_t key) {
    come_map_entry_t* e = map_find(&m, &key);
    return e ? e->value : NULL;
}

void* come_map_get_e(come_map_t m, come_string_t key) {
    void* v = come_map_get(m, key);
    if (!v) come_ERR_set(ENOENT, "map key not found");
    return v;
}

void come_map_remove(come_map_t* m, come_string_t key) {
    if (!m || !m->data) return;

    uint32_t hash = hash_string(kdata(&key));
    uint32_t idx = hash % m->cap;
    uint32_t start_idx = idx;

    while (m->data[idx].occupied) {
        if (strcmp(kdata(&m->data[idx].key), kdata(&key)) == 0) {
            m->data[idx].occupied = false;
            memset(&m->data[idx].key, 0, sizeof(come_string_t));
            m->data[idx].value = NULL;
            m->count--;
            // Linear probing removal: re-insert the following
            // cluster so probe chains stay intact.
            uint32_t next = (idx + 1) % m->cap;
            while (m->data[next].occupied) {
                come_map_entry_t entry = m->data[next];
                m->data[next].occupied = false;
                m->count--;
                come_map_put(m, entry.key, entry.value);
                next = (next + 1) % m->cap;
            }
            return;
        }
        idx = (idx + 1) % m->cap;
        if (idx == start_idx) break;
    }
}

int come_map_remove_e(come_map_t* m, come_string_t key) {
    if (!come_map_get(*m, key)) {
        come_ERR_set(ENOENT, "map key not found");
        return 0;
    }
    come_map_remove(m, key);
    return 1;
}

uint32_t come_map_len(come_map_t m) {
    return m.count;
}

void come_map_clear(come_map_t* m) {
    if (!m) return;
    if (m->data) memset(m->data, 0, sizeof(come_map_entry_t) * m->cap);
    m->count = 0;
}

void come_map_free(come_map_t* m) {
    if (m) mem_talloc_free(m->data);
}

come_string_list_t come_map_keys(come_map_t m) {
    come_string_list_t list = COME_HDR_INIT(m.ctx);
    if (!m.data) return list;
    for (uint32_t i = 0; i < m.cap; i++) {
        if (m.data[i].occupied) {
            come_string_t k = m.data[i].key;
            come_array_append_g((come_hdr_t*)&list, sizeof(come_string_t), &k);
        }
    }
    return list;
}

// A shallow copy: the new table shares the key strings and the
// value pointers with the source.
come_map_t come_map_copy(come_map_t src) {
    come_map_t copy = come_map_new(src.ctx);
    if (!src.data) return copy;
    for (uint32_t i = 0; i < src.cap; i++) {
        if (src.data[i].occupied) {
            come_map_put(&copy, src.data[i].key, src.data[i].value);
        }
    }
    return copy;
}
