#ifndef CONFIG_H
#define CONFIG_H

#define _GNU_SOURCE 1

#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDINT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_BOOL 1
#define HAVE__BOOL 1
#define HAVE_STDBOOL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_VA_COPY 1
#define HAVE_INTPTR_T 1
#define HAVE_UINTPTR_T 1
#define HAVE_USECONDS_T 1
#define HAVE_DLFCN_H 1
#define HAVE_USLEEP 1
#define HAVE_DLSYM 1
#define HAVE_DLOPEN 1
#define HAVE_DLCLOSE 1
#define HAVE_DLERROR 1
#define HAVE_FPRINTF 1
#define HAVE_MEMMOVE 1
#define HAVE_STRNLEN 1
#define HAVE_VSNPRINTF 1
#define HAVE_C99_VSNPRINTF 1
#define HAVE_MEMSET_EXPLICIT 1
#define HAVE_VASPRINTF 1
#define HAVE_ASPRINTF 1
#define HAVE_SNPRINTF 1
#define HAVE_VSYSLOG 1
#define HAVE_STRERROR_R 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_TIME_H 1

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define memset_explicit(d,v,n) memset(d,v,n)

// Talloc version macros - let talloc.h define them or match exactly if needed for build
// Talloc build checks these against header values.
// We should probably just define the BUILD versions to match what talloc.h expects (2.4.0 based on error)
#define TALLOC_BUILD_VERSION_MAJOR 2
#define TALLOC_BUILD_VERSION_MINOR 4
#define TALLOC_BUILD_VERSION_RELEASE 0

#endif

