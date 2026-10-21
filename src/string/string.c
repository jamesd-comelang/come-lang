#include "come_string.h"
#include "mem/talloc.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#ifndef _WIN32
#include <regex.h>
#endif


// Module Initialization
void come_string__init_local() {
    // String module initialization (currently none)
}

void come_string__exit_local() {
    // String module cleanup (currently none)
}

come_string_t* come_string_new(TALLOC_CTX* ctx, const char* str) {
    return come_string_new_len(ctx, str, strlen(str));
}

come_string_t* come_string_new_len(TALLOC_CTX* ctx, const char* str, size_t len) {
    // Allocate single block struct + data
    come_string_t* s = mem_talloc_alloc(ctx, sizeof(come_string_t) + len + 1);
    if (!s) return NULL;

    s->size = (uint32_t)(sizeof(come_string_t) + len + 1);
    s->count = (uint32_t)len;
    
    memcpy(s->data, str, len);
    s->data[len] = '\0';
    return s;
}

void come_string_free(come_string_t* str) {
    mem_talloc_free(str);
}

uint32_t come_string_size(const come_string_t* a) {
    return a ? a->count : 0;
}

// Basic UTF-8 char counting
uint32_t come_string_len(const come_string_t* a) {
    if (!a) return 0;
    uint32_t count = 0;
    const char* p = a->data;
    while (*p) {
        if ((*p & 0xC0) != 0x80) count++;
        p++;
    }
    return count;
}

static size_t come_utf8_char_width(const unsigned char* p) {
    if (!p || *p == 0) return 0;
    size_t width = 1;
    if ((*p & 0x80) == 0) width = 1;
    else if ((*p & 0xE0) == 0xC0) width = 2;
    else if ((*p & 0xF0) == 0xE0) width = 3;
    else if ((*p & 0xF8) == 0xF0) width = 4;

    size_t actual = 1;
    while (actual < width && p[actual] != 0) actual++;
    return actual;
}

static int come_utf8_compare_next(const char** a, const char** b, bool fold_ascii) {
    const unsigned char* p1 = (const unsigned char*)*a;
    const unsigned char* p2 = (const unsigned char*)*b;

    if (*p1 == 0 || *p2 == 0) {
        return (int)*p1 - (int)*p2;
    }

    size_t w1 = come_utf8_char_width(p1);
    size_t w2 = come_utf8_char_width(p2);

    if (fold_ascii && w1 == 1 && w2 == 1) {
        int c1 = tolower((unsigned char)*p1);
        int c2 = tolower((unsigned char)*p2);
        *a += 1;
        *b += 1;
        return c1 - c2;
    }

    size_t min = w1 < w2 ? w1 : w2;
    for (size_t i = 0; i < min; i++) {
        if (p1[i] != p2[i]) {
            *a += w1;
            *b += w2;
            return (int)p1[i] - (int)p2[i];
        }
    }

    *a += w1;
    *b += w2;
    return (int)w1 - (int)w2;
}

int come_string_cmp(const come_string_t* a, const come_string_t* b, size_t n) {
    if (!a || !b) return 0; // Safety
    if (n == 0) return strcmp(a->data, b->data);

    const char* p1 = a->data;
    const char* p2 = b->data;
    size_t chars = 0;

    while (chars < n) {
        if (!*p1 || !*p2) return (unsigned char)*p1 - (unsigned char)*p2;
        int diff = come_utf8_compare_next(&p1, &p2, false);
        if (diff != 0) return diff;
        chars++;
    }

    return 0;
}

int come_string_casecmp(const come_string_t* a, const come_string_t* b, size_t n) {
    if (!a || !b) return 0;

    const char* p1 = a->data;
    const char* p2 = b->data;
    size_t chars = 0;

    while (n == 0 || chars < n) {
        if (!*p1 || !*p2) return tolower((unsigned char)*p1) - tolower((unsigned char)*p2);
        int diff = come_utf8_compare_next(&p1, &p2, true);
        if (diff != 0) return diff;
        chars++;
    }

    return 0;
}

long come_string_chr(const come_string_t* a, int c) {
    char* p = strchr(a->data, c);
    return p ? (p - a->data) : -1;
}

long come_string_rchr(const come_string_t* a, int c) {
    char* p = strrchr(a->data, c);
    return p ? (p - a->data) : -1;
}

long come_string_memchr(const come_string_t* a, int c, size_t n) {
    void* p = memchr(a->data, c, n > a->count ? a->count : n);
    return p ? ((char*)p - a->data) : -1;
}

long come_string_find(const come_string_t* a, const char* sub) {
    char* p = strstr(a->data, sub);
    return p ? (p - a->data) : -1;
}

long come_string_rfind(const come_string_t* a, const char* sub) {
    // strrstr is not standard C, implement manually
    size_t sub_len = strlen(sub);
    if (sub_len > a->count) return -1;
    
    for (long i = a->count - sub_len; i >= 0; i--) {
        if (strncmp(a->data + i, sub, sub_len) == 0) return i;
    }
    return -1;
}

uint32_t come_string_count(const come_string_t* a, const char* sub) {
    uint32_t count = 0;
    const char* p = a->data;
    size_t sub_len = strlen(sub);
    if (sub_len == 0) return 0;
    
    while ((p = strstr(p, sub)) != NULL) {
        count++;
        p += sub_len;
    }
    return count;
}

// Validation
bool come_string_isdigit(const come_string_t* a) {
    for (size_t i = 0; i < a->count; i++) {
        if (!isdigit(a->data[i])) return false;
    }
    return true;
}

bool come_string_isalpha(const come_string_t* a) {
    for (size_t i = 0; i < a->count; i++) {
        if (!isalpha(a->data[i])) return false;
    }
    return true;
}

bool come_string_isalnum(const come_string_t* a) {
    for (size_t i = 0; i < a->count; i++) {
        if (!isalnum(a->data[i])) return false;
    }
    return true;
}

bool come_string_isspace(const come_string_t* a) {
    for (size_t i = 0; i < a->count; i++) {
        if (!isspace(a->data[i])) return false;
    }
    return true;
}

bool come_string_isascii(const come_string_t* a) {
    if (!a) return false;
    // Check if high bit is set
    const unsigned char* bytes = (const unsigned char*)a->data;
    while (*bytes) {
        if (*bytes & 0x80) return false;
        bytes++;
    }
    return true;
}

// Transformation
come_string_t* come_string_upper(const come_string_t* a) {
    come_string_t* new_str = come_string_new_len((void*)a, a->data, a->count);
    for (size_t i = 0; i < new_str->count; i++) {
        new_str->data[i] = toupper(new_str->data[i]);
    }
    return new_str;
}

come_string_t* come_string_lower(const come_string_t* a) {
    come_string_t* new_str = come_string_new_len((void*)a, a->data, a->count);
    for (size_t i = 0; i < new_str->count; i++) {
        new_str->data[i] = tolower(new_str->data[i]);
    }
    return new_str;
}

come_string_t* come_string_repeat(const come_string_t* a, size_t n) {
    size_t new_len = a->count * n;
    come_string_t* new_str = come_string_new_len((void*)a, "", new_len); // Alloc space
    // Manually fill
    for (size_t i = 0; i < n; i++) {
        memcpy(new_str->data + (i * a->count), a->data, a->count);
    }
    new_str->data[new_len] = '\0';
    return new_str;
}

come_string_t* come_string_replace(const come_string_t* a, const char* old_str, const char* new_str, size_t n) {
    // Simple implementation: count matches, alloc new string, copy
    size_t old_len = strlen(old_str);
    size_t new_len_part = strlen(new_str);
    
    if (old_len == 0) return come_string_new((void*)a, a->data); // No-op if old is empty
    
    // Count matches
    size_t count = 0;
    const char* p = a->data;
    while ((p = strstr(p, old_str)) != NULL) {
        count++;
        p += old_len;
        if (n > 0 && count >= n) break;
    }
    
    size_t final_len = a->count + count * (new_len_part - old_len);
    come_string_t* res = come_string_new_len((void*)a, "", final_len);
    
    p = a->data;
    char* dest = res->data;
    size_t matches = 0;
    while (*p) {
        char* next_match = strstr(p, old_str);
        if (next_match && (n == 0 || matches < n)) {
            size_t segment_len = next_match - p;
            memcpy(dest, p, segment_len);
            dest += segment_len;
            memcpy(dest, new_str, new_len_part);
            dest += new_len_part;
            p = next_match + old_len;
            matches++;
        } else {
            strcpy(dest, p);
            break;
        }
    }
    
    return res;
}

void come_string_chown(come_string_t* a, TALLOC_CTX* new_ctx) {
    if (a) {
        mem_talloc_steal(new_ctx, a);
    }
}

// Stub for other methods to allow compilation
// Helper for trim
static bool is_cutset(char c, const char* cutset) {
    if (!cutset) return isspace(c);
    return strchr(cutset, c) != NULL;
}

come_string_t* come_string_trim(const come_string_t* a, const char* cutset) {
    if (!a) return NULL;
    size_t start = 0;
    size_t end = a->count;

    while (start < end && is_cutset(a->data[start], cutset)) start++;
    while (end > start && is_cutset(a->data[end - 1], cutset)) end--;

    size_t new_len = end - start;
    come_string_t* new_str = come_string_new_len((void*)a, a->data + start, new_len);
    return new_str;
}

come_string_t* come_string_ltrim(const come_string_t* a, const char* cutset) {
    if (!a) return NULL;
    size_t start = 0;
    size_t end = a->count;

    while (start < end && is_cutset(a->data[start], cutset)) start++;

    size_t new_len = end - start;
    come_string_t* new_str = come_string_new_len((void*)a, a->data + start, new_len);
    return new_str;
}

come_string_t* come_string_rtrim(const come_string_t* a, const char* cutset) {
    if (!a) return NULL;
    size_t start = 0;
    size_t end = a->count;

    while (end > start && is_cutset(a->data[end - 1], cutset)) end--;

    size_t new_len = end - start;
    come_string_t* new_str = come_string_new_len((void*)a, a->data + start, new_len);
    return new_str;
}

come_string_list_t* come_string_split_n(const come_string_t* a, const char* sep, size_t n) {
    if (!a || !sep) return NULL;
    
    size_t sep_len = strlen(sep);
    if (sep_len == 0) {
        come_string_list_t* list = mem_talloc_alloc((void*)a, sizeof(come_string_list_t) + sizeof(come_string_t*));
        list->size = 1;
        list->count = 1;
        list->items[0] = come_string_new((void*)a, a->data);
        return list;
    }
    
    // First pass: count parts to alloc array
    size_t count = 1;
    const char* p = a->data;
    size_t matches = 0;
    while ((p = strstr(p, sep)) != NULL) {
        if (n > 0 && matches >= n - 1) break; // n parts means n-1 splits
        count++;
        matches++;
        p += sep_len;
    }

    // Allocate list struct with items FAM on 'a' context
    come_string_list_t* list = mem_talloc_alloc((void*)a, sizeof(come_string_list_t) + sizeof(come_string_t*) * count);
    list->size = count;
    list->count = count;

    // Second pass: fill
    p = a->data;
    matches = 0;
    for (size_t i = 0; i < count; i++) {
        const char* next = strstr(p, sep);
        size_t len;
        if (next && (n == 0 || matches < n - 1)) {
            len = next - p;
            list->items[i] = come_string_new_len(list, p, len);
            p = next + sep_len;
            matches++;
        } else {
            // Last part
            len = strlen(p);
            list->items[i] = come_string_new_len(list, p, len);
        }
    }

    return list;
}

come_string_list_t* come_string_split(const come_string_t* a, const char* sep) {
    return come_string_split_n(a, sep, 0);
}

come_string_t* come_string_join(const come_string_list_t* list, const come_string_t* sep) {
    if (!list || list->size == 0) return come_string_new_len(NULL, "", 0); // Context?
    // If list is empty, return empty string. Context? Maybe list itself?
    // If sep is NULL, assume empty separator.
    
    size_t sep_len = sep ? sep->count : 0;
    size_t total_len = 0;
    for (size_t i = 0; i < list->size; i++) {
        if (list->items[i]) total_len += list->items[i]->count;
        if (i < list->size - 1) total_len += sep_len;
    }

    // Allocate on list context? Or sep context? Or new?
    // Usually join creates a new string. Let's use list as parent.
    come_string_t* res = come_string_new_len((void*)list, "", total_len);
    
    char* p = res->data;
    for (size_t i = 0; i < list->size; i++) {
        if (list->items[i]) {
            memcpy(p, list->items[i]->data, list->items[i]->count);
            p += list->items[i]->count;
        }
        if (i < list->size - 1 && sep) {
            memcpy(p, sep->data, sep_len);
            p += sep_len;
        }
    }
    *p = '\0';
    return res;
}

come_string_t* come_string_substr(const come_string_t* a, size_t start, size_t end) {
    if (!a) return NULL;
    // start/end are character indices, not bytes!
    // Need to iterate UTF-8
    
    const char* p = a->data;
    size_t char_idx = 0;
    const char* start_p = NULL;
    const char* end_p = NULL;

    while (*p) {
        if (char_idx == start) start_p = p;
        if (char_idx == end) { end_p = p; break; }
        
        // Advance char
        do { p++; } while ((*p & 0xC0) == 0x80);
        char_idx++;
    }
    
    if (char_idx == start) start_p = p; // Start at end of string
    if (!end_p) end_p = p; // End beyond string

    if (!start_p) start_p = end_p; // Out of bounds
    if (start_p > end_p) start_p = end_p;

    size_t byte_len = end_p - start_p;
    return come_string_new_len((void*)a, start_p, byte_len);
}


// Regex
#ifndef _WIN32
bool come_string_regex(const come_string_t* a, const char* pattern) {
    if (!a || !pattern) return false;
    regex_t regex;
    if (regcomp(&regex, pattern, REG_EXTENDED | REG_NOSUB) != 0) return false;
    
    int ret = regexec(&regex, a->data, 0, NULL, 0);
    regfree(&regex);
    return ret == 0;
}

come_string_list_t* come_string_regex_split(const come_string_t* a, const char* pattern, size_t n) {
    if (!a || !pattern) return NULL;
    
    regex_t regex;
    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) return NULL;

    // First pass: count
    size_t count = 1;
    const char* p = a->data;
    regmatch_t pmatch[1];
    size_t matches = 0;
    
    while (regexec(&regex, p, 1, pmatch, 0) == 0) {
        if (n > 0 && matches >= n - 1) break;
        // Avoid infinite loop on empty match
        if (pmatch[0].rm_eo == pmatch[0].rm_so) {
             p++; // Advance one char if empty match
             if (*p == '\0') break;
             continue;
        }
        count++;
        matches++;
        p += pmatch[0].rm_eo;
    }
    
    come_string_list_t* list = mem_talloc_alloc((void*)a, sizeof(come_string_list_t) + sizeof(come_string_t*) * count);
    list->size = count;
    list->count = count;

    // Second pass: fill
    p = a->data;
    matches = 0;
    for (size_t i = 0; i < count; i++) {
        if (regexec(&regex, p, 1, pmatch, 0) == 0 && (n == 0 || matches < n - 1)) {
            size_t len = pmatch[0].rm_so;
            list->items[i] = come_string_new_len(list, p, len);
            p += pmatch[0].rm_eo;
            matches++;
        } else {
            size_t len = strlen(p);
            list->items[i] = come_string_new_len(list, p, len);
        }
    }

    regfree(&regex);
    return list;
}

come_string_list_t* come_string_regex_groups(const come_string_t* a, const char* pattern) {
    if (!a || !pattern) return NULL;
    
    regex_t regex;
    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) return NULL;

    size_t nmatch = regex.re_nsub + 1; // 0 is full match, 1..n are groups
    regmatch_t* pmatch_vals = malloc(sizeof(regmatch_t) * nmatch);
    
    if (regexec(&regex, a->data, nmatch, pmatch_vals, 0) == 0) {
        come_string_list_t* list = mem_talloc_alloc((void*)a, sizeof(come_string_list_t) + sizeof(come_string_t*) * nmatch);
        list->size = (uint32_t)nmatch;
        list->count = (uint32_t)nmatch;
        for (size_t i = 0; i < nmatch; i++) {
            if (pmatch_vals[i].rm_so != -1) {
                size_t len = pmatch_vals[i].rm_eo - pmatch_vals[i].rm_so;
                list->items[i] = come_string_new_len(list, a->data + pmatch_vals[i].rm_so, len);
            } else {
                list->items[i] = NULL; // Optional group not matched
            }
        }
        free(pmatch_vals);
        regfree(&regex);
        return list;
    }

    free(pmatch_vals);
    regfree(&regex);
    come_string_list_t* empty = mem_talloc_alloc((void*)a, sizeof(come_string_list_t));
    empty->size = 0;
    empty->count = 0;
    return empty;
}

come_string_t* come_string_regex_replace(const come_string_t* a, const char* pattern, const char* repl, size_t count) {
    if (!a || !pattern) return NULL;
    
    regex_t regex;
    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) return NULL;

    // We need to build the new string.
    // Since we don't know the size, we might need a dynamic buffer or two passes.
    // Two passes is easier.
    
    // Pass 1: calculate size
    size_t new_len = 0;
    const char* p = a->data;
    size_t matches = 0;
    regmatch_t pmatch[1];
    size_t repl_len = strlen(repl);
    
    while (*p && (count == 0 || matches < count)) {
        if (regexec(&regex, p, 1, pmatch, 0) == 0) {
            new_len += pmatch[0].rm_so; // Prefix
            new_len += repl_len;        // Replacement
            p += pmatch[0].rm_eo;
            matches++;
            if (pmatch[0].rm_eo == pmatch[0].rm_so) { // Empty match
                if (*p) { new_len++; p++; } // Advance one char
            }
        } else {
            new_len += strlen(p);
            break;
        }
    }
    if (matches == count && *p) new_len += strlen(p); // Remaining
    
    come_string_t* res = come_string_new_len((void*)a, "", new_len);
    
    // Pass 2: copy
    p = a->data;
    char* dest = res->data;
    matches = 0;
    
    while (*p && (count == 0 || matches < count)) {
        if (regexec(&regex, p, 1, pmatch, 0) == 0) {
            size_t prefix_len = pmatch[0].rm_so;
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
    if (matches == count && *p) strcpy(dest, p);
    
    regfree(&regex);
    return res;
}
#else
typedef struct {
    const char* start;
    const char* end;
} come_regex_match_t;

#define COME_REGEX_MAX_GROUPS 16

static const char* come_regex_pattern_start;

static const char* come_find_matching(const char* p, char open, char close) {
    int depth = 0;
    int escaped = 0;
    for (; *p; p++) {
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (*p == '\\') {
            escaped = 1;
            continue;
        }
        if (*p == open) depth++;
        else if (*p == close) {
            depth--;
            if (depth == 0) return p;
        }
    }
    return NULL;
}

static int come_group_index(const char* group_start) {
    int idx = 0;
    int escaped = 0;
    for (const char* p = come_regex_pattern_start; p < group_start; p++) {
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (*p == '\\') escaped = 1;
        else if (*p == '(') idx++;
    }
    return idx + 1;
}

static int come_match_escape(char esc, unsigned char c) {
    switch (esc) {
        case 'd': return isdigit(c);
        case 'D': return !isdigit(c);
        case 's': return isspace(c);
        case 'S': return !isspace(c);
        case 'w': return isalnum(c) || c == '_';
        case 'W': return !(isalnum(c) || c == '_');
        case 't': return c == '\t';
        case 'n': return c == '\n';
        case 'r': return c == '\r';
        default: return c == (unsigned char)esc;
    }
}

static int come_match_class(const char* start, const char* end, unsigned char c) {
    int negated = 0;
    int matched = 0;
    const char* p = start;
    if (p < end && (*p == '^' || *p == '!')) {
        negated = 1;
        p++;
    }

    while (p < end) {
        unsigned char first;
        if (*p == '\\' && p + 1 < end) {
            if (strchr("dDsSwWtnr", p[1])) {
                if (come_match_escape(p[1], c)) matched = 1;
                p += 2;
                continue;
            }
            first = (unsigned char)p[1];
            p += 2;
        } else {
            first = (unsigned char)*p++;
        }

        if (p + 1 < end && *p == '-') {
            unsigned char last = (unsigned char)p[1];
            if (last == '\\' && p + 2 < end) last = (unsigned char)p[2];
            if (first <= c && c <= last) matched = 1;
            p += (p[1] == '\\' && p + 2 < end) ? 3 : 2;
        } else if (c == first) {
            matched = 1;
        }
    }

    return negated ? !matched : matched;
}

static int come_regex_match_seq(const char* pat, const char* text, char stop,
                                come_regex_match_t* caps, const char** out_text);

static const char* come_regex_match_atom_once(const char* pat, const char* text,
                                              come_regex_match_t* caps,
                                              const char** atom_end) {
    if (!*text && *pat != '$') return NULL;

    if (*pat == '.') {
        *atom_end = pat + 1;
        return *text ? text + 1 : NULL;
    }

    if (*pat == '\\') {
        if (!pat[1]) return NULL;
        *atom_end = pat + 2;
        return (*text && come_match_escape(pat[1], (unsigned char)*text)) ? text + 1 : NULL;
    }

    if (*pat == '[') {
        const char* close = come_find_matching(pat, '[', ']');
        if (!close) return NULL;
        *atom_end = close + 1;
        return (*text && come_match_class(pat + 1, close, (unsigned char)*text)) ? text + 1 : NULL;
    }

    if (*pat == '(') {
        const char* close = come_find_matching(pat, '(', ')');
        const char* group_text_end = NULL;
        if (!close) return NULL;
        int group = come_group_index(pat);
        come_regex_match_t saved[COME_REGEX_MAX_GROUPS];
        memcpy(saved, caps, sizeof(saved));
        if (group < COME_REGEX_MAX_GROUPS) caps[group].start = text;
        if (!come_regex_match_seq(pat + 1, text, ')', caps, &group_text_end)) {
            memcpy(caps, saved, sizeof(saved));
            return NULL;
        }
        if (group < COME_REGEX_MAX_GROUPS) caps[group].end = group_text_end;
        *atom_end = close + 1;
        return group_text_end;
    }

    *atom_end = pat + 1;
    return (*text == *pat) ? text + 1 : NULL;
}

static const char* come_regex_read_quant(const char* pat, int* min, int* max) {
    *min = 1;
    *max = 1;
    if (*pat == '*') {
        *min = 0; *max = -1; return pat + 1;
    }
    if (*pat == '+') {
        *min = 1; *max = -1; return pat + 1;
    }
    if (*pat == '?') {
        *min = 0; *max = 1; return pat + 1;
    }
    if (*pat == '{') {
        const char* p = pat + 1;
        int a = 0;
        int b = -2;
        while (isdigit((unsigned char)*p)) {
            a = a * 10 + (*p - '0');
            p++;
        }
        if (*p == ',') {
            p++;
            if (*p == '}') {
                b = -1;
            } else {
                b = 0;
                while (isdigit((unsigned char)*p)) {
                    b = b * 10 + (*p - '0');
                    p++;
                }
            }
        } else {
            b = a;
        }
        if (*p == '}') {
            *min = a;
            *max = b;
            return p + 1;
        }
    }
    return pat;
}

static int come_regex_match_seq(const char* pat, const char* text, char stop,
                                come_regex_match_t* caps, const char** out_text) {
    if ((stop && *pat == stop) || (!stop && *pat == '\0')) {
        *out_text = text;
        return 1;
    }

    if (*pat == '$' && (!pat[1] || (stop && pat[1] == stop))) {
        if (*text == '\0') {
            *out_text = text;
            return 1;
        }
        return 0;
    }

    const char* atom_end = NULL;
    const char* first_end = come_regex_match_atom_once(pat, text, caps, &atom_end);
    if (!atom_end) return 0;
    int min = 1;
    int max = 1;
    const char* rest = come_regex_read_quant(atom_end, &min, &max);

    if (!first_end && min > 0) return 0;

    const char* positions[512];
    int count = 0;
    positions[count++] = text;
    const char* cur = text;
    come_regex_match_t base_caps[COME_REGEX_MAX_GROUPS];
    memcpy(base_caps, caps, sizeof(base_caps));

    while ((max < 0 || count <= max) && count < 512) {
        come_regex_match_t tmp_caps[COME_REGEX_MAX_GROUPS];
        memcpy(tmp_caps, caps, sizeof(tmp_caps));
        const char* next_atom_end = NULL;
        const char* next = come_regex_match_atom_once(pat, cur, tmp_caps, &next_atom_end);
        if (!next || next == cur) break;
        positions[count++] = next;
        cur = next;
        memcpy(caps, tmp_caps, sizeof(tmp_caps));
    }

    if (count - 1 < min) {
        memcpy(caps, base_caps, sizeof(base_caps));
        return 0;
    }

    for (int i = count - 1; i >= min; i--) {
        come_regex_match_t attempt[COME_REGEX_MAX_GROUPS];
        memcpy(attempt, caps, sizeof(attempt));
        const char* end_text = NULL;
        if (come_regex_match_seq(rest, positions[i], stop, attempt, &end_text)) {
            memcpy(caps, attempt, sizeof(attempt));
            *out_text = end_text;
            return 1;
        }
    }

    memcpy(caps, base_caps, sizeof(base_caps));
    return 0;
}

static int come_regex_group_count(const char* pattern) {
    int count = 0;
    int escaped = 0;
    for (const char* p = pattern; *p; p++) {
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (*p == '\\') escaped = 1;
        else if (*p == '(') count++;
    }
    return count;
}

static int come_regex_find(const char* text, const char* pattern, come_regex_match_t* caps) {
    if (!text || !pattern) return 0;
    come_regex_pattern_start = pattern;
    int anchored = pattern[0] == '^';
    const char* pat = anchored ? pattern + 1 : pattern;

    for (const char* p = text; ; p++) {
        memset(caps, 0, sizeof(come_regex_match_t) * COME_REGEX_MAX_GROUPS);
        const char* end = NULL;
        if (come_regex_match_seq(pat, p, '\0', caps, &end)) {
            caps[0].start = p;
            caps[0].end = end;
            return 1;
        }
        if (anchored || *p == '\0') break;
    }
    return 0;
}

bool come_string_regex(const come_string_t* a, const char* pattern) {
    if (!a || !pattern) return false;
    come_regex_match_t caps[COME_REGEX_MAX_GROUPS];
    return come_regex_find(a->data, pattern, caps) != 0;
}

come_string_list_t* come_string_regex_split(const come_string_t* a, const char* pattern, size_t n) {
    if (!a) return NULL;
    size_t count = 1;
    size_t matches = 0;
    const char* p = a->data;
    come_regex_match_t caps[COME_REGEX_MAX_GROUPS];
    while (pattern && come_regex_find(p, pattern, caps) && (n == 0 || matches < n - 1)) {
        if (caps[0].end == caps[0].start) break;
        count++;
        matches++;
        p = caps[0].end;
    }

    come_string_list_t* list = mem_talloc_alloc((void*)a, sizeof(come_string_list_t) + sizeof(come_string_t*) * count);
    if (!list) return NULL;
    list->size = (uint32_t)count;
    list->count = (uint32_t)count;

    p = a->data;
    matches = 0;
    for (size_t i = 0; i < count; i++) {
        if (pattern && come_regex_find(p, pattern, caps) && (n == 0 || matches < n - 1)) {
            list->items[i] = come_string_new_len(list, p, (size_t)(caps[0].start - p));
            p = caps[0].end;
            matches++;
        } else {
            list->items[i] = come_string_new_len(list, p, strlen(p));
        }
    }

    return list;
}

come_string_list_t* come_string_regex_groups(const come_string_t* a, const char* pattern) {
    if (!a) return NULL;
    come_regex_match_t caps[COME_REGEX_MAX_GROUPS];
    if (pattern && come_regex_find(a->data, pattern, caps)) {
        int groups = come_regex_group_count(pattern) + 1;
        if (groups > COME_REGEX_MAX_GROUPS) groups = COME_REGEX_MAX_GROUPS;
        come_string_list_t* list = mem_talloc_alloc((void*)a, sizeof(come_string_list_t) + sizeof(come_string_t*) * groups);
        if (!list) return NULL;
        list->size = (uint32_t)groups;
        list->count = (uint32_t)groups;
        for (int i = 0; i < groups; i++) {
            if (caps[i].start && caps[i].end >= caps[i].start) {
                list->items[i] = come_string_new_len(list, caps[i].start, (size_t)(caps[i].end - caps[i].start));
            } else {
                list->items[i] = NULL;
            }
        }
        return list;
    }

    come_string_list_t* empty = mem_talloc_alloc((void*)a, sizeof(come_string_list_t));
    if (!empty) return NULL;
    empty->size = 0;
    empty->count = 0;
    return empty;
}

come_string_t* come_string_regex_replace(const come_string_t* a, const char* pattern, const char* repl, size_t count) {
    if (!a) return NULL;
    if (!pattern || !repl) {
        return come_string_new_len((void*)a, a->data, strlen(a->data));
    }

    size_t repl_len = strlen(repl);
    size_t matches = 0;
    size_t new_len = 0;
    const char* p = a->data;
    come_regex_match_t caps[COME_REGEX_MAX_GROUPS];

    while (come_regex_find(p, pattern, caps) && (count == 0 || matches < count)) {
        if (caps[0].end == caps[0].start) break;
        new_len += (size_t)(caps[0].start - p) + repl_len;
        p = caps[0].end;
        matches++;
    }
    new_len += strlen(p);

    come_string_t* res = come_string_new_len((void*)a, "", new_len);
    if (!res) return NULL;
    char* out = res->data;
    p = a->data;
    matches = 0;
    while (come_regex_find(p, pattern, caps) && (count == 0 || matches < count)) {
        if (caps[0].end == caps[0].start) break;
        size_t prefix_len = (size_t)(caps[0].start - p);
        memcpy(out, p, prefix_len);
        out += prefix_len;
        memcpy(out, repl, repl_len);
        out += repl_len;
        p = caps[0].end;
        matches++;
    }
    strcpy(out, p);
    return res;
}
#endif

uint32_t come_string_list_len(const come_string_list_t* list) {
    if (!list) return 0;
    return list->count;
}

come_string_list_t* come_string_list_from_argv(TALLOC_CTX* ctx, int argc, char* argv[]) {
    come_string_list_t* list = mem_talloc_alloc(ctx, sizeof(come_string_list_t) + sizeof(come_string_t*) * argc);
    list->size = (uint32_t)argc;
    list->count = (uint32_t)argc;
    for (int i = 0; i < argc; i++) {
        list->items[i] = come_string_new(list, argv[i]);
    }
    return list;
}

// Formatting
#include <stdarg.h>

come_string_t* come_string_sprintf(TALLOC_CTX* ctx, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    // Calculate length
    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);
    
    if (len < 0) {
        va_end(args);
        return NULL;
    }
    
    come_string_t* s = come_string_new_len(ctx, "", len);
    if (!s) {
        va_end(args);
        return NULL;
    }
    
    vsnprintf(s->data, len + 1, fmt, args);
    va_end(args);
    
    return s;
}

come_byte_array_t* come_string_to_byte_array(const come_string_t* a) {
    if (!a) return NULL;
    
    // Allocate byte array structure with items FAM
    come_byte_array_t* ba = mem_talloc_alloc((void*)a, sizeof(come_byte_array_t) + a->count);
    if (!ba) return NULL;
    
    ba->size = a->count;
    ba->count = a->count;
    
    memcpy(ba->items, a->data, a->count);
    
    return ba;
}

// Element Access
come_string_t* come_string_at(const come_string_t* a, size_t index) {
    if (!a) return NULL;
    
    const char* p = a->data;
    size_t current_idx = 0;
    
    while (*p) {
        if (current_idx == index) {
            // Found the character start
            const char* start = p;
            // Advance to find end
            do { p++; } while ((*p & 0xC0) == 0x80);
            
            size_t len = p - start;
            return come_string_new_len((void*)a, start, len);
        }
        
        // Advance
        do { p++; } while ((*p & 0xC0) == 0x80);
        current_idx++;
    }
    
    return NULL; // Out of bounds
}

long come_string_tol(const come_string_t* a) {
    if (!a) return 0;
    return strtol(a->data, NULL, 10);
}
