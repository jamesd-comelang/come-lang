#ifndef COME_STD_H
#define COME_STD_H

#include <stdio.h>
#include "come_string.h"

/* The std module's FILE object (Come_std.md section 2). The
 * definition is shared: the runtime (src/std/std.c) implements
 * the methods, generated code declares FILE values and passes
 * them to the runtime by address. */
struct come_std__FILE {
    FILE *fp;             /* libc FILE* */
    int   fd;             /* cached fd, -1 when not open */
    int   flags;          /* mode, ownership flags, etc */
    come_string_t* fname; /* file name, empty for std streams */
};

typedef struct come_std__FILE come_std__FILE_t;

#endif
