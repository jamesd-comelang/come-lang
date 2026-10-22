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

// The remaining scalar element types (COIL-116): same header
// layout, so the generic toolbox functions below work on all of
// them through the element size.
typedef struct { uint32_t size; uint32_t count; int16_t items[]; } come_array_short_t;
typedef struct { uint32_t size; uint32_t count; uint16_t items[]; } come_array_ushort_t;
typedef struct { uint32_t size; uint32_t count; uint8_t items[]; } come_array_ubyte_t;
typedef struct { uint32_t size; uint32_t count; uint32_t items[]; } come_array_uint_t;
typedef struct { uint32_t size; uint32_t count; long items[]; } come_array_long_t;
typedef struct { uint32_t size; uint32_t count; unsigned long items[]; } come_array_ulong_t;
typedef struct { uint32_t size; uint32_t count; float items[]; } come_array_float_t;
typedef struct { uint32_t size; uint32_t count; double items[]; } come_array_double_t;
typedef struct { uint32_t size; uint32_t count; bool items[]; } come_array_bool_t;
typedef struct { uint32_t size; uint32_t count; int32_t items[]; } come_array_wchar_t;

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

// Toolbox (Come_array.md section 4, COIL-116). The generic _g
// functions work on any array with the shared header layout via
// the element size; growing operations return the (possibly
// moved) array, so callers assign the result back.
void* come_array_append_g(void* a, size_t esz, const void* elem);
void* come_array_insert_g(void* a, size_t esz, uint32_t idx, const void* elem);
void* come_array_remove_g(void* a, size_t esz, uint32_t idx);
void come_array_clear_g(void* a);
void come_array_reverse_g(void* a, size_t esz);
void come_array_set_g(void* a, size_t esz, const void* value);
void* come_array_cpy_g(void* dst, size_t esz, const void* src);
void* come_array_dup_g(void* ctx, const void* a, size_t esz);
long come_array_index_g(const void* a, size_t esz, const void* elem);
bool come_array_contains_g(const void* a, size_t esz, const void* elem);

come_int_array_t* come_int_array_append(come_int_array_t* a, int elem);
come_int_array_t* come_int_array_insert(come_int_array_t* a, uint32_t idx, int elem);
void come_int_array_set(come_int_array_t* a, int value);
come_int_array_t* come_int_array_cpy(come_int_array_t* dst, const come_int_array_t* src);
come_int_array_t* come_int_array_dup(void* ctx, const come_int_array_t* a);
long come_int_array_index(const come_int_array_t* a, int elem);
bool come_int_array_contains(const come_int_array_t* a, int elem);
void come_int_array_sort(come_int_array_t* a, int (*cmp)(int, int));

come_byte_array_t* come_byte_array_append(come_byte_array_t* a, uint8_t elem);
come_byte_array_t* come_byte_array_insert(come_byte_array_t* a, uint32_t idx, uint8_t elem);
void come_byte_array_set(come_byte_array_t* a, uint8_t value);
come_byte_array_t* come_byte_array_cpy(come_byte_array_t* dst, const come_byte_array_t* src);
come_byte_array_t* come_byte_array_dup(void* ctx, const come_byte_array_t* a);
long come_byte_array_index(const come_byte_array_t* a, uint8_t elem);
bool come_byte_array_contains(const come_byte_array_t* a, uint8_t elem);
void come_byte_array_sort(come_byte_array_t* a, int (*cmp)(int8_t, int8_t));

come_string_list_t* come_string_list_append(come_string_list_t* a, struct come_string_t* elem);
come_string_list_t* come_string_list_insert(come_string_list_t* a, uint32_t idx, struct come_string_t* elem);
void come_string_list_set(come_string_list_t* a, struct come_string_t* value);
come_string_list_t* come_string_list_cpy(come_string_list_t* dst, const come_string_list_t* src);
come_string_list_t* come_string_list_dup(void* ctx, const come_string_list_t* a);
long come_string_list_index(const come_string_list_t* a, const struct come_string_t* elem);
bool come_string_list_contains(const come_string_list_t* a, const struct come_string_t* elem);
void come_string_list_sort(come_string_list_t* a, int (*cmp)(struct come_string_t*, struct come_string_t*));

// Generic Accessor
// Out-of-bounds scratch slots (COIL-106): each call zeroes the slot
// and sets ERR (errno = ERANGE). An OOB read therefore yields the
// element type's zero value; an OOB write lands in the slot and is
// discarded by the next call. Declared here, defined in array.c.
int* come_oob_slot_int(void);
uint8_t* come_oob_slot_byte(void);
struct come_string_t** come_oob_slot_string(void);
void* come_oob_slot_bytes(void);

// Bounds-checked element access (COIL-106). In-bounds this is the
// plain items[] access, as an lvalue, so arr[i] = v keeps working.
// Note: arr and idx are each evaluated twice in the expansion -
// do not use side-effecting expressions as an index.
// Strings are NOT routed through this macro (GCC type-checks every
// _Generic branch, and the default branch is invalid for strings);
// codegen emits come_string_at_e for string indexing instead.
#define COME_ARR_GET(arr, idx) _Generic((arr), \
    come_string_list_t*: *((uint32_t)(idx) < ((come_string_list_t*)(arr))->count ? ((come_string_list_t*)(arr))->items + (idx) : come_oob_slot_string()), \
    const come_string_list_t*: *((uint32_t)(idx) < ((const come_string_list_t*)(arr))->count ? ((const come_string_list_t*)(arr))->items + (idx) : come_oob_slot_string()), \
    come_int_array_t*: *((uint32_t)(idx) < ((come_int_array_t*)(arr))->count ? ((come_int_array_t*)(arr))->items + (idx) : come_oob_slot_int()), \
    const come_int_array_t*: *((uint32_t)(idx) < ((const come_int_array_t*)(arr))->count ? ((const come_int_array_t*)(arr))->items + (idx) : come_oob_slot_int()), \
    come_byte_array_t*: *((uint32_t)(idx) < ((come_byte_array_t*)(arr))->count ? ((come_byte_array_t*)(arr))->items + (idx) : come_oob_slot_byte()), \
    const come_byte_array_t*: *((uint32_t)(idx) < ((const come_byte_array_t*)(arr))->count ? ((const come_byte_array_t*)(arr))->items + (idx) : come_oob_slot_byte()), \
    default: *((uint32_t)(idx) < (arr)->count ? (arr)->items + (idx) : (__typeof__((arr)->items + 0))come_oob_slot_bytes()) \
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

// Generic Byte Size: element count x element size. The element size
// comes from a _Generic over constant sizeof expressions (branch
// expressions may not dereference arr - only the selected branch's
// type is known). For strings the count field already counts content
// bytes, so the element size is 1. (COIL-134: .bytesize() -
// .length() counts, .bytesize() measures bytes, .size() is gone.)
#define come_array_bytesize(arr) (come_array_size(arr) * _Generic(&(arr), \
    come_int_array_t**: sizeof(int), \
    const come_int_array_t**: sizeof(int), \
    come_byte_array_t**: sizeof(uint8_t), \
    const come_byte_array_t**: sizeof(uint8_t), \
    come_string_list_t**: sizeof(struct come_string_t*), \
    const come_string_list_t**: sizeof(struct come_string_t*), \
    struct come_string_t**: 1, \
    const struct come_string_t**: 1) \
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
