#include "come_string.h"
#include "mem/talloc.h"
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <regex.h>

// COIL-135: a string is a header value {data, count(bytes), cap, ctx}
// plus a context-owned byte block. A zeroed header (data == NULL)
// reads as the empty string everywhere below.
static const char* sdata(come_string_t a) {
    return a.data ? a.data : "";
}
static void* sctx(come_string_t a) {
    return a.ctx;
}

// Module Initialization
void come_string__init_local() {
    // String module initialization (currently none)
}

void come_string__exit_local() {
    // String module cleanup (currently none)
}

come_string_t come_string_new_len(TALLOC_CTX* ctx, const char* str, size_t len) {
    come_string_t s;
    s.data = (char*)mem_talloc_alloc(ctx, len + 1);
    s.count = (uint32_t)len;
    s.cap = (uint32_t)(len + 1);
    s.ctx = ctx;
    if (!s.data) { s.count = 0; s.cap = 0; return s; }
    if (str && len) memcpy(s.data, str, len);
    else if (len) memset(s.data, 0, len);
    s.data[len] = '\0';
    return s;
}

come_string_t come_string_new(TALLOC_CTX* ctx, const char* str) {
    return come_string_new_len(ctx, str, str ? strlen(str) : 0);
}

// Concatenation (spec 6.5: `+` produces a new string; the
// operands are unchanged because strings are immutable).
come_string_t come_string_concat(TALLOC_CTX* ctx, come_string_t a, come_string_t b) {
    size_t la = a.count;
    size_t lb = b.count;
    come_string_t s;
    s.data = (char*)mem_talloc_alloc(ctx, la + lb + 1);
    s.count = (uint32_t)(la + lb);
    s.cap = (uint32_t)(la + lb + 1);
    s.ctx = ctx;
    if (!s.data) { s.count = 0; s.cap = 0; return s; }
    if (la) memcpy(s.data, sdata(a), la);
    if (lb) memcpy(s.data + la, sdata(b), lb);
    s.data[la + lb] = '\0';
    return s;
}

// Design 2 (redesign): concat into the bump arena. The
// result is a temporary: it dies with its scope frame
// (bulk-free at pop), so no talloc chunk or adoption
// bookkeeping is needed. ctx is the current scope, as
// for a talloc temporary, so .owner() reads the same;
// the data itself is not a talloc chunk, so a scope pop
// never frees it - the arena reset does.
come_string_t come_string_concat_arena(come_string_t a, come_string_t b) {
    size_t la = a.count;
    size_t lb = b.count;
    come_string_t s;
    s.data = (char*)come_arena_alloc(la + lb + 1);
    s.count = (uint32_t)(la + lb);
    s.cap = (uint32_t)(la + lb + 1);
    s.ctx = come_scope_current();
    if (!s.data) { s.count = 0; s.cap = 0; return s; }
    if (la) memcpy(s.data, sdata(a), la);
    if (lb) memcpy(s.data + la, sdata(b), lb);
    s.data[la + lb] = '\0';
    return s;
}

// Design 2 (redesign): promote an arena string to a
// talloc copy owned by dst. The safety net when an
// arena value reaches an adoption site (a storing
// position the promotion flag missed): copying is
// always safe, where re-parenting an arena pointer
// would corrupt the talloc tree.
come_string_t come_string_promote(come_string_t h, TALLOC_CTX* dst) {
    return come_string_new_len(dst, sdata(h), h.count);
}

void come_string_free(come_string_t str) {
    // Design 2 (redesign): an arena string is freed by
    // its frame's arena reset, not here; freeing an
    // arena pointer as a talloc chunk would corrupt.
    if (come_arena_contains(str.data)) return;
    mem_talloc_free(str.data);
}

uint32_t come_string_size(come_string_t a) {
    return a.count;
}

// Character count: scan the UTF-8 content.
uint32_t come_string_len(come_string_t a) {
    // Design 3 (redesign): bounded by the byte count,
    // not by NUL, so a view (which need not contain a
    // NUL at its end) counts only its own characters.
    // For owned strings this is identical: they are
    // NUL-terminated with no embedded NUL, so the count
    // bytes are exactly the bytes to the terminator.
    uint32_t count = 0;
    const char* p = sdata(a);
    for (uint32_t i = 0; i < a.count; i++) {
        if ((p[i] & 0xC0) != 0x80) count++;
    }
    return count;
}

int come_string_cmp(come_string_t a, come_string_t b, size_t n) {
    // Design 3 (redesign): count-bounded, like len.
    const char* p1 = sdata(a);
    const char* p2 = sdata(b);
    if (n == 0) {
        size_t m = a.count < b.count ? a.count : b.count;
        int c = m ? memcmp(p1, p2, m) : 0;
        if (c != 0) return c;
        if (a.count < b.count) return -1;
        if (a.count > b.count) return 1;
        return 0;
    }
    size_t i1 = 0, i2 = 0, chars = 0;
    while (chars < n && i1 < a.count && i2 < b.count) {
        if (p1[i1] != p2[i2]) return (unsigned char)p1[i1] - (unsigned char)p2[i2];
        do { i1++; } while (i1 < a.count && ((p1[i1] & 0xC0) == 0x80));
        do { i2++; } while (i2 < b.count && ((p2[i2] & 0xC0) == 0x80));
        chars++;
    }
    if (chars == n) return 0;
    if (i1 >= a.count && i2 >= b.count) return 0;
    if (i1 >= a.count) return -1;
    if (i2 >= b.count) return 1;
    return (unsigned char)p1[i1] - (unsigned char)p2[i2];
}

int come_string_casecmp(come_string_t a, come_string_t b, size_t n) {
    // Design 3 (redesign): count-bounded, like cmp.
    const char* p1 = sdata(a);
    const char* p2 = sdata(b);
    size_t i1 = 0, i2 = 0, chars = 0;
    while ((n == 0 || chars < n) && i1 < a.count && i2 < b.count) {
        if (tolower((unsigned char)p1[i1]) != tolower((unsigned char)p2[i2]))
            return tolower((unsigned char)p1[i1]) - tolower((unsigned char)p2[i2]);
        do { i1++; } while (i1 < a.count && ((p1[i1] & 0xC0) == 0x80));
        do { i2++; } while (i2 < b.count && ((p2[i2] & 0xC0) == 0x80));
        chars++;
    }
    if (n != 0 && chars == n) return 0;
    if (i1 >= a.count && i2 >= b.count) return 0;
    if (i1 >= a.count) return -1;
    if (i2 >= b.count) return 1;
    return tolower((unsigned char)p1[i1]) - tolower((unsigned char)p2[i2]);
}

long come_string_chr(come_string_t a, int c) {
    // Design 3 follow-up (redesign): bounded by the byte
    // count, not by NUL, so a non-suffix view does not
    // match characters past its end in its parent.
    const void* p = memchr(sdata(a), c, a.count);
    return p ? ((const char*)p - sdata(a)) : -1;
}

long come_string_rchr(come_string_t a, int c) {
    // Design 3 follow-up (redesign): backward scan within
    // the byte count (strrchr would run to the parent's
    // NUL and could return an offset past the view).
    const char* p = sdata(a);
    for (long i = (long)a.count - 1; i >= 0; i--) {
        if (p[i] == (char)c) return i;
    }
    return -1;
}

long come_string_memchr(come_string_t a, int c, size_t n) {
    const void* p = memchr(sdata(a), c, n > a.count ? a.count : n);
    return p ? ((const char*)p - sdata(a)) : -1;
}

// Design 3 follow-up (redesign): bounded substring search
// within [from, hay_len): the byte offset of the first
// occurrence of needle, or -1. Unlike strstr, it never
// reads past the receiver's byte count, so it is correct
// for views. (The needle itself is a bare C string - a
// view passed as a needle over-reads into its parent;
// bounded needle arguments are a follow-up.)
static long str_find_bounded(const char* hay, size_t hay_len,
                             const char* needle, size_t needle_len,
                             size_t from) {
    if (needle_len == 0) return from <= hay_len ? (long)from : -1;
    if (needle_len > hay_len) return -1;
    if (from > hay_len - needle_len) return -1;
    for (size_t i = from; i <= hay_len - needle_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) return (long)i;
    }
    return -1;
}

long come_string_find(come_string_t a, const char* sub) {
    if (!sub) return -1;
    return str_find_bounded(sdata(a), a.count, sub, strlen(sub), 0);
}

long come_string_rfind(come_string_t a, const char* sub) {
    size_t sub_len = strlen(sub);
    if (sub_len > a.count) return -1;

    for (long i = (long)a.count - (long)sub_len; i >= 0; i--) {
        if (strncmp(sdata(a) + i, sub, sub_len) == 0) return i;
    }
    return -1;
}

uint32_t come_string_count(come_string_t a, const char* sub) {
    uint32_t count = 0;
    if (!sub) return 0;
    size_t sub_len = strlen(sub);
    if (sub_len == 0) return 0;

    size_t from = 0;
    long hit;
    while ((hit = str_find_bounded(sdata(a), a.count, sub, sub_len, from)) >= 0) {
        count++;
        from = (size_t)hit + sub_len;
    }
    return count;
}

// Validation
bool come_string_isdigit(come_string_t a) {
    for (size_t i = 0; i < a.count; i++) {
        if (!isdigit((unsigned char)sdata(a)[i])) return false;
    }
    return true;
}

bool come_string_isalpha(come_string_t a) {
    for (size_t i = 0; i < a.count; i++) {
        if (!isalpha((unsigned char)sdata(a)[i])) return false;
    }
    return true;
}

bool come_string_isalnum(come_string_t a) {
    for (size_t i = 0; i < a.count; i++) {
        if (!isalnum((unsigned char)sdata(a)[i])) return false;
    }
    return true;
}

bool come_string_isspace(come_string_t a) {
    for (size_t i = 0; i < a.count; i++) {
        if (!isspace((unsigned char)sdata(a)[i])) return false;
    }
    return true;
}

bool come_string_isascii(come_string_t a) {
    // Design 3 follow-up (redesign): bounded by count
    // (was a NUL walk, which scanned past a view's end
    // into its parent and could report non-ASCII for an
    // ASCII view).
    const unsigned char* bytes = (const unsigned char*)sdata(a);
    for (size_t i = 0; i < a.count; i++) {
        if (bytes[i] & 0x80) return false;
    }
    return true;
}

// Transformation (derived strings are owned by the source's context)
come_string_t come_string_upper(come_string_t a) {
    come_string_t s = come_string_new_len(sctx(a), sdata(a), a.count);
    for (size_t i = 0; i < s.count; i++) {
        s.data[i] = (char)toupper((unsigned char)s.data[i]);
    }
    return s;
}

come_string_t come_string_lower(come_string_t a) {
    come_string_t s = come_string_new_len(sctx(a), sdata(a), a.count);
    for (size_t i = 0; i < s.count; i++) {
        s.data[i] = (char)tolower((unsigned char)s.data[i]);
    }
    return s;
}

come_string_t come_string_repeat(come_string_t a, size_t n) {
    size_t clen = a.count;
    size_t new_len = clen * n;
    come_string_t s = come_string_new_len(sctx(a), NULL, new_len);
    if (!s.data) return s;
    for (size_t i = 0; i < n; i++) {
        memcpy(s.data + (i * clen), sdata(a), clen);
    }
    s.data[new_len] = '\0';
    return s;
}

come_string_t come_string_replace(come_string_t a, const char* old_str, const char* new_str, size_t n) {
    size_t old_len = old_str ? strlen(old_str) : 0;
    size_t new_len_part = new_str ? strlen(new_str) : 0;

    // Design 3 follow-up (redesign): the no-op copy is
    // bounded (was come_string_new, a strlen to the
    // parent's NUL for a view).
    if (old_len == 0) return come_string_new_len(sctx(a), sdata(a), a.count);

    size_t count = 0;
    size_t from = 0;
    long hit;
    while ((hit = str_find_bounded(sdata(a), a.count, old_str, old_len, from)) >= 0) {
        count++;
        from = (size_t)hit + old_len;
        if (n > 0 && count >= n) break;
    }

    size_t alen = a.count;
    size_t final_len = alen + count * (new_len_part - old_len);
    come_string_t res = come_string_new_len(sctx(a), NULL, final_len);
    if (!res.data) return res;

    size_t pos = 0;
    from = 0;
    char* dest = res.data;
    size_t matches = 0;
    while ((hit = str_find_bounded(sdata(a), a.count, old_str, old_len, from)) >= 0 &&
           (n == 0 || matches < n)) {
        size_t segment_len = (size_t)hit - pos;
        memcpy(dest, sdata(a) + pos, segment_len);
        dest += segment_len;
        memcpy(dest, new_str, new_len_part);
        dest += new_len_part;
        pos = (size_t)hit + old_len;
        from = pos;
        matches++;
    }
    memcpy(dest, sdata(a) + pos, alen - pos);
    return res;
}

// dup(): an independent copy of the string, owned by the
// source string's context like the other derived strings.
come_string_t come_string_dup(come_string_t a) {
    return come_string_new_len(a.ctx, sdata(a), a.count);
}

void come_string_chown(come_string_t* a, TALLOC_CTX* new_ctx) {
    if (a->data) mem_talloc_steal(new_ctx, a->data);
    a->ctx = new_ctx;
}

// Helper for trim
static bool is_cutset(char c, const char* cutset) {
    if (!cutset) return isspace((unsigned char)c);
    return strchr(cutset, c) != NULL;
}

come_string_t come_string_trim(come_string_t a, const char* cutset) {
    size_t start = 0;
    size_t end = a.count;

    while (start < end && is_cutset(sdata(a)[start], cutset)) start++;
    while (end > start && is_cutset(sdata(a)[end - 1], cutset)) end--;

    return come_string_new_len(sctx(a), sdata(a) + start, end - start);
}

come_string_t come_string_ltrim(come_string_t a, const char* cutset) {
    size_t start = 0;
    size_t end = a.count;

    while (start < end && is_cutset(sdata(a)[start], cutset)) start++;

    return come_string_new_len(sctx(a), sdata(a) + start, end - start);
}

come_string_t come_string_rtrim(come_string_t a, const char* cutset) {
    size_t start = 0;
    size_t end = a.count;

    while (end > start && is_cutset(sdata(a)[end - 1], cutset)) end--;

    return come_string_new_len(sctx(a), sdata(a) + start, end - start);
}

// Split into a fresh string list owned by the source's context;
// elements are header values in the list's data block.
static come_string_list_t split_collect(come_string_t a, const char* sep, size_t n) {
    come_string_list_t list = COME_HDR_INIT(sctx(a));
    size_t sep_len = sep ? strlen(sep) : 0;
    if (sep_len == 0) {
        // Design 3 follow-up (redesign): bounded copy
        // (was come_string_new, a strlen to the parent's
        // NUL for a view).
        come_string_t whole = come_string_new_len(sctx(a), sdata(a), a.count);
        come_array_append_g((come_hdr_t*)&list, sizeof(come_string_t), &whole);
        return list;
    }

    size_t pos = 0;
    size_t matches = 0;
    for (;;) {
        long hit = str_find_bounded(sdata(a), a.count, sep, sep_len, pos);
        come_string_t part;
        if (hit >= 0 && (n == 0 || matches < n - 1)) {
            part = come_string_new_len(sctx(a), sdata(a) + pos, (size_t)hit - pos);
            pos = (size_t)hit + sep_len;
            matches++;
        } else {
            part = come_string_new_len(sctx(a), sdata(a) + pos, a.count - pos);
            come_array_append_g((come_hdr_t*)&list, sizeof(come_string_t), &part);
            break;
        }
        come_array_append_g((come_hdr_t*)&list, sizeof(come_string_t), &part);
    }
    return list;
}

come_string_list_t come_string_split_n(come_string_t a, const char* sep, size_t n) {
    come_string_list_t empty = COME_HDR_INIT(sctx(a));
    if (!sep) return empty;
    return split_collect(a, sep, n);
}

come_string_list_t come_string_split(come_string_t a, const char* sep) {
    return come_string_split_n(a, sep, 0);
}

come_string_t come_string_join(come_string_list_t list, come_string_t sep) {
    void* ctx = list.ctx;
    size_t sep_len = sep.count;
    size_t total_len = 0;
    uint32_t n = list.count;
    come_string_t* items = (come_string_t*)list.data;
    for (size_t i = 0; i < n; i++) {
        total_len += items[i].count;
        if (i + 1 < n) total_len += sep_len;
    }

    come_string_t res = come_string_new_len(ctx, NULL, total_len);
    if (!res.data) return res;

    char* p = res.data;
    for (size_t i = 0; i < n; i++) {
        memcpy(p, sdata(items[i]), items[i].count);
        p += items[i].count;
        if (i + 1 < n) {
            memcpy(p, sdata(sep), sep_len);
            p += sep_len;
        }
    }
    *p = '\0';
    return res;
}

come_string_t come_string_substr(come_string_t a, size_t start, size_t end) {
    // start/end are character indices, not bytes: walk the UTF-8.
    // Design 3 follow-up (redesign): bounded by the byte
    // count, not by NUL, so a substr of a view clamps to
    // the view instead of running into its parent.
    const char* p = sdata(a);
    const char* end_p_limit = sdata(a) + a.count;
    size_t char_idx = 0;
    const char* start_p = NULL;
    const char* end_p = NULL;

    while (p < end_p_limit) {
        if (char_idx == start) start_p = p;
        if (char_idx == end) { end_p = p; break; }
        do { p++; } while (p < end_p_limit && (*p & 0xC0) == 0x80);
        char_idx++;
    }

    if (char_idx == start) start_p = p; // Start at end of string
    if (!end_p) end_p = p; // End beyond string

    if (!start_p) start_p = end_p; // Out of bounds
    if (start_p > end_p) start_p = end_p;

    return come_string_new_len(a.ctx, start_p, (size_t)(end_p - start_p));
}

// Design 3 (redesign): a string view. Same character
// indices and clamping as substr, but no allocation:
// the header points into the parent's byte block
// (count is bytes, cap is 0 - a view never grows in
// place; growing forks a fresh owned block, COIL-135).
// The view borrows the parent: it must not outlive
// the parent's scope. The compiler guarantees that by
// promoting a view to a copy (substr) at every storing
// position (assignment, return, container/field store,
// escaping call argument); a view that survives to
// codegen is a temporary consumed within its statement,
// where the parent is in scope and therefore alive.
// Views compose: a view of a view is a subrange of the
// original parent's block, so the walk below is bounded
// by the input's byte count (not by NUL, which a view
// need not contain at its end).
come_string_t come_string_view(come_string_t a, size_t start, size_t end) {
    const char* base = sdata(a);
    size_t total = a.count;
    size_t byte_idx = 0;
    size_t char_idx = 0;
    const char* start_p = NULL;
    const char* end_p = NULL;

    while (byte_idx < total) {
        if (char_idx == start) start_p = base + byte_idx;
        if (char_idx == end) { end_p = base + byte_idx; break; }
        do { byte_idx++; } while (byte_idx < total && ((base[byte_idx] & 0xC0) == 0x80));
        char_idx++;
    }

    if (char_idx == start) start_p = base + total; // Start at end of string
    if (!end_p) end_p = base + total; // End beyond string

    if (!start_p) start_p = end_p; // Out of bounds
    if (start_p > end_p) start_p = end_p;

    come_string_t v;
    v.data = (char*)start_p;
    v.count = (uint32_t)(end_p - start_p);
    v.cap = 0;
    v.ctx = a.ctx;
    return v;
}

// Design 3 (redesign): a byte view. Same as a string
// view but over byte indices (no character walk), for
// byte-level work. Same borrowing rule as above.
come_byte_array_t come_string_byte_view(come_string_t a, size_t start, size_t end) {
    const char* base = sdata(a);
    size_t total = a.count;
    if (start > total) start = total;
    if (end > total) end = total;
    if (start > end) start = end;
    come_byte_array_t v = COME_HDR_INIT(a.ctx);
    v.data = (uint8_t*)(base + start);
    v.count = (uint32_t)(end - start);
    v.cap = 0;
    return v;
}

// Design 3 (redesign): the owned copy of a byte range,
// used when a byte view reaches a storing position and
// must promote (same clamping as byte_view).
come_byte_array_t come_string_byte_range(come_string_t a, size_t start, size_t end) {
    const char* base = sdata(a);
    size_t total = a.count;
    if (start > total) start = total;
    if (end > total) end = total;
    if (start > end) start = end;
    come_byte_array_t ba = COME_HDR_INIT(a.ctx);
    size_t len = end - start;
    ba.data = (uint8_t*)mem_talloc_alloc(a.ctx, len ? len : 1);
    if (!ba.data) return ba;
    if (len) memcpy(ba.data, base + start, len);
    ba.count = (uint32_t)len;
    ba.cap = (uint32_t)len;
    return ba;
}

// Regex
// Design 3 follow-up (redesign): a NUL-terminated copy of
// a string's bytes, owned by the current scope (freed at
// frame pop, so loop iterations don't accumulate it). For
// regex and numeric parse, which take bare C strings and
// read to NUL: running them on a view directly would read
// past the view's end into its parent. The copy is exactly
// the receiver's bytes, so match offsets relative to it
// match offsets within the receiver, and the results built
// from it are independent copies that outlive it.
static come_string_t str_nul_copy(come_string_t a) {
    return come_string_new_len(come_scope_current(), sdata(a), a.count);
}

bool come_string_regex(come_string_t a, const char* pattern) {
    if (!pattern) return false;
    regex_t regex;
    if (regcomp(&regex, pattern, REG_EXTENDED | REG_NOSUB) != 0) { come_ERR_set(EINVAL, "invalid regular expression"); return false; }

    come_string_t tmp = str_nul_copy(a);
    int ret = regexec(&regex, sdata(tmp), 0, NULL, 0);
    regfree(&regex);
    return ret == 0;
}

come_string_list_t come_string_regex_split(come_string_t a, const char* pattern, size_t n) {
    come_string_list_t list = COME_HDR_INIT(sctx(a));
    if (!pattern) return list;

    regex_t regex;
    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) { come_ERR_set(EINVAL, "invalid regular expression"); return list; }

    // Design 3 follow-up (redesign): scan a NUL-terminated
    // copy of the receiver (see str_nul_copy), so the
    // NUL-based regexec loop stops at the view's end.
    // Pieces are independent copies in the receiver's
    // context, so they outlive the copy.
    come_string_t tmp = str_nul_copy(a);
    const char* p = sdata(tmp);
    regmatch_t pmatch[1];
    size_t matches = 0;

    for (;;) {
        int hit = regexec(&regex, p, 1, pmatch, 0) == 0;
        come_string_t part;
        if (hit && (n == 0 || matches < n - 1) && pmatch[0].rm_eo > pmatch[0].rm_so) {
            part = come_string_new_len(sctx(a), p, (size_t)pmatch[0].rm_so);
            p += pmatch[0].rm_eo;
            matches++;
        } else if (hit && (n == 0 || matches < n - 1) && pmatch[0].rm_eo == 0) {
            // Empty match at the end: the remainder is the last part.
            part = come_string_new(sctx(a), p);
            come_array_append_g((come_hdr_t*)&list, sizeof(come_string_t), &part);
            break;
        } else {
            part = come_string_new(sctx(a), p);
            come_array_append_g((come_hdr_t*)&list, sizeof(come_string_t), &part);
            break;
        }
        come_array_append_g((come_hdr_t*)&list, sizeof(come_string_t), &part);
    }

    regfree(&regex);
    return list;
}

come_string_list_t come_string_regex_groups(come_string_t a, const char* pattern) {
    come_string_list_t list = COME_HDR_INIT(sctx(a));
    if (!pattern) return list;

    regex_t regex;
    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) { come_ERR_set(EINVAL, "invalid regular expression"); return list; }

    size_t nmatch = regex.re_nsub + 1; // 0 is full match, 1..n are groups
    regmatch_t* pmatch_vals = malloc(sizeof(regmatch_t) * nmatch);

    // Design 3 follow-up (redesign): match against a
    // NUL-terminated copy (see str_nul_copy); offsets
    // relative to it match offsets within the receiver.
    come_string_t tmp = str_nul_copy(a);
    if (regexec(&regex, sdata(tmp), nmatch, pmatch_vals, 0) == 0) {
        for (size_t i = 0; i < nmatch; i++) {
            come_string_t part;
            if (pmatch_vals[i].rm_so != -1) {
                size_t len = (size_t)(pmatch_vals[i].rm_eo - pmatch_vals[i].rm_so);
                part = come_string_new_len(sctx(a), sdata(tmp) + pmatch_vals[i].rm_so, len);
            } else {
                // Optional group not matched: an empty string.
                memset(&part, 0, sizeof(part));
                part.ctx = sctx(a);
            }
            come_array_append_g((come_hdr_t*)&list, sizeof(come_string_t), &part);
        }
    }

    free(pmatch_vals);
    regfree(&regex);
    return list;
}

come_string_t come_string_regex_replace(come_string_t a, const char* pattern, const char* repl, size_t count) {
    come_string_t empty = { NULL, 0, 0, sctx(a) };
    if (!pattern) return empty;

    regex_t regex;
    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) { come_ERR_set(EINVAL, "invalid regular expression"); return empty; }

    // Two passes: measure, then copy. Both scan a
    // NUL-terminated copy of the receiver (see
    // str_nul_copy), so NUL-based reads stop at the
    // view's end.
    size_t new_len = 0;
    come_string_t tmp = str_nul_copy(a);
    const char* p = sdata(tmp);
    size_t matches = 0;
    regmatch_t pmatch[1];
    size_t repl_len = strlen(repl);

    while (*p && (count == 0 || matches < count)) {
        if (regexec(&regex, p, 1, pmatch, 0) == 0) {
            new_len += (size_t)pmatch[0].rm_so; // Prefix
            new_len += repl_len;                // Replacement
            p += pmatch[0].rm_eo;
            matches++;
            if (pmatch[0].rm_eo == pmatch[0].rm_so) { // Empty match
                if (*p) { new_len++; p++; }
            }
        } else {
            new_len += strlen(p);
            break;
        }
    }
    if (matches == count && *p) new_len += strlen(p); // Remaining

    come_string_t res = come_string_new_len(a.ctx, NULL, new_len);

    p = sdata(tmp);
    char* dest = res.data;
    matches = 0;

    while (dest && *p && (count == 0 || matches < count)) {
        if (regexec(&regex, p, 1, pmatch, 0) == 0) {
            size_t prefix_len = (size_t)pmatch[0].rm_so;
            memcpy(dest, p, prefix_len);
            dest += prefix_len;
            memcpy(dest, repl, repl_len);
            dest += repl_len;
            p += pmatch[0].rm_eo;
            matches++;
            if (pmatch[0].rm_eo == pmatch[0].rm_so) {
                if (*p) { *dest++ = *p++; }
            }
        } else {
            strcpy(dest, p);
            dest += strlen(p);
            break;
        }
    }
    if (dest && matches == count && *p) strcpy(dest, p);

    regfree(&regex);
    return res;
}

uint32_t come_string_list_len(come_string_list_t list) {
    return list.count;
}

come_string_list_t come_string_list_from_argv(TALLOC_CTX* ctx, int argc, char* argv[]) {
    come_string_list_t list = COME_HDR_INIT(ctx);
    for (int i = 0; i < argc; i++) {
        come_string_t s = come_string_new(ctx, argv[i]);
        come_array_append_g((come_hdr_t*)&list, sizeof(come_string_t), &s);
    }
    return list;
}

// Formatting
come_string_t come_string_sprintf(TALLOC_CTX* ctx, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    come_string_t s = come_string_vsprintf(ctx, fmt, args);
    va_end(args);
    return s;
}

come_string_t come_string_vsprintf(TALLOC_CTX* ctx, const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int len = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    come_string_t empty = { NULL, 0, 0, ctx };
    if (len < 0) return empty;
    come_string_t s = come_string_new_len(ctx, NULL, (size_t)len);
    if (!s.data) return s;
    vsnprintf(s.data, (size_t)len + 1, fmt, args);
    return s;
}

int come_string_vsscanf(come_string_t a, const char* fmt, va_list args) {
    if (!fmt) return EOF;
    return vsscanf(sdata(a), fmt, args);
}

int come_string_sscanf(come_string_t a, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int r = come_string_vsscanf(a, fmt, args);
    va_end(args);
    return r;
}

come_byte_array_t come_string_byte_array(come_string_t a) {
    come_byte_array_t ba = COME_HDR_INIT(sctx(a));
    ba.data = (uint8_t*)mem_talloc_alloc(a.ctx, a.count ? a.count : 1);
    if (!ba.data) return ba;
    if (a.count) memcpy(ba.data, sdata(a), a.count);
    ba.count = a.count;
    ba.cap = a.count;
    return ba;
}

// Character access as a wchar (COIL-110, spec 6.5): s[i] is the
// i-th character decoded to its Unicode scalar value. An
// out-of-bounds index (including negative) sets ERR
// (errno = ERANGE) and yields 0, the zero wchar.
int32_t come_string_wchar_at_e(come_string_t a, long index) {
    if (index < 0) {
        come_ERR_set(ERANGE, "string index out of bounds");
        return 0;
    }
    const unsigned char* p = (const unsigned char*)sdata(a);
    const unsigned char* end_p_limit = (const unsigned char*)sdata(a) + a.count;
    long current = 0;
    while (p < end_p_limit) {
        uint32_t cp;
        int extra;
        if (*p < 0x80) { cp = *p; extra = 0; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; extra = 1; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; extra = 2; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; extra = 3; }
        else { cp = *p; extra = 0; } // invalid lead byte: yield it raw
        const unsigned char* q = p + 1;
        for (int i = 0; i < extra && q < end_p_limit && (*q & 0xC0) == 0x80; i++, q++)
            cp = (cp << 6) | (*q & 0x3F);
        if (current == index) return (int32_t)cp;
        p = q;
        current++;
    }
    come_ERR_set(ERANGE, "string index out of bounds");
    return 0;
}

// Element Access: the character at an index as a one-character
// string. An out-of-bounds index sets ERR (errno = ERANGE) and
// yields the zero value for a string - an empty string.
come_string_t come_string_at(come_string_t a, size_t index) {
    {
        // Design 3 follow-up (redesign): bounded by the
        // byte count (was a NUL walk, which read parent
        // characters for an out-of-range index on a view).
        const char* p = sdata(a);
        const char* end_p_limit = sdata(a) + a.count;
        size_t current_idx = 0;

        while (p < end_p_limit) {
            const char* start = p;
            do { p++; } while (p < end_p_limit && (*p & 0xC0) == 0x80);
            if (current_idx == index) {
                return come_string_new_len(a.ctx, start, (size_t)(p - start));
            }
            current_idx++;
        }
    }
    come_ERR_set(ERANGE, "string index out of bounds");
    come_string_t e = { NULL, 0, 0, sctx(a) };
    return e;
}

long come_string_tol(come_string_t a) {
    return come_string_tol_base(a, 10);
}

// tol with an explicit base (COIL-117): base 0 auto-detects
// (0x = hex, leading 0 = octal), 2..36 as strtoll. A string
// with no digits at all sets ERR (EINVAL); overflow sets
// ERANGE via strtoll. (The old tol never set ERR.)
long come_string_tol_base(come_string_t a, int base) {
    
    char* end = NULL;
    errno = 0;
    // Design 3 follow-up (redesign): parse a
    // NUL-terminated copy (see str_nul_copy), so
    // strtoll stops at the view's end instead of
    // reading digits from its parent.
    come_string_t tmp = str_nul_copy(a);
    long v = strtoll(sdata(tmp), &end, base);
    if (end == sdata(tmp)) come_ERR_set(EINVAL, "string is not a number");
    return v;
}

double come_string_tod(come_string_t a) {
    char* end = NULL;
    errno = 0;
    // Design 3 follow-up (redesign): see tol_base.
    come_string_t tmp = str_nul_copy(a);
    double v = strtod(sdata(tmp), &end);
    if (end == sdata(tmp)) come_ERR_set(EINVAL, "string is not a number");
    return v;
}

// Come_std.md section 5: atof/atoi/atol equivalents.
long come_string_toi(come_string_t a) {
    return come_string_tol_base(a, 10);
}

double come_string_tof(come_string_t a) {
    return come_string_tod(a);
}

// utf8(): is the string valid UTF-8? Rejects overlong forms,
// surrogates, and code points above U+10FFFF.
bool come_string_utf8(come_string_t a) {
    const unsigned char* p = (const unsigned char*)sdata(a);
    uint32_t remaining = a.count;
    while (remaining > 0) {
        unsigned char c = *p;
        uint32_t cp, min_cp;
        int extra;
        if (c < 0x80) { cp = c; extra = 0; min_cp = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; min_cp = 0x80; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; min_cp = 0x800; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; min_cp = 0x10000; }
        else return false;
        if (remaining < (uint32_t)(extra + 1)) return false;
        for (int i = 1; i <= extra; i++) {
            if ((p[i] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (uint32_t)(p[i] & 0x3F);
        }
        if (cp < min_cp) return false;                    // overlong
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;   // surrogate
        if (cp > 0x10FFFF) return false;
        p += extra + 1;
        remaining -= (uint32_t)(extra + 1);
    }
    return true;
}
