
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

// Simple Symbol Table for tracking local variable types
typedef struct {
    char name[64];
    char type[64];
} LocalVar;

static LocalVar local_vars[256];
static int local_var_count = 0;

static void reset_local_variables() {
    local_var_count = 0;
}

static void add_local_variable(const char* name, const char* type) {
    if (local_var_count < 256) {
        strncpy(local_vars[local_var_count].name, name, 63);
        strncpy(local_vars[local_var_count].type, type, 63);
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
