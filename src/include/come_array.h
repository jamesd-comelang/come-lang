#ifndef COME_ARRAY_MODULE_H
#define COME_ARRAY_MODULE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Forward declaration for talloc context
typedef void TALLOC_CTX;

// Generic array header structure (not used directly, but for clarity)
// [uint32 size, uint32 count, items...]

typedef struct come_int_array_t {
    uint32_t size;  // Capacity (elements)
    uint32_t count; // Used length
    int items[]; 
} come_int_array_t;

typedef struct come_byte_array_t {
    uint32_t size;  // Capacity (elements)
    uint32_t count; // Used length
    uint8_t items[]; 
} come_byte_array_t;

// Note: come_string_t is defined in come_string.h.
// Forward declaring it here if needed, or include come_string.h
struct come_string_t;

typedef struct {
    uint32_t size;
    uint32_t count;
    struct come_string_t* items[];
} come_string_list_t;

// Allocation / Management
void* come_array_alloc(TALLOC_CTX* ctx, size_t elem_size, uint32_t count);
void* come_array_realloc(void* arr, size_t elem_size, uint32_t new_size);

// Helpers for specific types (to be called by codegen via _Generic)
void* come_int_array_resize(come_int_array_t* a, uint32_t n);
void* come_byte_array_resize(come_byte_array_t* a, uint32_t n);
void* come_string_list_resize(come_string_list_t* a, uint32_t n);

come_int_array_t* come_int_array_slice(come_int_array_t* a, uint32_t start, uint32_t end);
come_byte_array_t* come_byte_array_slice(come_byte_array_t* a, uint32_t start, uint32_t end);
come_string_list_t* come_string_list_slice(come_string_list_t* a, uint32_t start, uint32_t end);

// Generic Accessor
#define COME_ARR_GET(arr, idx) _Generic((arr), \
    come_string_list_t*: ((come_string_list_t*)(arr))->items[(idx)], \
    const come_string_list_t*: ((const come_string_list_t*)(arr))->items[(idx)], \
    struct come_string_t*: come_string_at((struct come_string_t*)(arr), (idx)), \
    const struct come_string_t*: come_string_at((const struct come_string_t*)(arr), (idx)), \
    default: (arr)->items[(idx)] \
)

// Generic Size
#define come_array_size(arr) _Generic(&(arr), \
    come_int_array_t**: ((arr) ? (arr)->count : 0), \
    const come_int_array_t**: ((arr) ? (arr)->count : 0), \
    come_byte_array_t**: ((arr) ? (arr)->count : 0), \
    const come_byte_array_t**: ((arr) ? (arr)->count : 0), \
    come_string_list_t**: ((arr) ? (arr)->count : 0), \
    const come_string_list_t**: ((arr) ? (arr)->count : 0), \
    struct come_string_t**: ((arr) ? (arr)->count : 0), \
    const struct come_string_t**: ((arr) ? (arr)->count : 0) \
)

// Array Resize Helper Macro
#define come_array_resize(a, n) ((a) = _Generic((a), \
    come_int_array_t*: come_int_array_resize, \
    come_byte_array_t*: come_byte_array_resize, \
    come_string_list_t*: come_string_list_resize \
)((a), (n)))

// Array Slice Helper Macro
#define come_array_slice(a, start, end) _Generic((a), \
    come_int_array_t*: come_int_array_slice, \
    come_byte_array_t*: come_byte_array_slice, \
    come_string_list_t*: come_string_list_slice \
)((a), (start), (end))

#endif // COME_ARRAY_MODULE_H
