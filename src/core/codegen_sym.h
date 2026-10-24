
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

// Simple Symbol Table for tracking local variable types
typedef struct {
    char name[64];
    char type[300]; // wide enough for fn: signatures (COIL-109)
    // The lexical scope depth the variable was declared at
    // (COIL-157): 0 is the function body, deeper blocks and
    // loop bodies count up. First match wins, like the type
    // lookup - and that is the safe direction for adoption:
    // an outer scope always outlives an inner one.
    int scope_depth;
} LocalVar;

static LocalVar local_vars[256];
static int local_var_count = 0;
// The depth add_local_variable records: codegen sets this
// to its current block depth before registering.
static int g_local_add_depth = 0;

static void reset_local_variables() {
    local_var_count = 0;
}

static void add_local_variable(const char* name, const char* type) {
    if (local_var_count < 256) {
        strncpy(local_vars[local_var_count].name, name, 63);
        strncpy(local_vars[local_var_count].type, type, sizeof(local_vars[local_var_count].type) - 1);
        local_vars[local_var_count].scope_depth = g_local_add_depth;
        local_var_count++;
    }
}

static const char* get_local_variable_type(const char* name) {
    for (int i = 0; i < local_var_count; i++) {
        if (strcmp(local_vars[i].name, name) == 0) {
            return local_vars[i].type;
        }
    }
    return NULL;
}

static int get_local_variable_scope(const char* name) {
    for (int i = 0; i < local_var_count; i++) {
        if (strcmp(local_vars[i].name, name) == 0) {
            return local_vars[i].scope_depth;
        }
    }
    return 0;
}

// Rewrite a name's recorded type (COIL-127: a `var` local
// locks to its inferred type at declaration/first assignment,
// spec 10.1). Returns 1 when the name was found.
static int update_local_variable_type(const char* name, const char* type) {
    for (int i = 0; i < local_var_count; i++) {
        if (strcmp(local_vars[i].name, name) == 0) {
            strncpy(local_vars[i].type, type, sizeof(local_vars[i].type) - 1);
            local_vars[i].type[sizeof(local_vars[i].type) - 1] = 0;
            return 1;
        }
    }
    return 0;
}
