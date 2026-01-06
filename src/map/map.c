#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

come_map_t* come_map_new(TALLOC_CTX* ctx) {
    size_t header_size = sizeof(uint32_t) * 2;
    size_t total_size = header_size + sizeof(come_map_entry_t) * INITIAL_CAPACITY;
    come_map_t* m = mem_talloc_alloc(ctx, total_size);
    if (!m) return NULL;
    
    m->size = INITIAL_CAPACITY;
    m->count = 0;
    memset(m->entries, 0, sizeof(come_map_entry_t) * INITIAL_CAPACITY);
    
    return m;
}

static void rehash(come_map_t** m_ptr) {
    come_map_t* old_m = *m_ptr;
    uint32_t new_size = old_m->size * 2;
    TALLOC_CTX* ctx = NULL; // Use NULL context for rehash, or pass as param if needed
    
    come_map_t* new_m = mem_talloc_alloc(ctx, sizeof(uint32_t) * 2 + sizeof(come_map_entry_t) * new_size);
    if (!new_m) return;
    
    new_m->size = new_size;
    new_m->count = 0;
    memset(new_m->entries, 0, sizeof(come_map_entry_t) * new_size);
    
    for (uint32_t i = 0; i < old_m->size; i++) {
        if (old_m->entries[i].occupied) {
            come_map_put(&new_m, old_m->entries[i].key, old_m->entries[i].value);
        }
    }
    
    *m_ptr = new_m;
    // We don't explicitly free old_m if it behaves like talloc_realloc but here it's a new alloc.
    // In Come's model, we might want to let the arena handle it or free it.
    // mem_talloc_free(old_m); // Safe to free since we copied entries
}

void come_map_put(come_map_t** m_ptr, string key, void* value) {
    if (!m_ptr || !key) return;
    
    if (!*m_ptr) {
        *m_ptr = come_map_new(NULL);
        if (!*m_ptr) return;
    }
    
    come_map_t* m = *m_ptr;
    
    if ((float)(m->count + 1) / m->size > LOAD_FACTOR_THRESHOLD) {
        rehash(m_ptr);
        m = *m_ptr;
    }
    
    uint32_t hash = hash_string(key->data);
    uint32_t idx = hash % m->size;
    
    while (m->entries[idx].occupied) {
        if (strcmp(m->entries[idx].key->data, key->data) == 0) {
            m->entries[idx].value = value;
            return;
        }
        idx = (idx + 1) % m->size;
    }
    
    m->entries[idx].key = key;
    m->entries[idx].value = value;
    m->entries[idx].hash = hash;
    m->entries[idx].occupied = true;
    m->count++;
}

void* come_map_get(come_map_t* m, string key) {
    if (!m || !key) return NULL;
    
    uint32_t hash = hash_string(key->data);
    uint32_t idx = hash % m->size;
    uint32_t start_idx = idx;
    
    while (m->entries[idx].occupied) {
        if (strcmp(m->entries[idx].key->data, key->data) == 0) {
            return m->entries[idx].value;
        }
        idx = (idx + 1) % m->size;
        if (idx == start_idx) break;
    }
    
    return NULL;
}

void come_map_remove(come_map_t* m, string key) {
    if (!m || !key) return;
    
    uint32_t hash = hash_string(key->data);
    uint32_t idx = hash % m->size;
    uint32_t start_idx = idx;
    
    while (m->entries[idx].occupied) {
        if (strcmp(m->entries[idx].key->data, key->data) == 0) {
            m->entries[idx].occupied = false;
            m->entries[idx].key = NULL;
            m->entries[idx].value = NULL;
            m->count--;
            // Linear probing removal needs re-insertion of subsequent elements or tombstones.
            // For MVP, we'll just mark it unoccupied. But collisions at this index will break.
            // Better: re-insert until empty slot.
            uint32_t next = (idx + 1) % m->size;
            while (m->entries[next].occupied) {
                come_map_entry_t entry = m->entries[next];
                m->entries[next].occupied = false;
                m->count--;
                come_map_put(&m, entry.key, entry.value);
                next = (next + 1) % m->size;
            }
            return;
        }
        idx = (idx + 1) % m->size;
        if (idx == start_idx) break;
    }
}

uint32_t come_map_len(const come_map_t* m) {
    return m ? m->count : 0;
}

void come_map_free(come_map_t* m) {
    // mem_talloc_free(m);
}
