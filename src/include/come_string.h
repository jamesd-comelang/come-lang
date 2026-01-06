#ifndef COME_STRING_MODULE_H
#define COME_STRING_MODULE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "come_array.h"

// Forward declaration for talloc context
typedef void TALLOC_CTX;



typedef struct come_string_t {
    uint32_t size;  // Total allocated capacity (bytes)
    uint32_t count; // Number of characters used
    char data[];    // Flexible array member
} come_string_t;

typedef come_string_t* string;

// Constructor/Destructor
come_string_t* come_string_new(TALLOC_CTX* ctx, const char* str);
come_string_t* come_string_new_len(TALLOC_CTX* ctx, const char* str, size_t len);
void come_string_free(come_string_t* str);

// Core Methods
uint32_t come_string_size(const come_string_t* a);
uint32_t come_string_len(const come_string_t* a);
int come_string_cmp(const come_string_t* a, const come_string_t* b, size_t n); // n=0 for full
int come_string_casecmp(const come_string_t* a, const come_string_t* b, size_t n); // n=0 for full

// Search
long come_string_chr(const come_string_t* a, int c);
long come_string_rchr(const come_string_t* a, int c);
long come_string_memchr(const come_string_t* a, int c, size_t n);
long come_string_find(const come_string_t* a, const char* sub);
long come_string_rfind(const come_string_t* a, const char* sub);
uint32_t come_string_count(const come_string_t* a, const char* sub);

// Validation
bool come_string_isdigit(const come_string_t* a);
bool come_string_isalpha(const come_string_t* a);
bool come_string_isalnum(const come_string_t* a);
bool come_string_isspace(const come_string_t* a);
bool come_string_isascii(const come_string_t* a);

// Transformation (allocates new string on parent context)
come_string_t* come_string_upper(const come_string_t* a);
come_string_t* come_string_lower(const come_string_t* a);
come_string_t* come_string_repeat(const come_string_t* a, size_t n);
come_string_t* come_string_replace(const come_string_t* a, const char* old_str, const char* new_str, size_t n); // n=0 for all

// Trimming
come_string_t* come_string_trim(const come_string_t* a, const char* cutset);
come_string_t* come_string_ltrim(const come_string_t* a, const char* cutset);
come_string_t* come_string_rtrim(const come_string_t* a, const char* cutset);

// Element Access
come_string_t* come_string_at(const come_string_t* a, size_t index);

// Splitting/Joining
// Note: These return arrays/lists, we'll define a simple list structure or use char** for now
// For MVP, we might skip complex list returns or define a simple string_list_t

come_string_list_t* come_string_split(const come_string_t* a, const char* sep);
come_string_list_t* come_string_split_n(const come_string_t* a, const char* sep, size_t n);
come_string_t* come_string_join(const come_string_list_t* list, const come_string_t* sep);
uint32_t come_string_list_len(const come_string_list_t* list);
come_string_list_t* come_string_list_from_argv(TALLOC_CTX* ctx, int argc, char* argv[]);

// Substring
come_string_t* come_string_substr(const come_string_t* a, size_t start, size_t end);

// Regex
bool come_string_regex(const come_string_t* a, const char* pattern);
come_string_list_t* come_string_regex_split(const come_string_t* a, const char* pattern, size_t n);
come_string_list_t* come_string_regex_groups(const come_string_t* a, const char* pattern);
come_string_t* come_string_regex_replace(const come_string_t* a, const char* pattern, const char* repl, size_t count);

// Memory Management
// Memory Management
void come_string_chown(come_string_t* a, TALLOC_CTX* new_ctx);

// Formatting
// format string is standard C format
come_string_t* come_string_sprintf(TALLOC_CTX* ctx, const char* fmt, ...);

// Conversions
come_byte_array_t* come_string_to_byte_array(const come_string_t* a);

long come_string_tol(const come_string_t* a);
#endif // COME_STRING_MODULE_H
