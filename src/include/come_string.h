#ifndef COME_STRING_MODULE_H
#define COME_STRING_MODULE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include "come_array.h"

// Forward declaration for talloc context
typedef void TALLOC_CTX;

// A string is a split header kept inline in the variable (COIL-135),
// with the same layout as every buffer kind: data is the heap byte
// block (context-owned, NUL-terminated), count is the byte length
// of the content (come_string_len counts characters), cap is the
// byte capacity of the block, ctx is the owner context used for
// derived strings. Strings are immutable: no operation grows the
// block in place; every transformation allocates a fresh block and
// returns a fresh header value. A zeroed header is an empty string.
typedef struct come_string_t {
    char* data;
    uint32_t count;
    uint32_t cap;
    void* ctx;
} come_string_t;

typedef come_string_t string;

// Constructors (return a fresh header value)
come_string_t come_string_new(TALLOC_CTX* ctx, const char* str);
come_string_t come_string_new_len(TALLOC_CTX* ctx, const char* str, size_t len);
come_string_t come_string_concat(TALLOC_CTX* ctx, come_string_t a, come_string_t b);
void come_string_free(come_string_t str);

// Core reads
uint32_t come_string_size(come_string_t a); // byte length of the content
uint32_t come_string_len(come_string_t a);  // character count
int come_string_cmp(come_string_t a, come_string_t b, size_t n); // n=0 for full
int come_string_casecmp(come_string_t a, come_string_t b, size_t n); // n=0 for full

// Search
long come_string_chr(come_string_t a, int c);
long come_string_rchr(come_string_t a, int c);
long come_string_memchr(come_string_t a, int c, size_t n);
long come_string_find(come_string_t a, const char* sub);
long come_string_rfind(come_string_t a, const char* sub);
uint32_t come_string_count(come_string_t a, const char* sub);

// Validation
bool come_string_isdigit(come_string_t a);
bool come_string_isalpha(come_string_t a);
bool come_string_isalnum(come_string_t a);
bool come_string_isspace(come_string_t a);
bool come_string_isascii(come_string_t a);

// Transformation (allocates a new string in the source's context)
come_string_t come_string_upper(come_string_t a);
come_string_t come_string_lower(come_string_t a);
come_string_t come_string_repeat(come_string_t a, size_t n);
come_string_t come_string_replace(come_string_t a, const char* old_str, const char* new_str, size_t n); // n=0 for all

// Trimming
come_string_t come_string_trim(come_string_t a, const char* cutset);
come_string_t come_string_ltrim(come_string_t a, const char* cutset);
come_string_t come_string_rtrim(come_string_t a, const char* cutset);

// Element access: the character at an index, as a one-char string or
// directly as a wchar (codegen uses the _e forms, which set ERR on
// an out-of-bounds index).
come_string_t come_string_at(come_string_t a, size_t index);
int32_t come_string_wchar_at_e(come_string_t a, long index);

// Splitting/Joining
come_string_list_t come_string_split(come_string_t a, const char* sep);
come_string_list_t come_string_split_n(come_string_t a, const char* sep, size_t n);
come_string_t come_string_join(come_string_list_t list, come_string_t sep);
uint32_t come_string_list_len(come_string_list_t list);
come_string_list_t come_string_list_from_argv(TALLOC_CTX* ctx, int argc, char* argv[]);

// Substring
come_string_t come_string_substr(come_string_t a, size_t start, size_t end);

// Regex
bool come_string_regex(come_string_t a, const char* pattern);
come_string_list_t come_string_regex_split(come_string_t a, const char* pattern, size_t n);
come_string_list_t come_string_regex_groups(come_string_t a, const char* pattern);
come_string_t come_string_regex_replace(come_string_t a, const char* pattern, const char* repl, size_t count);

// Sort a string list in place with a value comparator.
void come_string_list_sort(come_string_list_t* a, int (*cmp)(come_string_t, come_string_t));

// Ownership: re-parent the data block and adopt the new context;
// dup deep-copies the data block in the string's own context.
void come_string_chown(come_string_t* a, TALLOC_CTX* new_ctx);
come_string_t come_string_dup(come_string_t a);

// Formatting (format string is standard C format)
come_string_t come_string_sprintf(TALLOC_CTX* ctx, const char* fmt, ...);
come_string_t come_string_vsprintf(TALLOC_CTX* ctx, const char* fmt, va_list args);
int come_string_sscanf(come_string_t a, const char* fmt, ...);
int come_string_vsscanf(come_string_t a, const char* fmt, va_list args);

// Conversions
come_byte_array_t come_string_byte_array(come_string_t a);
long come_string_tol(come_string_t a);
long come_string_tol_base(come_string_t a, int base);
double come_string_tod(come_string_t a);
long come_string_toi(come_string_t a);
double come_string_tof(come_string_t a);
bool come_string_utf8(come_string_t a);
#endif // COME_STRING_MODULE_H
