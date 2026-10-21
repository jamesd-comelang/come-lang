#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <ctype.h>
#include <wchar.h>
#include "come_string.h"
#include "mem/talloc.h"
#ifdef _WIN32
#include <io.h>
#define come_fdopen _fdopen
#define come_fileno _fileno
#else
#include <unistd.h>
#define come_fdopen fdopen
#define come_fileno fileno
#endif

/* COME std module - FILE and related types */

// Forward declaration
struct come_string_t;

// FILE structure following COME naming convention
struct come_std__FILE {
    FILE *fp;        // libc FILE*
    int   fd;        // optional: cached fd
    int   flags;     // mode, ownership flags, etc
    come_string_t* fname;     // optional: filename
};

typedef struct come_std__FILE come_std__FILE_t;

struct come_std__Proc {
    int reserved;
};

typedef struct come_std__Proc come_std__Proc_t;

// ERR_t structure with preallocated 1024-byte buffer
struct come_std__ERR_t {
    int no;
    come_string_t* str;  // Points to the buffer below
    char buffer[1024];   // Preallocated buffer for error strings
};

typedef struct come_std__ERR_t come_std__ERR_t;

// Global ERR instance - exported as 'ERR' in std.co
come_std__ERR_t come_std__ERR;


// Pre-instantiated FILE objects: in, out, err
come_std__FILE_t std_in;
come_std__FILE_t std_out;
come_std__FILE_t std_err;
come_std__Proc_t std_proc;

// Helper to extract C string from come_string_t
static const char* come_string_to_cstr(come_string_t* s) {
    if (!s) return "(null)";
    // come_string_t has: uint32_t size, uint32_t count, char data[]
    // Offset past the header to get to the char array
    return (const char*)&s->data[0];
}

// Module Initialization
void come_std__init_local() {
    // Initialize FILE objects
    std_in.fp = stdin;
    std_in.fd = 0;
    std_out.fp = stdout;
    std_out.fd = 1;
    std_err.fp = stderr;
    std_err.fd = 2;
    
    // Initialize ERR object
    memset(&come_std__ERR, 0, sizeof(come_std__ERR));
    come_std__ERR.str = (come_string_t*)come_std__ERR.buffer;
    come_std__ERR.str->size = sizeof(come_std__ERR.buffer);
    come_std__ERR.str->count = 0;
    come_std__ERR.str->data[0] = '\0';
}

void come_std__exit_local() {
    // Cleanup if needed
}

// Deprecated, keep for now if needed by other modules
void come_std__FILE__init() {
    come_std__init_local();
}

void come_std__FILE__exit() {
    // cleanup
}

bool come_std__FILE__open(come_std__FILE_t* self, come_string_t* path, come_string_t* mode) {
    if (!self) return false;
    FILE* f = fopen(come_string_to_cstr(path), come_string_to_cstr(mode));
    if (!f) return false;
    if (self->fp && self->fp != stdin && self->fp != stdout && self->fp != stderr) fclose(self->fp);
    self->fp = f;
    self->fd = come_fileno(f);
    self->fname = come_string_new((void*)self, come_string_to_cstr(path));
    return true;
}

void come_std__FILE__close(come_std__FILE_t* self) {
    if (self && self->fp) {
        fclose(self->fp);
        self->fp = NULL;
    }
}

static int come_std_vfprintf_come(FILE* fp, const char* fmt, va_list ap) {
    if (!fp || !fmt) return -1;

    int total = 0;
    const char* p = fmt;

    while (*p) {
        if (*p != '%') {
            if (fputc((unsigned char)*p++, fp) == EOF) return -1;
            total++;
            continue;
        }

        char spec[128];
        int si = 0;
        spec[si++] = *p++;

        if (*p == '%') {
            if (fputc('%', fp) == EOF) return -1;
            p++;
            total++;
            continue;
        }

        while (*p && strchr("-+ #0", *p) && si < (int)sizeof(spec) - 2) spec[si++] = *p++;
        while (*p && isdigit((unsigned char)*p) && si < (int)sizeof(spec) - 2) spec[si++] = *p++;
        if (*p == '.' && si < (int)sizeof(spec) - 2) {
            spec[si++] = *p++;
            while (*p && isdigit((unsigned char)*p) && si < (int)sizeof(spec) - 2) spec[si++] = *p++;
        }

        char length[4] = "";
        int li = 0;
        while (*p && strchr("hljzL", *p) && li < 3 && si < (int)sizeof(spec) - 2) {
            length[li++] = *p;
            spec[si++] = *p++;
            if ((length[0] == 'h' || length[0] == 'l') && li == 1 && *p == length[0]) {
                length[li++] = *p;
                spec[si++] = *p++;
            }
            break;
        }
        length[li] = '\0';

        char conv = *p ? *p++ : '\0';
        if (!conv) return -1;

        if (conv == 't' || conv == 'T') {
            bool b = va_arg(ap, int) != 0;
            const char* s = b ? (conv == 't' ? "true" : "TRUE") : (conv == 't' ? "false" : "FALSE");
            int r = fputs(s, fp);
            if (r == EOF) return -1;
            total += (int)strlen(s);
            continue;
        }

        spec[si++] = conv;
        spec[si] = '\0';

        int written = 0;
        switch (conv) {
            case 's': {
                come_string_t* s = va_arg(ap, come_string_t*);
                written = fprintf(fp, "%s", come_string_to_cstr(s));
                break;
            }
            case 'd':
            case 'i':
                if (strcmp(length, "ll") == 0) written = fprintf(fp, spec, va_arg(ap, long long));
                else if (strcmp(length, "l") == 0) written = fprintf(fp, spec, va_arg(ap, long));
                else written = fprintf(fp, spec, va_arg(ap, int));
                break;
            case 'u':
            case 'o':
            case 'x':
            case 'X':
                if (strcmp(length, "ll") == 0) written = fprintf(fp, spec, va_arg(ap, unsigned long long));
                else if (strcmp(length, "l") == 0) written = fprintf(fp, spec, va_arg(ap, unsigned long));
                else written = fprintf(fp, spec, va_arg(ap, unsigned int));
                break;
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A':
                if (strcmp(length, "L") == 0) written = fprintf(fp, spec, va_arg(ap, long double));
                else written = fprintf(fp, spec, va_arg(ap, double));
                break;
            case 'c':
            case 'C':
                if (strcmp(length, "l") == 0 || conv == 'C') written = fprintf(fp, "%lc", (wint_t)va_arg(ap, int));
                else written = fprintf(fp, spec, va_arg(ap, int));
                break;
            case 'p':
                written = fprintf(fp, spec, va_arg(ap, void*));
                break;
            default:
                return -1;
        }

        if (written < 0) return -1;
        total += written;
    }

    return total;
}

int come_std__FILE__printf(come_std__FILE_t* self, come_string_t* fmt, ...) {
    if (!self || !self->fp || !fmt) return -1;
    va_list args;
    va_start(args, fmt);
    int ret = come_std_vfprintf_come(self->fp, fmt->data, args);
    va_end(args);
    return ret;
}

bool come_std__FILE__fdopen(come_std__FILE_t* self, int fd, come_string_t* mode) {
    if (!self) return false;
    FILE* f = come_fdopen(fd, come_string_to_cstr(mode));
    if (!f) return false;
    if (self->fp && self->fp != stdin && self->fp != stdout && self->fp != stderr) fclose(self->fp);
    self->fp = f;
    self->fd = fd;
    return true;
}

bool come_std__FILE__reopen(come_std__FILE_t* self, come_string_t* path, come_string_t* mode) {
    if (!self || !self->fp) return false;
    FILE* f = freopen(come_string_to_cstr(path), come_string_to_cstr(mode), self->fp);
    if (!f) return false;
    self->fp = f;
    self->fd = come_fileno(f);
    self->fname = come_string_new((void*)self, come_string_to_cstr(path));
    return true;
}

int come_std__FILE__fileno(come_std__FILE_t* self) {
    if (!self || !self->fp) return -1;
    return come_fileno(self->fp);
}

int come_std__FILE__scanf(come_std__FILE_t* self, come_string_t* fmt, ...) {
    if (!self || !self->fp || !fmt) return -1;
    va_list ap;
    va_start(ap, fmt);
    int ret = vfscanf(self->fp, fmt->data, ap);
    va_end(ap);
    return ret;
}

int come_std__FILE__vprintf(come_std__FILE_t* self, come_string_t* fmt, va_list ap) {
    if (!self || !self->fp || !fmt) return -1;
    return come_std_vfprintf_come(self->fp, fmt->data, ap);
}

int come_std__FILE__vscanf(come_std__FILE_t* self, come_string_t* fmt, va_list ap) {
    if (!self || !self->fp || !fmt) return -1;
    return vfscanf(self->fp, fmt->data, ap);
}

uint32_t come_std__FILE__read(come_std__FILE_t* self, come_byte_array_t* buf, uint32_t n) {
    if (!self || !self->fp || !buf) return 0;
    if (n > buf->size) n = buf->size;
    size_t got = fread(buf->items, 1, n, self->fp);
    if (got > buf->count) buf->count = (uint32_t)got;
    return (uint32_t)got;
}

uint32_t come_std__FILE__write(come_std__FILE_t* self, come_byte_array_t* buf, uint32_t n) {
    if (!self || !self->fp || !buf) return 0;
    if (n > buf->count) n = buf->count;
    return (uint32_t)fwrite(buf->items, 1, n, self->fp);
}

int32_t come_std__FILE__getc(come_std__FILE_t* self) {
    if (!self || !self->fp) return -1;
    return (int32_t)fgetc(self->fp);
}

void come_std__FILE__putc(come_std__FILE_t* self, int32_t c) {
    if (self && self->fp) fputc((int)c, self->fp);
}

come_string_t* come_std__FILE__gets(come_std__FILE_t* self) {
    if (!self || !self->fp) return NULL;
    char buf[4096];
    if (!fgets(buf, sizeof(buf), self->fp)) return NULL;
    return come_string_new((void*)self, buf);
}

uint32_t come_std__FILE__puts(come_std__FILE_t* self, come_string_t* s) {
    if (!self || !self->fp || !s) return 0;
    int ret = fputs(s->data, self->fp);
    if (ret < 0) return 0;
    if (fputc('\n', self->fp) == EOF) return 0;
    return s->count + 1;
}

come_string_t* come_std__FILE__fname(come_std__FILE_t* self) {
    if (!self || !self->fname) return come_string_new((void*)self, "");
    return self->fname;
}

void come_std__FILE__ungetc(come_std__FILE_t* self, int32_t c) {
    if (self && self->fp) ungetc((int)c, self->fp);
}

void come_std__FILE__seek(come_std__FILE_t* self, long offset, int whence) {
    if (self && self->fp) fseek(self->fp, offset, whence);
}

long come_std__FILE__tell(come_std__FILE_t* self) {
    if (!self || !self->fp) return -1;
    return ftell(self->fp);
}

void come_std__FILE__rewind(come_std__FILE_t* self) {
    if (self && self->fp) rewind(self->fp);
}
bool come_std__FILE__isopen(come_std__FILE_t* self) { return self && self->fp; }
bool come_std__FILE__eof(come_std__FILE_t* self) { return self && self->fp && feof(self->fp); }
bool come_std__FILE__error(come_std__FILE_t* self) { return self && self->fp && ferror(self->fp); }
void come_std__FILE__flush(come_std__FILE_t* self) { if (self && self->fp) fflush(self->fp); }
void come_std__FILE__clearerr(come_std__FILE_t* self) { if (self && self->fp) clearerr(self->fp); }
void come_std__FILE__setbuf(come_std__FILE_t* self, come_byte_array_t* buf, uint32_t size) {
    (void)size;
    if (self && self->fp) setbuf(self->fp, buf ? (char*)buf->items : NULL);
}
void come_std__FILE__setvbuf(come_std__FILE_t* self, come_byte_array_t* buf, int mode, uint32_t size) {
    if (self && self->fp) setvbuf(self->fp, buf ? (char*)buf->items : NULL, mode, size);
}
void come_std__FILE__setlinebuf(come_std__FILE_t* self) {
    if (self && self->fp) setvbuf(self->fp, NULL, _IOLBF, 0);
}

void come_std__Proc__abort(void* self) { (void)self; abort(); }
void come_std__Proc__exit(void* self, int status) { (void)self; exit(status); }
void come_std__Proc__atexit(void* self, void* cb) {
    (void)self;
    if (cb) atexit((void (*)(void))cb);
}
come_string_t* come_std__Proc__getenv(void* self, come_string_t* name) {
    (void)self;
    const char* val = getenv(come_string_to_cstr(name));
    return val ? come_string_new(NULL, val) : NULL;
}
int come_std__Proc__system(void* self, come_string_t* cmd) {
    (void)self;
    return system(come_string_to_cstr(cmd));
}

// Global file ops
bool come_std__remove(come_string_t* path) {
    return path && remove(path->data) == 0;
}

come_std__FILE_t* come_std__tmpfile(void) {
    FILE* fp = tmpfile();
    if (!fp) return NULL;
    come_std__FILE_t* f = mem_talloc_alloc(NULL, sizeof(come_std__FILE_t));
    if (!f) {
        fclose(fp);
        return NULL;
    }
    f->fp = fp;
    f->fd = come_fileno(fp);
    f->flags = 0;
    f->fname = NULL;
    return f;
}

come_string_t* come_std__tmpname(void) {
    char buf[L_tmpnam];
    if (!tmpnam(buf)) return NULL;
    return come_string_new(NULL, buf);
}

// ERR_t methods - following the name mangling convention: come_std__ERR_t__method
int come_std__ERR_t__no(come_std__ERR_t* self) {
    // Set ERR.no to errno and return it
    if (!self) self = &come_std__ERR;
    self->no = errno;
    return self->no;
}


come_string_t* come_std__ERR_t__str(come_std__ERR_t* self) {
    // Copy strerror into ERR.str buffer and return it
    if (!self) self = &come_std__ERR;

    
    const char* err_msg = strerror(errno);
    if (!err_msg) err_msg = "Unknown error";
    
    size_t len = strlen(err_msg);
    if (len >= sizeof(self->buffer) - sizeof(come_string_t)) {
        len = sizeof(self->buffer) - sizeof(come_string_t) - 1;
    }
    
    // Use the buffer as a come_string_t
    self->str = (come_string_t*)self->buffer;
    self->str->size = (uint32_t)(sizeof(self->buffer));
    self->str->count = (uint32_t)len;
    memcpy(self->str->data, err_msg, len);
    self->str->data[len] = '\0';
    
    return self->str;
}

void come_std__ERR_t__clear(come_std__ERR_t* self) {
    if (!self) self = &come_std__ERR;

    self->no = 0;
    errno = 0;
    if (self->str) {
        self->str->count = 0;
        self->str->data[0] = '\0';
    }
}

// Wrapper functions for global ERR object access
int come_ERR_no() {
    return come_std__ERR_t__no(&come_std__ERR);
}

come_string_t* come_ERR_str() {
    return come_std__ERR_t__str(&come_std__ERR);
}

void come_ERR_clear() {
    come_std__ERR_t__clear(&come_std__ERR);
}

