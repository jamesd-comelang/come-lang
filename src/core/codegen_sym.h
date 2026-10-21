
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

// Simple Symbol Table for tracking local variable types
typedef struct {
    char name[64];
    char type[128];
} LocalVar;

static LocalVar local_vars[256];
static int local_var_count = 0;

static void reset_local_variables() {
    local_var_count = 0;
}

static void add_local_variable(const char* name, const char* type) {
    for (int i = 0; i < local_var_count; i++) {
        if (strcmp(local_vars[i].name, name) == 0) {
            strncpy(local_vars[i].type, type, sizeof(local_vars[i].type) - 1);
            local_vars[i].type[sizeof(local_vars[i].type) - 1] = '\0';
            return;
        }
    }
    if (local_var_count < 256) {
        strncpy(local_vars[local_var_count].name, name, sizeof(local_vars[local_var_count].name) - 1);
        local_vars[local_var_count].name[sizeof(local_vars[local_var_count].name) - 1] = '\0';
        strncpy(local_vars[local_var_count].type, type, sizeof(local_vars[local_var_count].type) - 1);
        local_vars[local_var_count].type[sizeof(local_vars[local_var_count].type) - 1] = '\0';
        local_var_count++;
    }
}

static void update_local_variable_type(const char* name, const char* type) {
    add_local_variable(name, type);
}

static const char* get_local_variable_type(const char* name) {
    for (int i = 0; i < local_var_count; i++) {
        if (strcmp(local_vars[i].name, name) == 0) {
            return local_vars[i].type;
        }
    }
    return NULL;
}
