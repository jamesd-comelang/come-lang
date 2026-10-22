#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include "come_string.h"
#include "come_array.h"
#include "come_std.h"

// COIL-114: the Come_std.md FILE tables, tested at the runtime
// level. (The Come-level calls are covered by src/std/t/01-file.co;
// vprintf/vscanf take a va_list, so they are exercised here.)

void come_std__init_local(void);
bool come_std__FILE__open(come_std__FILE_t*, come_string_t*, come_string_t*);
void come_std__FILE__close(come_std__FILE_t*);
bool come_std__FILE__fdopen(come_std__FILE_t*, int, come_string_t*);
bool come_std__FILE__reopen(come_std__FILE_t*, come_string_t*, come_string_t*);
int come_std__FILE__fileno(come_std__FILE_t*);
int come_std__FILE__printf(come_std__FILE_t*, come_string_t*, ...);
int come_std__FILE__scanf(come_std__FILE_t*, come_string_t*, ...);
int come_std__FILE__vprintf(come_std__FILE_t*, come_string_t*, va_list);
int come_std__FILE__vscanf(come_std__FILE_t*, come_string_t*, va_list);
long come_std__FILE__read(come_std__FILE_t*, come_byte_array_t*, long);
long come_std__FILE__write(come_std__FILE_t*, come_byte_array_t*, long);
int32_t come_std__FILE__getc(come_std__FILE_t*);
void come_std__FILE__putc(come_std__FILE_t*, int32_t);
come_string_t* come_std__FILE__gets(come_std__FILE_t*);
long come_std__FILE__puts(come_std__FILE_t*, come_string_t*);
come_string_t* come_std__FILE__fname(come_std__FILE_t*);
void come_std__FILE__ungetc(come_std__FILE_t*, int32_t);
void come_std__FILE__seek(come_std__FILE_t*, long, int);
long come_std__FILE__tell(come_std__FILE_t*);
void come_std__FILE__rewind(come_std__FILE_t*);
bool come_std__FILE__isopen(come_std__FILE_t*);
bool come_std__FILE__eof(come_std__FILE_t*);
bool come_std__FILE__error(come_std__FILE_t*);
void come_std__FILE__flush(come_std__FILE_t*);
void come_std__FILE__clearerr(come_std__FILE_t*);
void come_std__FILE__setbuf(come_std__FILE_t*, come_byte_array_t*, long);
void come_std__FILE__setvbuf(come_std__FILE_t*, come_byte_array_t*, int, long);
void come_std__FILE__setlinebuf(come_std__FILE_t*);
bool come_std__remove(come_string_t*);
come_std__FILE_t come_std__tmpfile(void);
come_string_t* come_std__tmpname(void);
int come_std__rand(void);
void come_std__srand(unsigned int);

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

static int call_vprintf(come_std__FILE_t* fp, come_string_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = come_std__FILE__vprintf(fp, fmt, ap);
    va_end(ap);
    return r;
}
static int call_vscanf(come_std__FILE_t* fp, come_string_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = come_std__FILE__vscanf(fp, fmt, ap);
    va_end(ap);
    return r;
}

void come_std__Proc__abort(void*);
void come_std__Proc__exit(void*, int);
void come_std__Proc__atexit(void*, void (*)(void));
come_string_t* come_std__Proc__getenv(void*, come_string_t*);
int come_std__Proc__system(void*, come_string_t*);

static come_string_t* S(const char* s) { return come_string_new(NULL, s); }

// COIL-115: callbacks used by the fork-based atexit test below.
static const char* g_order_path = "/tmp/come_test_atexit_115.txt";
static void cb_write_1(void) { FILE* u = fopen(g_order_path, "a"); if (u) { fputc('1', u); fclose(u); } }
static void cb_write_2(void) { FILE* u = fopen(g_order_path, "a"); if (u) { fputc('2', u); fclose(u); } }

int main() {
    come_std__init_local();
    const char* path = "/tmp/come_test_std_114.txt";
    remove(path);

    come_std__FILE_t f = {0};
    CHECK(!come_std__FILE__isopen(&f), "new FILE is not open");
    CHECK(come_std__FILE__fileno(&f) == -1, "unopened fileno is -1");

    errno = 0;
    CHECK(!come_std__FILE__open(&f, S("/nonexistent-dir-xyz/nope.txt"), S("r")), "open of missing file fails");
    CHECK(errno != 0, "failed open sets errno");

    // Write side
    CHECK(come_std__FILE__open(&f, S(path), S("w")), "open for write");
    CHECK(come_std__FILE__isopen(&f), "isopen after open");
    CHECK(come_std__FILE__fileno(&f) >= 0, "fileno >= 0 when open");
    CHECK(strcmp(come_std__FILE__fname(&f)->data, path) == 0, "fname records the path");
    CHECK(come_std__FILE__printf(&f, S("Count: %d\n"), 42) > 0, "printf writes");
    CHECK(call_vprintf(&f, S("via vprintf %d\n"), 7) > 0, "vprintf writes");
    CHECK(come_std__FILE__puts(&f, S("second")) == 7, "puts returns chars + newline");
    come_byte_array_t* buf = (come_byte_array_t*)malloc(sizeof(come_byte_array_t) + 3);
    buf->size = 3; buf->count = 3;
    buf->items[0] = 'h'; buf->items[1] = 'i'; buf->items[2] = '\n';
    CHECK(come_std__FILE__write(&f, buf, 3) == 3, "write returns byte count");
    come_std__FILE__putc(&f, 'Z');
    come_std__FILE__putc(&f, '\n');
    come_std__FILE__flush(&f);
    // Content: "Count: 42\n" (10) + "via vprintf 7\n" (14) +
    // "second\n" (7) + "hi\n" (3) + "Z\n" (2) = 36 bytes.
    CHECK(come_std__FILE__tell(&f) == 36, "tell after writes");
    CHECK(!come_std__FILE__error(&f), "no error flag on healthy stream");
    come_std__FILE__close(&f);
    CHECK(!come_std__FILE__isopen(&f), "not open after close");

    // Buffering controls (must run before any I/O on the stream)
    CHECK(come_std__FILE__open(&f, S(path), S("r+")), "reopen r+ for setbuf");
    come_byte_array_t* sbuf = (come_byte_array_t*)malloc(sizeof(come_byte_array_t) + 64);
    sbuf->size = 64; sbuf->count = 64;
    memset(sbuf->items, 0, 64);
    come_std__FILE__setbuf(&f, sbuf, 64);
    come_std__FILE__setvbuf(&f, sbuf, _IOFBF, 64);
    come_std__FILE__setlinebuf(&f);
    come_std__FILE__close(&f);

    // Read side
    CHECK(come_std__FILE__open(&f, S(path), S("r")), "open for read");
    int v = 0;
    CHECK(come_std__FILE__scanf(&f, S("Count: %d"), &v) == 1 && v == 42, "scanf reads int");
    come_std__FILE__rewind(&f);
    CHECK(come_std__FILE__tell(&f) == 0, "rewind resets position");
    v = 0;
    CHECK(call_vscanf(&f, S("Count: %d"), &v) == 1 && v == 42, "vscanf reads int");
    come_std__FILE__rewind(&f);
    come_string_t* l1 = come_std__FILE__gets(&f);
    CHECK(strcmp(l1->data, "Count: 42\n") == 0, "gets returns first line with newline");
    CHECK(come_std__FILE__getc(&f) == 'v', "getc reads first char of line 2");
    come_std__FILE__ungetc(&f, 'v');
    CHECK(come_std__FILE__getc(&f) == 'v', "ungetc pushes the char back");
    come_std__FILE__seek(&f, 7, SEEK_SET);
    CHECK(come_std__FILE__getc(&f) == '4', "seek + getc lands on the digit");
    come_std__FILE__seek(&f, 0, SEEK_END);
    CHECK(come_std__FILE__tell(&f) == 36, "seek END + tell gives the size");
    come_std__FILE__rewind(&f);
    come_byte_array_t* rb = (come_byte_array_t*)malloc(sizeof(come_byte_array_t) + 4);
    rb->size = 4; rb->count = 4;
    CHECK(come_std__FILE__read(&f, rb, 4) == 4 && memcmp(rb->items, "Coun", 4) == 0, "read fills the byte buffer");
    // Drain to EOF
    come_string_t* line;
    int guard = 0;
    do { line = come_std__FILE__gets(&f); } while (line->count > 0 && ++guard < 20);
    CHECK(line->count == 0, "gets at EOF returns an empty string, never null");
    CHECK(come_std__FILE__eof(&f), "eof flag set at end");
    come_std__FILE__clearerr(&f);
    CHECK(!come_std__FILE__eof(&f), "clearerr clears the eof flag");

    // fdopen adopts a duplicated fd
    come_std__FILE_t d = {0};
    CHECK(come_std__FILE__fdopen(&d, dup(come_std__FILE__fileno(&f)), S("r")), "fdopen adopts fd");
    CHECK(come_std__FILE__isopen(&d), "fdopen stream is open");
    come_std__FILE__close(&d);

    // reopen switches the stream to a new path
    CHECK(come_std__FILE__reopen(&f, S(path), S("r")), "reopen same path");
    CHECK(strcmp(come_std__FILE__gets(&f)->data, "Count: 42\n") == 0, "reopened stream reads from the start");
    come_std__FILE__close(&f);

    // Module-level operations (Come_std.md section 4)
    come_std__FILE_t t = come_std__tmpfile();
    CHECK(come_std__FILE__isopen(&t), "tmpfile is open");
    come_std__FILE__puts(&t, S("tmp data"));
    come_std__FILE__rewind(&t);
    CHECK(strcmp(come_std__FILE__gets(&t)->data, "tmp data\n") == 0, "tmpfile round-trips");
    come_std__FILE__close(&t);

    come_string_t* n1 = come_std__tmpname();
    come_string_t* n2 = come_std__tmpname();
    CHECK(n1->count > 0 && strcmp(n1->data, n2->data) != 0, "tmpname is unique and non-empty");

    come_std__srand(42);
    int a = come_std__rand();
    srand(42);
    CHECK(a == rand(), "srand/rand match the C sequence for the same seed");

    CHECK(come_std__remove(S(path)), "remove deletes the file");
    errno = 0;
    come_std__FILE_t gone = {0};
    CHECK(!come_std__FILE__open(&gone, S(path), S("r")), "removed file no longer opens");

    // COIL-115: proc & environment (Come_std.md section 5)
    setenv("COME_T115", "yes", 1);
    come_string_t* gv = come_std__Proc__getenv(NULL, S("COME_T115"));
    CHECK(gv && strcmp(gv->data, "yes") == 0, "getenv reads a set variable");
    errno = 0;
    come_string_t* gm = come_std__Proc__getenv(NULL, S("COME_T115_MISSING"));
    CHECK(gm && gm->count == 0, "getenv of a missing variable is an empty string");
    CHECK(errno == ENOENT, "getenv of a missing variable sets ENOENT");
    CHECK(come_std__Proc__system(NULL, S("true")) == 0, "system(true) is 0");
    CHECK(come_std__Proc__system(NULL, S("false")) != 0, "system(false) is non-zero");

    // atexit callbacks run in reverse order, and proc.exit
    // terminates with its status: observed from a child process.
    remove(g_order_path);
    pid_t pid = fork();
    if (pid == 0) {
        come_std__Proc__atexit(NULL, cb_write_1);
        come_std__Proc__atexit(NULL, cb_write_2);
        come_std__Proc__exit(NULL, 5);
        _exit(9);   // unreachable if proc.exit works
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid, "waitpid for atexit child");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 5, "proc.exit(5) terminates with status 5");
    FILE* of = fopen(g_order_path, "r");
    char order[8] = {0};
    if (of) { size_t n = fread(order, 1, sizeof(order) - 1, of); order[n] = 0; fclose(of); }
    CHECK(strcmp(order, "21") == 0, "atexit callbacks run in reverse registration order");
    remove(g_order_path);

    free(buf); free(sbuf); free(rb);
    if (failures) { printf("test_std: %d FAILURES\n", failures); return 1; }
    printf("test_std: all checks passed\n");
    return 0;
}
