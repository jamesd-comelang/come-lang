#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "come_array.h"
#include "mem/talloc.h"

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
