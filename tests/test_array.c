#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include "come_array.h"
#include "come_string.h"

// COIL-116: the Come_array.md section 4 toolbox at the runtime
// level. (Come-level calls: src/array/t/07-toolbox.co.)

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

static int cmp_int(int a, int b) { return a - b; }
static int cmp_byte(int8_t a, int8_t b) { return (int)a - (int)b; }
static int cmp_str(come_string_t* a, come_string_t* b) { return strcmp(a->data, b->data); }

int main() {
    // int array: append / insert / remove / contains / index
    come_int_array_t* a = (come_int_array_t*)come_array_alloc(NULL, sizeof(int), 0);
    a = come_int_array_append(a, 3);
    a = come_int_array_append(a, 1);
    a = come_int_array_append(a, 2);
    CHECK(a->count == 3 && a->items[0] == 3, "append builds the array");
    a = come_int_array_insert(a, 0, 9);
    CHECK(a->count == 4 && a->items[0] == 9 && a->items[1] == 3, "insert at head");
    errno = 0;
    a = come_int_array_insert(a, 99, 0);
    CHECK(a->count == 4 && errno == ERANGE, "insert past the end is rejected with ERANGE");
    CHECK(come_int_array_contains(a, 2) && !come_int_array_contains(a, 7), "contains");
    CHECK(come_int_array_index(a, 2) == 3 && come_int_array_index(a, 7) == -1, "index");
    a = (come_int_array_t*)come_array_remove_g(a, sizeof(int), 0);
    CHECK(a->count == 3 && a->items[0] == 3, "remove at head");
    errno = 0;
    a = (come_int_array_t*)come_array_remove_g(a, sizeof(int), 42);
    CHECK(a->count == 3 && errno == ERANGE, "remove out of range is rejected with ERANGE");

    // sort / reverse / set / cpy / dup / clear
    come_int_array_sort(a, cmp_int);
    CHECK(a->items[0] == 1 && a->items[1] == 2 && a->items[2] == 3, "sort ascending");
    come_array_reverse_g(a, sizeof(int));
    CHECK(a->items[0] == 3 && a->items[2] == 1, "reverse");
    come_int_array_set(a, 5);
    CHECK(a->items[0] == 5 && a->items[2] == 5, "set fills");
    come_int_array_t* b = (come_int_array_t*)come_array_alloc(NULL, sizeof(int), 0);
    b = come_int_array_cpy(b, a);
    CHECK(b->count == 3 && b->items[1] == 5, "cpy copies elements and length");
    come_int_array_t* d = come_int_array_dup(NULL, a);
    CHECK(d->count == 3 && d->items[0] == 5, "dup copies");
    d->items[0] = 99;
    CHECK(a->items[0] == 5, "dup is independent");
    come_array_clear_g(a);
    CHECK(a->count == 0, "clear empties");

    // growth from empty via the generic path (long elements)
    long lv = 10000000000L;
    void* g = come_array_append_g(NULL, sizeof(long), &lv);
    long sv = 5;
    g = come_array_append_g(g, sizeof(long), &sv);
    CHECK(((uint32_t*)g)[1] == 2, "generic append from null");
    CHECK(come_array_index_g(g, sizeof(long), &sv) == 1, "generic index");
    CHECK(come_array_contains_g(g, sizeof(long), &lv), "generic contains");

    // byte array sort
    come_byte_array_t* by = (come_byte_array_t*)come_array_alloc(NULL, sizeof(uint8_t), 0);
    by = come_byte_array_append(by, 5);
    by = come_byte_array_append(by, 2);
    by = come_byte_array_append(by, 9);
    come_byte_array_sort(by, cmp_byte);
    CHECK(by->items[0] == 2 && by->items[1] == 5 && by->items[2] == 9, "byte sort");

    // string list: append / contains / index / sort (by content)
    come_string_list_t* names = (come_string_list_t*)come_array_alloc(NULL, sizeof(void*), 0);
    names = come_string_list_append(names, come_string_new(NULL, "pear"));
    names = come_string_list_append(names, come_string_new(NULL, "apple"));
    names = come_string_list_append(names, come_string_new(NULL, "fig"));
    CHECK(names->count == 3, "string append");
    CHECK(come_string_list_contains(names, come_string_new(NULL, "fig")), "string contains by content");
    CHECK(!come_string_list_contains(names, come_string_new(NULL, "plum")), "string contains misses");
    CHECK(come_string_list_index(names, come_string_new(NULL, "apple")) == 1, "string index by content");
    come_string_list_sort(names, cmp_str);
    CHECK(strcmp(names->items[0]->data, "apple") == 0 && strcmp(names->items[2]->data, "pear") == 0, "string sort");

    if (failures) { printf("test_array: %d FAILURES\n", failures); return 1; }
    printf("test_array: all checks passed\n");
    return 0;
}
