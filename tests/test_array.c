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
// COIL-135: arrays are header values - mutators take the
// header's slot, reads take the header by value, and growth
// allocates a fresh data block instead of moving a shared one.

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

static int cmp_int(int a, int b) { return a - b; }
static int cmp_byte(int8_t a, int8_t b) { return (int)a - (int)b; }
static int cmp_str(come_string_t a, come_string_t b) { return come_string_cmp(a, b, 0); }

#define AI(a, i) (((int*)(a).data)[i])
#define APP(a, v) do { int _v = (v); come_array_append_g((come_hdr_t*)&(a), sizeof(int), &_v); } while (0)

int main() {
    // int array: append / insert / remove / contains / index
    come_int_array_t a = COME_HDR_INIT(NULL);
    APP(a, 3); APP(a, 1); APP(a, 2);
    CHECK(a.count == 3 && AI(a, 0) == 3, "append builds the array");
    { int v = 9; come_array_insert_g((come_hdr_t*)&a, sizeof(int), 0, &v); }
    CHECK(a.count == 4 && AI(a, 0) == 9 && AI(a, 1) == 3, "insert at head");
    errno = 0;
    { int v = 0; come_array_insert_g((come_hdr_t*)&a, sizeof(int), 99, &v); }
    CHECK(a.count == 4 && errno == ERANGE, "insert past the end is rejected with ERANGE");
    { int v = 2; CHECK(come_array_contains_g(a, sizeof(int), &v), "contains hit"); }
    { int v = 7; CHECK(!come_array_contains_g(a, sizeof(int), &v), "contains miss"); }
    { int v = 2; CHECK(come_array_index_g(a, sizeof(int), &v) == 3, "index"); }
    { int v = 7; CHECK(come_array_index_g(a, sizeof(int), &v) == -1, "index miss"); }
    come_array_remove_g((come_hdr_t*)&a, sizeof(int), 0);
    CHECK(a.count == 3 && AI(a, 0) == 3, "remove at head");
    errno = 0;
    come_array_remove_g((come_hdr_t*)&a, sizeof(int), 42);
    CHECK(a.count == 3 && errno == ERANGE, "remove out of range is rejected with ERANGE");

    // sort / reverse / set / cpy / dup / clear
    come_int_array_sort(&a, cmp_int);
    CHECK(AI(a, 0) == 1 && AI(a, 1) == 2 && AI(a, 2) == 3, "sort ascending");
    come_array_reverse_g((come_hdr_t*)&a, sizeof(int));
    CHECK(AI(a, 0) == 3 && AI(a, 2) == 1, "reverse");
    { int v = 5; come_array_set_g((come_hdr_t*)&a, sizeof(int), &v); }
    CHECK(AI(a, 0) == 5 && AI(a, 2) == 5, "set fills");
    come_int_array_t b = COME_HDR_INIT(NULL);
    come_array_cpy_g((come_hdr_t*)&b, sizeof(int), a);
    CHECK(b.count == 3 && AI(b, 1) == 5, "cpy copies elements and length");
    come_int_array_t d = come_array_dup_g(NULL, a, sizeof(int));
    CHECK(d.count == 3 && AI(d, 0) == 5, "dup copies");
    AI(d, 0) = 99;
    CHECK(AI(a, 0) == 5, "dup is independent");
    come_array_clear_g((come_hdr_t*)&a);
    CHECK(a.count == 0, "clear empties");

    // growth from empty via the generic path (long elements)
    come_hdr_t g = COME_HDR_INIT(NULL);
    long lv = 10000000000L;
    come_array_append_g(&g, sizeof(long), &lv);
    long sv = 5;
    come_array_append_g(&g, sizeof(long), &sv);
    CHECK(g.count == 2 && ((long*)g.data)[0] == lv, "generic append from empty");
    CHECK(come_array_index_g(g, sizeof(long), &sv) == 1, "generic index");
    CHECK(come_array_contains_g(g, sizeof(long), &lv), "generic contains");

    // COIL-135: an alias is a header copy sharing the data
    // block. Writes before growth are shared; growth forks
    // the alias into a valid snapshot of the old block - it
    // never dangles.
    come_int_array_t x = COME_HDR_INIT(NULL);
    for (int i = 0; i < 4; i++) APP(x, i * 10);
    come_int_array_t y = x;                    // alias
    AI(y, 0) = 42;                             // write through the alias
    CHECK(AI(x, 0) == 42, "alias writes are shared before growth");
    come_array_resize_g((come_hdr_t*)&x, sizeof(int), 100000);
    CHECK(x.count == 100000, "resize grows the operating header");
    CHECK(y.count == 4, "alias keeps its snapshot length after growth");
    CHECK(AI(y, 0) == 42 && AI(y, 3) == 30, "alias snapshot data stays valid");
    come_array_resize_g((come_hdr_t*)&y, sizeof(int), 3);
    CHECK(y.count == 3 && AI(y, 2) == 20, "alias can still shrink and read");

    // byte array sort
    come_byte_array_t by = COME_HDR_INIT(NULL);
    { uint8_t v = 5; come_array_append_g((come_hdr_t*)&by, sizeof(uint8_t), &v); }
    { uint8_t v = 2; come_array_append_g((come_hdr_t*)&by, sizeof(uint8_t), &v); }
    { uint8_t v = 9; come_array_append_g((come_hdr_t*)&by, sizeof(uint8_t), &v); }
    come_byte_array_sort(&by, cmp_byte);
    CHECK(((uint8_t*)by.data)[0] == 2 && ((uint8_t*)by.data)[1] == 5 && ((uint8_t*)by.data)[2] == 9, "byte sort");

    // string list: append / contains / index / sort (by content)
    come_string_list_t names = COME_HDR_INIT(NULL);
    { come_string_t v = come_string_new(NULL, "pear"); come_array_append_g((come_hdr_t*)&names, sizeof(come_string_t), &v); }
    { come_string_t v = come_string_new(NULL, "apple"); come_array_append_g((come_hdr_t*)&names, sizeof(come_string_t), &v); }
    { come_string_t v = come_string_new(NULL, "fig"); come_array_append_g((come_hdr_t*)&names, sizeof(come_string_t), &v); }
    CHECK(names.count == 3, "string append");
    CHECK(come_array_contains_string(names, come_string_new(NULL, "fig")), "string contains by content");
    CHECK(!come_array_contains_string(names, come_string_new(NULL, "plum")), "string contains misses");
    CHECK(come_array_index_string(names, come_string_new(NULL, "apple")) == 1, "string index by content");
    come_string_list_sort(&names, cmp_str);
    CHECK(strcmp(((come_string_t*)names.data)[0].data, "apple") == 0 &&
          strcmp(((come_string_t*)names.data)[2].data, "pear") == 0, "string sort");

    if (failures) { printf("test_array: %d FAILURES\n", failures); return 1; }
    printf("test_array: all checks passed\n");
    return 0;
}
