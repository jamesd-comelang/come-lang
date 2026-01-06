#ifndef COME_MAP_H
#define COME_MAP_H

#include <stdint.h>
#include <stdbool.h>
#include "come_string.h"

// Forward declaration for talloc context
typedef void TALLOC_CTX;

typedef struct come_map_entry_t {
    string key;
    void* value;
    uint32_t hash;
    bool occupied;
} come_map_entry_t;

typedef struct come_map_t {
    uint32_t size;  // Capacity (slots)
    uint32_t count; // Number of elements
    come_map_entry_t entries[];
} come_map_t;

typedef come_map_t* map;

// API
come_map_t* come_map_new(TALLOC_CTX* ctx);
void come_map_put(come_map_t** m, string key, void* value);
void* come_map_get(come_map_t* m, string key);
void come_map_remove(come_map_t* m, string key);
uint32_t come_map_len(const come_map_t* m);
void come_map_free(come_map_t* m);

#endif // COME_MAP_H
