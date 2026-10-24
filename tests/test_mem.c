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

/* COIL-108: the exit registry replays in exact reverse
 * registration order, and running it twice is a no-op. */
static int g_exit_order[8];
static int g_exit_n = 0;
static void exit_a() { g_exit_order[g_exit_n++] = 1; }
static void exit_b() { g_exit_order[g_exit_n++] = 2; }
static void exit_c() { g_exit_order[g_exit_n++] = 3; }
static void test_exit_registry() {
    come_register_exit(exit_a);
    come_register_exit(exit_b);
    come_register_exit(exit_c);
    come_run_exits();
    assert(g_exit_n == 3);
    assert(g_exit_order[0] == 3 && g_exit_order[1] == 2 && g_exit_order[2] == 1);
    come_run_exits();
    assert(g_exit_n == 3);
    passed("Exit registry (reverse order)");
}

// COIL-157: scope contexts (spec 7). Generated code runs
// inside a stack of scope contexts; a buffer a scope
// creates dies when the scope exits, and adopt moves a
// buffer only to a scope that outlives its current one.
// (Liveness at scale is pinned by the map-scope e2e test's
// RSS bound; these pin the stack and adopt mechanics.)
void test_scope_stack() {
    void* stack = come_scope_stack_new();
    assert(stack != NULL);
    come_scope_stack_activate(stack);
    void* root = come_scope_current();
    assert(root != NULL);

    // A function body is a marker frame; blocks nest under it.
    come_scope_push_marker();
    void* body = come_scope_current();
    assert(body != NULL && body != root);
    assert(come_scope_ctx_for_depth(0) == body);
    assert(come_scope_caller_ctx() == root);

    come_scope_push();
    void* inner = come_scope_current();
    assert(inner != NULL && inner != body);
    assert(come_scope_ctx_for_depth(0) == body);
    assert(come_scope_ctx_for_depth(1) == inner);
    // Depth past the top clamps to the current frame.
    assert(come_scope_ctx_for_depth(9) == inner);

    // Pop returns to the body frame.
    come_scope_pop();
    assert(come_scope_current() == body);

    // pop_to_depth: from two nested blocks back to the body.
    come_scope_push();
    come_scope_push();
    assert(come_scope_current() != body);
    come_scope_pop_to_depth(0);
    assert(come_scope_current() == body);

    // pop_to_marker: from a nested block past the marker.
    come_scope_push();
    come_scope_pop_to_marker();
    assert(come_scope_current() == root);

    come_scope_stack_activate(NULL);
    come_scope_stack_free(stack);
    passed("Scope stack");
}

void test_scope_adopt() {
    void* stack = come_scope_stack_new();
    come_scope_stack_activate(stack);
    // Off-stack: outlives every scope on this stack.
    void* outer_ctx = mem_talloc_new_ctx(NULL);

    come_scope_push_marker();
    void* body = come_scope_current();
    come_scope_push();
    void* inner = come_scope_current();

    // A buffer born in the inner scope moves outward...
    char* buf = mem_talloc_alloc(come_scope_current(), 16);
    strcpy(buf, "keep");
    assert(come_scope_adopt(body, buf) == 1);
    // ...and now sits in the body scope: adopting it to
    // the body again, or back to the inner scope, moves
    // nothing (a destination must strictly outlive).
    assert(come_scope_adopt(body, buf) == 0);
    assert(come_scope_adopt(inner, buf) == 0);
    // Adopting to an off-stack context always moves.
    assert(come_scope_adopt(outer_ctx, buf) == 1);
    // A buffer whose parent is not a scope never moves.
    char* rooted = mem_talloc_alloc(outer_ctx, 16);
    assert(come_scope_adopt(body, rooted) == 0);
    // A NULL destination is the root: it outlives everything.
    char* buf2 = mem_talloc_alloc(come_scope_current(), 16);
    assert(come_scope_adopt(NULL, buf2) == 1);
    // NULL data is a no-op, never a crash.
    assert(come_scope_adopt(body, NULL) == 0);

    // A buffer adopted to the body survives its birth
    // scope's exit (its parent is the body, not inner).
    char* buf3 = mem_talloc_alloc(come_scope_current(), 16);
    strcpy(buf3, "stay");
    assert(come_scope_adopt(body, buf3) == 1);
    come_scope_pop(); // inner
    assert(strcmp(buf3, "stay") == 0);
    assert(come_scope_adopt(body, buf3) == 0); // already there
    come_scope_pop_to_marker();
    assert(come_scope_current() != body);

    mem_talloc_free(outer_ctx);
    mem_talloc_free(buf2);
    come_scope_stack_activate(NULL);
    come_scope_stack_free(stack);
    passed("Scope adopt");
}

void test_scope_stacks_independent() {
    void* a = come_scope_stack_new();
    void* b = come_scope_stack_new();
    come_scope_stack_activate(a);
    come_scope_push_marker();
    void* a_body = come_scope_current();
    come_scope_stack_activate(b);
    // An empty stack falls back to the root, not a_body.
    assert(come_scope_current() != a_body);
    come_scope_push_marker();
    void* b_body = come_scope_current();
    assert(b_body != a_body);
    come_scope_stack_activate(a);
    assert(come_scope_current() == a_body);
    come_scope_stack_activate(NULL);
    // Freeing a stack with live frames reaps them.
    come_scope_stack_free(a);
    come_scope_stack_free(b);
    passed("Scope stacks");
}

int main() {
    mem_talloc_module_init();
    test_scope_stack();
    test_scope_adopt();
    test_scope_stacks_independent();
    test_exit_registry();
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
