#ifndef COME_MAP_H
#define COME_MAP_H

#include <stdint.h>
#include <stdbool.h>
#include "come_string.h"
#include "come_array.h"

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

// Typed-map support (COIL-103): ERR-aware access and the documented
// method table. ERR is errno-backed, so a miss sets errno = ENOENT,
// which is what ERR.no()/ERR.str() report.
void* come_map_get_e(come_map_t* m, string key);
int come_map_remove_e(come_map_t* m, string key);
void come_map_clear(come_map_t* m);
come_string_list_t* come_map_keys(TALLOC_CTX* ctx, come_map_t* m);
come_map_t* come_map_copy(TALLOC_CTX* ctx, come_map_t* src);

#endif // COME_MAP_H
