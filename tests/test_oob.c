// COIL-106: COME_ARR_GET_T bounds checking, exercised directly
// in C. An out-of-bounds read yields the zero value and sets
// errno to ERANGE; an out-of-bounds write is discarded and
// sets ERANGE; in-bounds access is the plain data[] access.
// COIL-135: the macro takes the header value and the element
// type - the header's data pointer is void*.
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "come_array.h"
#include "come_string.h"

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("\033[1;38;2;255;255;255;48;2;200;0;0mFAIL: %s (line %d)\033[0m\n", #cond, __LINE__); \
        return 1; \
    } \
} while (0)

int main() {
    come_int_array_t a = COME_HDR_INIT(NULL);
    come_array_resize_g((come_hdr_t*)&a, sizeof(int), 3);
    ((int*)a.data)[0] = 1;
    ((int*)a.data)[1] = 2;
    ((int*)a.data)[2] = 3;

    errno = 0;
    CHECK(COME_ARR_GET_T(a, 1, int) == 2);
    CHECK(errno == 0);

    errno = 0;
    CHECK(COME_ARR_GET_T(a, 100, int) == 0);
    CHECK(errno == ERANGE);

    errno = 0;
    CHECK(COME_ARR_GET_T(a, -1, int) == 0);
    CHECK(errno == ERANGE);

    errno = 0;
    COME_ARR_GET_T(a, 100, int) = 9;
    CHECK(errno == ERANGE);
    CHECK(a.count == 3 && ((int*)a.data)[0] == 1 && ((int*)a.data)[1] == 2 && ((int*)a.data)[2] == 3);

    // A discarded OOB write must not leak into a later OOB read.
    errno = 0;
    CHECK(COME_ARR_GET_T(a, 55, int) == 0);
    CHECK(errno == ERANGE);

    COME_ARR_GET_T(a, 0, int) = 7;
    CHECK(((int*)a.data)[0] == 7);

    // String index helper: in-bounds character, OOB -> empty + ERANGE.
    come_string_t s = come_string_new(NULL, "abc");
    come_string_t ch = come_string_at(s, 1);
    CHECK(ch.count == 1 && ch.data[0] == 'b');
    errno = 0;
    come_string_t none = come_string_at(s, 99);
    CHECK(none.count == 0);
    CHECK(errno == ERANGE);

    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-106 OOB test passed!\033[0m\n");
    return 0;
}
