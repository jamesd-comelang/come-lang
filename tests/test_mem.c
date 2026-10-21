#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "mem/talloc.h"

/* Unit tests for the memory framework behind the mem_talloc_* API
   (src/mem/memory.c). These pin the semantics the rest of the tree
   relies on: ownership trees, subtree free, steal, and realloc.
   For strongest signal, also run under -fsanitize=address,undefined. */

static void passed(const char* name) {
    printf("\033[1;38;2;255;255;255;48;2;0;150;0m%s tests passed\033[0m\n", name);
}

void test_alloc_basics() {
    void* ctx = mem_talloc_new_ctx(NULL);
    char* a = mem_talloc_alloc(ctx, 32);
    char* b = mem_talloc_alloc(ctx, 32);
    assert(a != NULL && b != NULL && a != b);

    memset(a, 'A', 32);
    memset(b, 'B', 32);
    assert(a[0] == 'A' && a[31] == 'A');
    assert(b[0] == 'B' && b[31] == 'B');

    /* Zero-size contexts are distinct, freeable pointers. */
    void* c1 = mem_talloc_new_ctx(ctx);
    void* c2 = mem_talloc_new_ctx(ctx);
    assert(c1 != NULL && c2 != NULL && c1 != c2);

    mem_talloc_free(ctx);
    passed("Mem alloc");
}

void test_subtree_free() {
    void* ctx = mem_talloc_new_ctx(NULL);
    char* a = mem_talloc_alloc(ctx, 16);
    char* b = mem_talloc_alloc(ctx, 16);
    char* c = mem_talloc_alloc(ctx, 16);
    strcpy(a, "aaa");
    strcpy(b, "bbb");
    strcpy(c, "ccc");

    /* Freeing the middle child leaves its siblings untouched. */
    mem_talloc_free(b);
    assert(strcmp(a, "aaa") == 0);
    assert(strcmp(c, "ccc") == 0);

    /* Freeing the context frees everything left, without a crash. */
    mem_talloc_free(ctx);
    passed("Mem subtree free");
}

void test_steal() {
    void* ctx_a = mem_talloc_new_ctx(NULL);
    void* ctx_b = mem_talloc_new_ctx(NULL);
    char* leaf = mem_talloc_alloc(ctx_a, 16);
    strcpy(leaf, "moved");

    mem_talloc_steal(ctx_b, leaf);
    mem_talloc_free(ctx_a);  /* donor context goes away... */
    assert(strcmp(leaf, "moved") == 0);  /* ...the stolen chunk survives */

    mem_talloc_free(ctx_b);
    passed("Mem steal");
}

void test_realloc() {
    void* ctx = mem_talloc_new_ctx(NULL);

    /* realloc(NULL) behaves as alloc. */
    char* p = mem_talloc_realloc(ctx, NULL, 16);
    assert(p != NULL);
    strcpy(p, "grow me please");

    /* Growing preserves contents. */
    char* big = mem_talloc_realloc(NULL, p, 1 << 16);
    assert(big != NULL);
    assert(strcmp(big, "grow me please") == 0);

    /* Shrinking preserves the prefix. */
    char* small = mem_talloc_realloc(NULL, big, 5);
    assert(small != NULL);
    assert(strncmp(small, "grow ", 5) == 0);

    /* realloc to size 0 frees and returns NULL. */
    assert(mem_talloc_realloc(NULL, small, 0) == NULL);

    mem_talloc_free(ctx);
    passed("Mem realloc");
}

void test_realloc_with_children() {
    void* ctx = mem_talloc_new_ctx(NULL);
    char* parent = mem_talloc_alloc(ctx, 32);
    char* child = mem_talloc_alloc(parent, 32);
    strcpy(parent, "parent");
    strcpy(child, "child");

    /* Growing the parent (large enough to force a move) must keep the
       child attached: its data stays readable through the child
       pointer, and freeing the parent afterwards frees the child via
       the rewired parent pointer — a stale pointer would corrupt the
       heap or crash here. */
    char* moved = mem_talloc_realloc(NULL, parent, 1 << 20);
    assert(moved != NULL);
    assert(strcmp(moved, "parent") == 0);
    assert(strcmp(child, "child") == 0);
    strcpy(child, "child still writable");
    assert(strcmp(child, "child still writable") == 0);

    mem_talloc_free(ctx);
    passed("Mem realloc with children");
}

void test_deep_tree() {
    void* top = mem_talloc_new_ctx(NULL);
    void* cur = top;
    for (int i = 0; i < 1000; i++)
        cur = mem_talloc_new_ctx(cur);
    char* leaf = mem_talloc_alloc(cur, 8);
    assert(leaf != NULL);
    strcpy(leaf, "deep");

    /* One free at the top releases the whole chain. */
    mem_talloc_free(top);
    passed("Mem deep tree");
}

void test_init_shutdown() {
    mem_talloc_module_shutdown();
    /* Allocation lazily re-initializes the root context. */
    void* ctx = mem_talloc_new_ctx(NULL);
    assert(ctx != NULL);
    mem_talloc_free(ctx);
    mem_talloc_module_shutdown();
    passed("Mem init/shutdown");
}

int main() {
    mem_talloc_module_init();
    test_alloc_basics();
    test_subtree_free();
    test_steal();
    test_realloc();
    test_realloc_with_children();
    test_deep_tree();
    test_init_shutdown();
    mem_talloc_module_shutdown();
    return 0;
}
