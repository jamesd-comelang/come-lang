#ifndef COME_ARRAY_MODULE_H
#define COME_ARRAY_MODULE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Forward declaration for talloc context
typedef void TALLOC_CTX;

// One uniform buffer design (COIL-135): every array, string and map
// is a split header kept INLINE in the variable - no heap allocation
// for the header itself. Only the data block is a context-owned heap
// allocation. The header layout is identical across all buffer
// kinds:
//
//   data  - the heap data block (NULL while empty)
//   count - elements / characters / entries in use
//   cap   - capacity of the data block (elements / bytes / slots)
//   ctx   - the owner context of the data block
//
// Assignment copies the header and shares the data block. Growth
// never moves a shared block: it allocates a fresh data block in
// ctx, copies, and updates only the header it was called on, so an
// alias keeps a valid older snapshot of the data.
typedef struct {
    void* data;
    uint32_t count;
    uint32_t cap;
    void* ctx;
} come_hdr_t;

// Every array kind is the same header type; the element type is a
// compile-time fact the code generator carries, not a distinct C
// struct. The per-kind names remain as aliases.
typedef come_hdr_t come_int_array_t;
typedef come_hdr_t come_byte_array_t;
typedef come_hdr_t come_string_list_t;
typedef come_hdr_t come_array_short_t;
typedef come_hdr_t come_array_ushort_t;
typedef come_hdr_t come_array_ubyte_t;
typedef come_hdr_t come_array_uint_t;
typedef come_hdr_t come_array_long_t;
typedef come_hdr_t come_array_ulong_t;
typedef come_hdr_t come_array_float_t;
typedef come_hdr_t come_array_double_t;
typedef come_hdr_t come_array_bool_t;
typedef come_hdr_t come_array_wchar_t;

// An empty header bound to an owner context; no allocation happens
// until the first growth.
#define COME_HDR_INIT(ctx) { NULL, 0, 0, (void*)(ctx) }

// Generic operations on the shared header layout. Mutating
// operations take the address of the header slot (a variable, a
// struct field, or an array element - the header lives wherever the
// value lives). Growth allocates a fresh data block in h->ctx and
// never frees or moves the old one; the owner context reclaims it.
void come_array_ensure(come_hdr_t* h, size_t esz, uint32_t mincap);
void come_array_resize_g(come_hdr_t* h, size_t esz, uint32_t n);
void come_array_append_g(come_hdr_t* h, size_t esz, const void* elem);
void come_array_insert_g(come_hdr_t* h, size_t esz, uint32_t idx, const void* elem);
void come_array_remove_g(come_hdr_t* h, size_t esz, uint32_t idx);
void come_array_clear_g(come_hdr_t* h);
void come_array_reverse_g(come_hdr_t* h, size_t esz);
void come_array_set_g(come_hdr_t* h, size_t esz, const void* value);
void come_array_cpy_g(come_hdr_t* dst, size_t esz, come_hdr_t src);
come_hdr_t come_array_dup_g(void* ctx, come_hdr_t a, size_t esz);
long come_array_index_g(come_hdr_t a, size_t esz, const void* elem);
bool come_array_contains_g(come_hdr_t a, size_t esz, const void* elem);
struct come_string_t;
typedef struct come_string_t come_string_t;
long come_array_index_string(come_hdr_t a, come_string_t v);
bool come_array_contains_string(come_hdr_t a, come_string_t v);
come_hdr_t come_array_slice_g(come_hdr_t a, size_t esz, uint32_t start, uint32_t end);

// Ownership helpers on the shared layout (any buffer kind).
// chown_ctx re-parents the data block to a context; chown_to
// re-parents it under another buffer's data block, so the buffer
// dies with that buffer's owner - an owner can be a variable.
// free releases the data block and empties the header.
void come_hdr_chown_ctx(come_hdr_t* h, void* new_ctx);
void come_hdr_chown_to(come_hdr_t* h, const come_hdr_t* owner);
void come_hdr_free(come_hdr_t* h);

// Typed wrappers kept for the sort comparators (in-place on data).
void come_int_array_sort(come_int_array_t* a, int (*cmp)(int, int));
void come_byte_array_sort(come_byte_array_t* a, int (*cmp)(int8_t, int8_t));

// Out-of-bounds scratch slots (COIL-106): each call zeroes the slot
// and sets ERR (errno = ERANGE). An OOB read therefore yields the
// element type's zero value; an OOB write lands in the slot and is
// discarded by the next call.
int* come_oob_slot_int(void);
uint8_t* come_oob_slot_byte(void);
void* come_oob_slot_bytes(void);
// Site-carrying variants (COIL-139): the macro below passes
// __FILE__/__LINE__/__func__ from the generated code, whose
// #line map makes them the user's .co file, line and function.
int* come_oob_slot_int_at(const char *file, int line, const char *func);
uint8_t* come_oob_slot_byte_at(const char *file, int line, const char *func);
void* come_oob_slot_bytes_at(const char *file, int line, const char *func);

// Bounds-checked element access on a header VALUE. In-bounds this is
// the plain data[] access, as an lvalue, so arr[i] = v keeps
// working. Note: h and idx are each evaluated twice in the expansion
// - do not use side-effecting expressions as an index.
#define COME_ARR_GET_T(h, idx, T) \
    (*((uint32_t)(idx) < (h).count ? ((T*)(h).data) + (idx) \
                                  : (T*)come_oob_slot_bytes_at(__FILE__, __LINE__, __func__)))

// Count of a header value (for codegen paths that need a plain C
// read of the length of any buffer kind).
#define COME_HDR_LEN(h) ((h).count)

// Byte footprint of the data block: capacity x element size. For
// strings the capacity already counts bytes, so the element size is
// 1. (COIL-134: .bytesize() measures bytes, .length() counts.)
#define come_array_bytesize(h, esz) ((size_t)(h).cap * (size_t)(esz))

#endif // COME_ARRAY_MODULE_H
