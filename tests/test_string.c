#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include "come_string.h"
#include "mem/talloc.h"

// COIL-135: strings are header VALUES (data pointer, byte count,
// capacity, owner context); every call below passes and returns
// values, never pointers to a string struct.

void test_basic() {
    TALLOC_CTX* ctx = mem_talloc_new_ctx(NULL);
    come_string_t s = come_string_new(ctx, "Hello");

    assert(come_string_size(s) == 5);
    assert(come_string_len(s) == 5);
    assert(come_string_cmp(s, s, 0) == 0);

    come_string_t s2 = come_string_new(ctx, "World");
    assert(come_string_cmp(s, s2, 0) < 0);

    mem_talloc_free(ctx);
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mBasic tests passed\033[0m\n");
}

void test_search() {
    TALLOC_CTX* ctx = mem_talloc_new_ctx(NULL);
    come_string_t s = come_string_new(ctx, "Hello World");

    assert(come_string_chr(s, 'H') == 0);
    assert(come_string_chr(s, 'o') == 4);
    assert(come_string_rchr(s, 'o') == 7);
    assert(come_string_find(s, "World") == 6);
    assert(come_string_count(s, "l") == 3);

    mem_talloc_free(ctx);
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mSearch tests passed\033[0m\n");
}

void test_transform() {
    TALLOC_CTX* ctx = mem_talloc_new_ctx(NULL);
    come_string_t s = come_string_new(ctx, "Hello");

    come_string_t upper = come_string_upper(s);
    assert(come_string_cmp(upper, s, 0) != 0);
    come_string_t expected = come_string_new(ctx, "HELLO");
    assert(come_string_cmp(upper, expected, 0) == 0);

    come_string_t lower = come_string_lower(s);
    come_string_t expected_lower = come_string_new(ctx, "hello");
    assert(come_string_cmp(lower, expected_lower, 0) == 0);

    come_string_t repeated = come_string_repeat(s, 3);
    come_string_t expected_repeat = come_string_new(ctx, "HelloHelloHello");
    assert(come_string_cmp(repeated, expected_repeat, 0) == 0);

    come_string_t replaced = come_string_replace(s, "l", "L", 0);
    come_string_t expected_replace = come_string_new(ctx, "HeLLo");
    assert(come_string_cmp(replaced, expected_replace, 0) == 0);

    mem_talloc_free(ctx);
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mTransform tests passed\033[0m\n");
}

void test_memory() {
    // Ownership travels with the header: transformations
    // allocate in the source string's context, and chown moves
    // a string's data block to a new context (COIL-135).
    TALLOC_CTX* root = mem_talloc_new_ctx(NULL);
    TALLOC_CTX* sub = mem_talloc_new_ctx(root);
    come_string_t s = come_string_new(sub, "Parent");

    // 'child' is allocated in the source's context (sub).
    come_string_t child = come_string_upper(s);
    assert(child.data != NULL);

    // 'stolen' starts in sub, then moves to root.
    come_string_t stolen = come_string_lower(s);
    come_string_chown(&stolen, root);

    // Freeing sub frees s and child but NOT stolen.
    mem_talloc_free(sub);

    // 'stolen' must still be valid.
    assert(come_string_len(stolen) == 6);

    // come_string_free releases one string's data block (the
    // caller's header copy is a value; it cannot be zeroed
    // through it - generated code frees through the slot).
    come_string_t scratch = come_string_new(root, "scratch");
    come_string_free(scratch);
    come_string_t after = come_string_new(root, "after");
    assert(come_string_len(after) == 5);

    mem_talloc_free(root);
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mMemory tests passed\033[0m\n");
}

void test_trim() {
    TALLOC_CTX* ctx = mem_talloc_new_ctx(NULL);
    come_string_t s = come_string_new(ctx, "  Hello  ");

    come_string_t trimmed = come_string_trim(s, NULL);
    come_string_t expected = come_string_new(ctx, "Hello");
    assert(come_string_cmp(trimmed, expected, 0) == 0);

    come_string_t ltrimmed = come_string_ltrim(s, NULL);
    come_string_t expected_l = come_string_new(ctx, "Hello  ");
    assert(come_string_cmp(ltrimmed, expected_l, 0) == 0);

    come_string_t rtrimmed = come_string_rtrim(s, NULL);
    come_string_t expected_r = come_string_new(ctx, "  Hello");
    assert(come_string_cmp(rtrimmed, expected_r, 0) == 0);

    come_string_t s2 = come_string_new(ctx, "__Hello__");
    come_string_t trimmed2 = come_string_trim(s2, "_");
    assert(come_string_cmp(trimmed2, expected, 0) == 0);

    mem_talloc_free(ctx);
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mTrim tests passed\033[0m\n");
}

void test_split_join() {
    TALLOC_CTX* ctx = mem_talloc_new_ctx(NULL);
    come_string_t s = come_string_new(ctx, "a,b,c");

    come_string_list_t list = come_string_split(s, ",");
    assert(list.count == 3);

    come_string_t expected_a = come_string_new(ctx, "a");
    assert(come_string_cmp(((come_string_t*)list.data)[0], expected_a, 0) == 0);

    come_string_t joined = come_string_join(list, come_string_new(ctx, "-"));
    come_string_t expected_join = come_string_new(ctx, "a-b-c");
    assert(come_string_cmp(joined, expected_join, 0) == 0);

    come_string_list_t list_n = come_string_split_n(s, ",", 2);
    assert(list_n.count == 2);
    come_string_t expected_rest = come_string_new(ctx, "b,c");
    assert(come_string_cmp(((come_string_t*)list_n.data)[1], expected_rest, 0) == 0);

    mem_talloc_free(ctx);
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mSplit/Join tests passed\033[0m\n");
}

void test_regex() {
    TALLOC_CTX* ctx = mem_talloc_new_ctx(NULL);
    come_string_t s = come_string_new(ctx, "user@example.com");

    assert(come_string_regex(s, "^[a-z]+@[a-z]+\\.com$") == true);
    assert(come_string_regex(s, "^[0-9]+$") == false);

    come_string_t text = come_string_new(ctx, "foo123bar456");
    come_string_list_t parts = come_string_regex_split(text, "[0-9]+", 0);
    assert(parts.count == 3); // "foo", "bar", "" (see COIL-117 notes)

    come_string_t expected_foo = come_string_new(ctx, "foo");
    assert(come_string_cmp(((come_string_t*)parts.data)[0], expected_foo, 0) == 0);

    come_string_list_t groups = come_string_regex_groups(s, "^([a-z]+)@([a-z]+)\\.com$");
    assert(groups.count == 3); // Full match + 2 groups
    come_string_t expected_user = come_string_new(ctx, "user");
    assert(come_string_cmp(((come_string_t*)groups.data)[1], expected_user, 0) == 0);

    come_string_t replaced = come_string_regex_replace(text, "[0-9]+", "#", 0);
    come_string_t expected_repl = come_string_new(ctx, "foo#bar#");
    assert(come_string_cmp(replaced, expected_repl, 0) == 0);

    mem_talloc_free(ctx);
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mRegex tests passed\033[0m\n");
}

// COIL-110: come_string_wchar_at_e decodes the character at a
// character index; come_string_len counts characters, not bytes.
void test_wchar_at() {
    TALLOC_CTX* ctx = mem_talloc_new_ctx(NULL);
    // a(1B) U+5B57(3B) b(1B) U+1F642(4B): 4 chars, 9 bytes
    come_string_t s = come_string_new(ctx, "a\xe5\xad\x97" "b\xf0\x9f\x99\x82");
    assert(come_string_len(s) == 4);
    assert(come_string_size(s) == 9);
    assert(come_string_wchar_at_e(s, 0) == 'a');
    assert(come_string_wchar_at_e(s, 1) == 0x5B57);
    assert(come_string_wchar_at_e(s, 2) == 'b');
    assert(come_string_wchar_at_e(s, 3) == 0x1F642);
    errno = 0;
    assert(come_string_wchar_at_e(s, 4) == 0);
    assert(errno == ERANGE);
    errno = 0;
    assert(come_string_wchar_at_e(s, -1) == 0);
    assert(errno == ERANGE);
    errno = 0;
    come_string_t empty = {0};
    assert(come_string_wchar_at_e(empty, 0) == 0);
    assert(errno == ERANGE);
    mem_talloc_free(ctx);
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mWchar-at (COIL-110) tests passed\033[0m\n");
}

static int call_vsprintf_check(TALLOC_CTX* ctx, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    come_string_t s = come_string_vsprintf(ctx, fmt, ap);
    va_end(ap);
    return s.data && strcmp(s.data, "x=3") == 0;
}
static int call_vsscanf_check(come_string_t s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = come_string_vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

void test_coil117() {
    TALLOC_CTX* ctx = mem_talloc_new_ctx(NULL);
    // utf8 validation
    assert(come_string_utf8(come_string_new(ctx, "plain ascii")));
    assert(come_string_utf8(come_string_new(ctx, "héllo 🙂")));
    assert(come_string_utf8(come_string_new(ctx, "")));
    come_string_t bad1 = come_string_new_len(ctx, "\xC3\x28", 2);   // bad continuation
    assert(!come_string_utf8(bad1));
    come_string_t bad2 = come_string_new_len(ctx, "\xC0\x80", 2);   // overlong NUL
    assert(!come_string_utf8(bad2));
    come_string_t bad3 = come_string_new_len(ctx, "\xED\xA0\x80", 3); // surrogate
    assert(!come_string_utf8(bad3));
    come_string_t bad4 = come_string_new_len(ctx, "\xE2\x82", 2);   // truncated
    assert(!come_string_utf8(bad4));
    // conversions
    assert(come_string_tol(come_string_new(ctx, "42")) == 42);
    assert(come_string_tol_base(come_string_new(ctx, "ff"), 16) == 255);
    assert(come_string_tol_base(come_string_new(ctx, "0x1F"), 0) == 31);
    errno = 0;
    assert(come_string_tol(come_string_new(ctx, "abc")) == 0);
    assert(errno == EINVAL);
    double d = come_string_tod(come_string_new(ctx, "3.5"));
    assert(d > 3.4 && d < 3.6);
    errno = 0;
    assert(come_string_tod(come_string_new(ctx, "zz")) == 0);
    assert(errno == EINVAL);
    assert(come_string_toi(come_string_new(ctx, "17")) == 17);
    double fv = come_string_tof(come_string_new(ctx, "2.25"));
    assert(fv > 2.2 && fv < 2.3);
    // formatted I/O
    come_string_t f1 = come_string_sprintf(ctx, "%s-%d", "n", 7);
    assert(f1.data && strcmp(f1.data, "n-7") == 0);
    assert(call_vsprintf_check(ctx, "x=%d", 3));
    int v = 0;
    assert(come_string_sscanf(come_string_new(ctx, "123"), "%d", &v) == 1 && v == 123);
    v = 0;
    assert(call_vsscanf_check(come_string_new(ctx, "77"), "%d", &v) == 1 && v == 77);
    mem_talloc_free(ctx);
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mCOIL-117 string toolbox (C) tests passed\033[0m\n");
}

int main() {
    test_basic();
    test_search();
    test_transform();
    test_memory();
    test_trim();
    test_split_join();
    test_regex();
    test_wchar_at();
    test_coil117();
    return 0;
}
