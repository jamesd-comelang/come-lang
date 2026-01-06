#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "string_module.h"
#include "array_module.h"
#include "mem/talloc.h"
#include <errno.h>
#define come_errno_wrapper() (errno)
static const char* come_strerror() { return strerror(errno); }
#include "net/tls.h"
#include "net/http.h"
#define come_call_accept(x) _Generic((x), net_tls_listener*: net_tls_accept((net_tls_listener*)(x)))

typedef int8_t byte;
typedef int8_t i8;
typedef uint8_t ubyte;
typedef uint8_t u8;
typedef int16_t i16;
typedef uint16_t ushort;
typedef uint16_t u16;
typedef int32_t i32;
typedef uint32_t uint;
typedef uint32_t u32;
typedef int64_t i64;
typedef uint64_t ulong;
typedef uint64_t u64;
typedef float f32;
typedef double f64;
typedef int32_t wchar;
typedef void* map;
#include <math.h>
#include <stdlib.h>
#include <arpa/inet.h>

/* Runtime Preamble */
#define come_free(p) mem_talloc_free(p)
#define come_net_hton(x) htons(x)
/* Runtime Preamble additions */
TALLOC_CTX* come_global_ctx = NULL;
#define come_std_eprintf(...) fprintf(stderr, __VA_ARGS__)
void foo();
int _come_user_main(void) {
    /* AST ERROR: NULL NODE */ 0();
    return 0;
}

/* Main Wrapper */
int main(int argc, char* argv[]) {
    come_global_ctx = mem_talloc_new_ctx(NULL);
    int ret = _come_user_main();
    mem_talloc_free(come_global_ctx);
    return ret;
}

void foo(void) {
    fprintf(stdout, "foo called\n");
}

