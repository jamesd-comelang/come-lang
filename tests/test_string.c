#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "come_string.h"
#include "mem/talloc.h"

void test_basic() {
    TALLOC_CTX* ctx = mem_talloc_new_ctx(NULL);
    come_string_t* s = come_string_new(ctx, "Hello");
    
    assert(come_string_size(s) == 5);
    assert(come_string_len(s) == 5);
    assert(come_string_cmp(s, s, 0) == 0);
    
    come_string_t* s2 = come_string_new(ctx, "World");
    assert(come_string_cmp(s, s2, 0) < 0);
    
    mem_talloc_free(ctx);
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mBasic tests passed\033[0m\n");
}

void test_search() {
    TALLOC_CTX* ctx = mem_talloc_new_ctx(NULL);
    come_string_t* s = come_string_new(ctx, "Hello World");
    
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
    come_string_t* s = come_string_new(ctx, "Hello");
    
    come_string_t* upper = come_string_upper(s);
    assert(come_string_cmp(upper, s, 0) != 0);
    // Manually check content since we don't have a way to peek easily without exposing struct or using cmp with C string
    // But we can use come_string_cmp with another come_string
    come_string_t* expected = come_string_new(ctx, "HELLO");
    assert(come_string_cmp(upper, expected, 0) == 0);
    
    come_string_t* lower = come_string_lower(s);
    come_string_t* expected_lower = come_string_new(ctx, "hello");
    assert(come_string_cmp(lower, expected_lower, 0) == 0);
    
    come_string_t* repeated = come_string_repeat(s, 3);
    come_string_t* expected_repeat = come_string_new(ctx, "HelloHelloHello");
    assert(come_string_cmp(repeated, expected_repeat, 0) == 0);
    
    come_string_t* replaced = come_string_replace(s, "l", "L", 0);
    come_string_t* expected_replace = come_string_new(ctx, "HeLLo");
    assert(come_string_cmp(replaced, expected_replace, 0) == 0);

    mem_talloc_free(ctx);
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mTransform tests passed\033[0m\n");
}

void test_memory() {
    // This is hard to test automatically without hooks, but we can verify no crash
    TALLOC_CTX* root = mem_talloc_new_ctx(NULL);
    come_string_t* s = come_string_new(root, "Parent");
    
    // 'child' is allocated on 's'
    come_string_t* child = come_string_upper(s);
    assert(child != NULL);
    
    // 'stolen' is allocated on 's' initially, then moved to 'root'
    come_string_t* stolen = come_string_lower(s);
    come_string_chown(stolen, root);
    
    // Freeing 's' should free 'child' but NOT 'stolen'
    come_string_free(s);
    
    // 'stolen' should still be valid (we can't easily verify validity without crash or valgrind, but we can try to use it)
    // If it was freed, this might crash or show garbage (use after free)
    assert(come_string_len(stolen) == 6);
    
    mem_talloc_free(root);
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mMemory tests passed\033[0m\n");
}

void test_trim() {
    TALLOC_CTX* ctx = mem_talloc_new_ctx(NULL);
    come_string_t* s = come_string_new(ctx, "  Hello  ");
    
    come_string_t* trimmed = come_string_trim(s, NULL);
    come_string_t* expected = come_string_new(ctx, "Hello");
    assert(come_string_cmp(trimmed, expected, 0) == 0);
    
    come_string_t* ltrimmed = come_string_ltrim(s, NULL);
    come_string_t* expected_l = come_string_new(ctx, "Hello  ");
    assert(come_string_cmp(ltrimmed, expected_l, 0) == 0);
    
    come_string_t* rtrimmed = come_string_rtrim(s, NULL);
    come_string_t* expected_r = come_string_new(ctx, "  Hello");
    assert(come_string_cmp(rtrimmed, expected_r, 0) == 0);
    
    come_string_t* s2 = come_string_new(ctx, "__Hello__");
    come_string_t* trimmed2 = come_string_trim(s2, "_");
    assert(come_string_cmp(trimmed2, expected, 0) == 0);

    mem_talloc_free(ctx);
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mTrim tests passed\033[0m\n");
}

void test_split_join() {
    TALLOC_CTX* ctx = mem_talloc_new_ctx(NULL);
    come_string_t* s = come_string_new(ctx, "a,b,c");
    
    come_string_list_t* list = come_string_split(s, ",");
    assert(list->count == 3);
    
    come_string_t* expected_a = come_string_new(ctx, "a");
    assert(come_string_cmp(list->items[0], expected_a, 0) == 0);
    
    come_string_t* joined = come_string_join(list, come_string_new(ctx, "-"));
    come_string_t* expected_join = come_string_new(ctx, "a-b-c");
    assert(come_string_cmp(joined, expected_join, 0) == 0);
    
    come_string_list_t* list_n = come_string_split_n(s, ",", 2);
    assert(list_n->count == 2);
    come_string_t* expected_rest = come_string_new(ctx, "b,c");
    assert(come_string_cmp(list_n->items[1], expected_rest, 0) == 0);

    mem_talloc_free(ctx);
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mSplit/Join tests passed\033[0m\n");
}

void test_regex() {
    TALLOC_CTX* ctx = mem_talloc_new_ctx(NULL);
    come_string_t* s = come_string_new(ctx, "user@example.com");
    
    assert(come_string_regex(s, "^[a-z]+@[a-z]+\\.com$") == true);
    assert(come_string_regex(s, "^[0-9]+$") == false);
    
    come_string_t* text = come_string_new(ctx, "foo123bar456");
    come_string_list_t* parts = come_string_regex_split(text, "[0-9]+", 0);
    assert(parts->count == 3); // "foo", "bar", "" (trailing empty string if match at end? No, "456" is match, so split after it is empty string? Or split consumes separator?)
    // "foo" (sep 123) "bar" (sep 456) "" ?
    // Let's check logic:
    // p="foo123bar456"
    // match "123" at 3. item="foo". p="bar456"
    // match "456" at 3. item="bar". p=""
    // Loop ends. Last part len=strlen("")=0. item=""
    // So 3 items: "foo", "bar", ""
    
    come_string_t* expected_foo = come_string_new(ctx, "foo");
    assert(come_string_cmp(parts->items[0], expected_foo, 0) == 0);
    
    come_string_list_t* groups = come_string_regex_groups(s, "^([a-z]+)@([a-z]+)\\.com$");
    assert(groups->count == 3); // Full match + 2 groups
    come_string_t* expected_user = come_string_new(ctx, "user");
    assert(come_string_cmp(groups->items[1], expected_user, 0) == 0);
    
    come_string_t* replaced = come_string_regex_replace(text, "[0-9]+", "#", 0);
    come_string_t* expected_repl = come_string_new(ctx, "foo#bar#");
    assert(come_string_cmp(replaced, expected_repl, 0) == 0);

    mem_talloc_free(ctx);
    printf("\033[1;38;2;255;255;255;48;2;0;150;0mRegex tests passed\033[0m\n");
}

int main() {
    test_basic();
    test_search();
    test_transform();
    test_memory();
    test_trim();
    test_split_join();
    test_regex();
    return 0;
}
