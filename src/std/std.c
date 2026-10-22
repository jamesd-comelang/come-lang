#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <wchar.h>
#include "come_string.h"
#include "come_array.h"
#include "come_std.h"

/* COME std module - FILE and related types (Come_std.md).
 * COIL-114: every FILE row of the Come_std tables is implemented
 * here for real; nothing in this file is a stub. The FILE struct
 * definition lives in come_std.h, shared with generated code. */

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

// Helper function to convert format string for COME types
// Converts %t/%T to %s (for bool) and %c to %lc (for wchar)
static char* come_convert_format(const char* fmt) {
    if (!fmt) return NULL;

    size_t len = strlen(fmt);
    char* new_fmt = (char*)malloc(len * 2 + 1); // Allocate extra space
    if (!new_fmt) return NULL;

    const char* src = fmt;
    char* dst = new_fmt;

    while (*src) {
        if (*src == '%') {
            *dst++ = *src++; // Copy '%'

            // Skip flags, width, precision
            while (*src && (strchr("-+ #0", *src) || isdigit(*src) || *src == '.')) {
                *dst++ = *src++;
            }

            // Check format specifier
            if (*src == 't' || *src == 'T') {
                // %t or %T for bool -> convert to %s
                *dst++ = 's';
                src++;
            } else if (*src == 'c') {
                // %c for wchar -> convert to %lc
                *dst++ = 'l';
                *dst++ = 'c';
                src++;
            } else if (*src) {
                // Copy other format specifiers as-is (including %s for come_string_t)
                *dst++ = *src++;
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    return new_fmt;
}

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
    std_in.fname = NULL;
    std_out.fp = stdout;
    std_out.fd = 1;
    std_out.fname = NULL;
    std_err.fp = stderr;
    std_err.fd = 2;
    std_err.fname = NULL;

    // Initialize ERR object
    memset(&come_std__ERR, 0, sizeof(come_std__ERR));
    come_std__ERR.str = (come_string_t*)come_std__ERR.buffer;
    come_std__ERR.str->size = sizeof(come_std__ERR.buffer);
    come_std__ERR.str->count = 0;
    come_std__ERR.str->data[0] = '\0';
}

void come_std__exit_local() {
    // The pre-instantiated streams belong to the process; the C
    // runtime flushes and closes them at exit.
}

// FILE.init(): (re)initialize one FILE object to the unopened
// state (underlying fd -1, per Come_std.md section 2).
void come_std__FILE__init(come_std__FILE_t* self) {
    if (!self) return;
    self->fp = NULL;
    self->fd = -1;
    self->flags = 0;
    self->fname = NULL;
}

// FILE.exit(): release the OS resource if still open. Called by
// the compiler at scope end; safe on an already-closed object.
void come_std__FILE__exit(come_std__FILE_t* self) {
    if (self && self->fp && self->fp != stdin && self->fp != stdout && self->fp != stderr) {
        fclose(self->fp);
    }
    if (self) {
        self->fp = NULL;
        self->fd = -1;
    }
}

bool come_std__FILE__open(come_std__FILE_t* self, come_string_t* path, come_string_t* mode) {
    if (!self || !path || !mode) return false;
    if (self->fp && self->fp != stdin && self->fp != stdout && self->fp != stderr) {
        fclose(self->fp);
        self->fp = NULL;
    }
    FILE* f = fopen(come_string_to_cstr(path), come_string_to_cstr(mode));
    if (!f) return false;   // errno from fopen: readable via ERR
    self->fp = f;
    self->fd = fileno(f);
    self->fname = come_string_new(NULL, come_string_to_cstr(path));
    return true;
}

void come_std__FILE__close(come_std__FILE_t* self) {
    if (self && self->fp && self->fp != stdin && self->fp != stdout && self->fp != stderr) {
        fclose(self->fp);
    }
    if (self) {
        self->fp = NULL;
        self->fd = -1;
    }
}

bool come_std__FILE__fdopen(come_std__FILE_t* self, int fd, come_string_t* mode) {
    if (!self || !mode || fd < 0) return false;
    FILE* f = fdopen(fd, come_string_to_cstr(mode));
    if (!f) return false;
    self->fp = f;
    self->fd = fd;
    self->fname = come_string_new(NULL, "");
    return true;
}

bool come_std__FILE__reopen(come_std__FILE_t* self, come_string_t* path, come_string_t* mode) {
    if (!self || !path || !mode) return false;
    FILE* f;
    if (self->fp) {
        f = freopen(come_string_to_cstr(path), come_string_to_cstr(mode), self->fp);
    } else {
        f = fopen(come_string_to_cstr(path), come_string_to_cstr(mode));
    }
    if (!f) {
        self->fp = NULL;
        self->fd = -1;
        return false;
    }
    self->fp = f;
    self->fd = fileno(f);
    self->fname = come_string_new(NULL, come_string_to_cstr(path));
    return true;
}

int come_std__FILE__fileno(come_std__FILE_t* self) {
    if (!self || !self->fp) return -1;
    return fileno(self->fp);
}

// Custom printf that handles COME types
// %s expects come_string_t* (converted to char*)
// %t/%T expects bool (converted to "true"/"false")
// %c expects wchar (converted to %lc)
int come_std__FILE__printf(come_std__FILE_t* self, come_string_t* fmt_s, ...) {
    if (!self || !self->fp || !fmt_s) return -1;
    const char* fmt = come_string_to_cstr(fmt_s);

    // Convert format string
    char* converted_fmt = come_convert_format(fmt);
    if (!converted_fmt) return -1;

    va_list args;
    va_start(args, fmt_s);

    // Build new argument list with converted values
    void* converted_args[16]; // Support up to 16 arguments
    int arg_idx = 0;

    const char* p = fmt;
    while (*p && arg_idx < 16) {
        if (*p == '%') {
            p++;
            if (*p == '%') { p++; continue; } // Skip %%

            // Skip flags, width, precision
            while (*p && (strchr("-+ #0", *p) || isdigit(*p) || *p == '.')) p++;

            if (*p == 't' || *p == 'T') {
                // bool -> "true" or "false"
                bool b = va_arg(args, int); // bool promoted to int
                converted_args[arg_idx++] = (void*)(b ? "true" : "false");
                p++;
            } else if (*p == 's') {
                // come_string_t* -> char*
                come_string_t* str = va_arg(args, come_string_t*);
                converted_args[arg_idx++] = (void*)come_string_to_cstr(str);
                p++;
            } else if (*p == 'd' || *p == 'i' || *p == 'u' || *p == 'x' || *p == 'X' || *p == 'o') {
                converted_args[arg_idx++] = (void*)(long)va_arg(args, int);
                p++;
            } else if (*p == 'l') {
                p++;
                if (*p == 'd' || *p == 'i' || *p == 'u' || *p == 'x' || *p == 'X' || *p == 'o') {
                    converted_args[arg_idx++] = (void*)va_arg(args, long);
                    p++;
                } else if (*p == 'c') {
                    // wchar (int32_t)
                    converted_args[arg_idx++] = (void*)(long)va_arg(args, int32_t);
                    p++;
                }
            } else if (*p == 'c') {
                // char (promoted to int, but we converted to %lc)
                converted_args[arg_idx++] = (void*)(long)va_arg(args, int);
                p++;
            } else if (*p == 'p') {
                converted_args[arg_idx++] = va_arg(args, void*);
                p++;
            } else if (*p) {
                // Other specifiers: pass the argument through as-is.
                converted_args[arg_idx++] = va_arg(args, void*);
                p++;
            }
        } else {
            p++;
        }
    }

    va_end(args);

    // Call fprintf with converted format and args
    int ret;
    switch (arg_idx) {
        case 0: ret = fprintf(self->fp, "%s", converted_fmt); break;
        case 1: ret = fprintf(self->fp, converted_fmt, converted_args[0]); break;
        case 2: ret = fprintf(self->fp, converted_fmt, converted_args[0], converted_args[1]); break;
        case 3: ret = fprintf(self->fp, converted_fmt, converted_args[0], converted_args[1], converted_args[2]); break;
        case 4: ret = fprintf(self->fp, converted_fmt, converted_args[0], converted_args[1], converted_args[2], converted_args[3]); break;
        case 5: ret = fprintf(self->fp, converted_fmt, converted_args[0], converted_args[1], converted_args[2], converted_args[3], converted_args[4]); break;
        case 6: ret = fprintf(self->fp, converted_fmt, converted_args[0], converted_args[1], converted_args[2], converted_args[3], converted_args[4], converted_args[5]); break;
        case 7: ret = fprintf(self->fp, converted_fmt, converted_args[0], converted_args[1], converted_args[2], converted_args[3], converted_args[4], converted_args[5], converted_args[6]); break;
        case 8: ret = fprintf(self->fp, converted_fmt, converted_args[0], converted_args[1], converted_args[2], converted_args[3], converted_args[4], converted_args[5], converted_args[6], converted_args[7]); break;
        default: ret = -1; // Too many arguments
    }

    free(converted_fmt);
    return ret;
}

int come_std__FILE__vprintf(come_std__FILE_t* self, come_string_t* fmt_s, va_list ap) {
    if (!self || !self->fp || !fmt_s) return -1;
    return vfprintf(self->fp, come_string_to_cstr(fmt_s), ap);
}

int come_std__FILE__scanf(come_std__FILE_t* self, come_string_t* fmt_s, ...) {
    if (!self || !self->fp || !fmt_s) return EOF;
    va_list ap;
    va_start(ap, fmt_s);
    int r = vfscanf(self->fp, come_string_to_cstr(fmt_s), ap);
    va_end(ap);
    return r;
}

int come_std__FILE__vscanf(come_std__FILE_t* self, come_string_t* fmt_s, va_list ap) {
    if (!self || !self->fp || !fmt_s) return EOF;
    return vfscanf(self->fp, come_string_to_cstr(fmt_s), ap);
}

long come_std__FILE__read(come_std__FILE_t* self, come_byte_array_t* buf, long n) {
    if (!self || !self->fp || !buf || n < 0) return -1;
    long want = n;
    if ((uint32_t)want > buf->count) want = buf->count;
    if (want == 0) return 0;
    return (long)fread(buf->items, 1, (size_t)want, self->fp);
}

long come_std__FILE__write(come_std__FILE_t* self, come_byte_array_t* buf, long n) {
    if (!self || !self->fp || !buf || n < 0) return -1;
    long want = n;
    if ((uint32_t)want > buf->count) want = buf->count;
    if (want == 0) return 0;
    return (long)fwrite(buf->items, 1, (size_t)want, self->fp);
}

// getc/putc/ungetc work on wchar (Unicode scalar) over a byte
// stream: the UTF-8 encoding is done here, not via fgetwc, whose
// behavior after byte I/O depends on the C locale.
int32_t come_std__FILE__getc(come_std__FILE_t* self) {
    if (!self || !self->fp) return -1;
    int b = fgetc(self->fp);
    if (b == EOF) return -1;
    if (b < 0x80) return b;
    int extra;
    uint32_t cp;
    if ((b & 0xE0) == 0xC0) { extra = 1; cp = b & 0x1F; }
    else if ((b & 0xF0) == 0xE0) { extra = 2; cp = b & 0x0F; }
    else if ((b & 0xF8) == 0xF0) { extra = 3; cp = b & 0x07; }
    else return b;   // invalid lead byte: hand it back as-is
    for (int i = 0; i < extra; i++) {
        int cb = fgetc(self->fp);
        if (cb == EOF || (cb & 0xC0) != 0x80) return b;
        cp = (cp << 6) | (uint32_t)(cb & 0x3F);
    }
    return (int32_t)cp;
}

void come_std__FILE__putc(come_std__FILE_t* self, int32_t c) {
    if (!self || !self->fp || c < 0) return;
    uint32_t cp = (uint32_t)c;
    if (cp < 0x80) {
        fputc((int)cp, self->fp);
    } else if (cp < 0x800) {
        fputc(0xC0 | (int)(cp >> 6), self->fp);
        fputc(0x80 | (int)(cp & 0x3F), self->fp);
    } else if (cp < 0x10000) {
        fputc(0xE0 | (int)(cp >> 12), self->fp);
        fputc(0x80 | (int)((cp >> 6) & 0x3F), self->fp);
        fputc(0x80 | (int)(cp & 0x3F), self->fp);
    } else {
        fputc(0xF0 | (int)(cp >> 18), self->fp);
        fputc(0x80 | (int)((cp >> 12) & 0x3F), self->fp);
        fputc(0x80 | (int)((cp >> 6) & 0x3F), self->fp);
        fputc(0x80 | (int)(cp & 0x3F), self->fp);
    }
}

come_string_t* come_std__FILE__gets(come_std__FILE_t* self) {
    if (!self || !self->fp) return come_string_new(NULL, "");
    size_t cap = 128, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) return come_string_new(NULL, "");
    // Read one line, including the newline; empty string at EOF.
    for (;;) {
        if (len + 64 > cap) {
            cap *= 2;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); return come_string_new(NULL, ""); }
            buf = nb;
        }
        if (!fgets(buf + len, (int)(cap - len), self->fp)) break;
        len += strlen(buf + len);
        if (len > 0 && buf[len - 1] == '\n') break;
        if (feof(self->fp)) break;
    }
    come_string_t* s = come_string_new_len(NULL, buf, len);
    free(buf);
    return s;
}

long come_std__FILE__puts(come_std__FILE_t* self, come_string_t* s) {
    if (!self || !self->fp || !s) return EOF;
    const char* text = come_string_to_cstr(s);
    if (fputs(text, self->fp) == EOF) return EOF;
    if (fputc('\n', self->fp) == EOF) return EOF;
    return (long)strlen(text) + 1;
}

come_string_t* come_std__FILE__fname(come_std__FILE_t* self) {
    if (!self || !self->fname) return come_string_new(NULL, "");
    return self->fname;
}

void come_std__FILE__ungetc(come_std__FILE_t* self, int32_t c) {
    if (!self || !self->fp || c < 0) return;
    // Push the character's UTF-8 bytes back, last byte first.
    uint32_t cp = (uint32_t)c;
    char bytes[4];
    int n = 0;
    if (cp < 0x80) {
        bytes[n++] = (char)cp;
    } else if (cp < 0x800) {
        bytes[n++] = (char)(0xC0 | (cp >> 6));
        bytes[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        bytes[n++] = (char)(0xE0 | (cp >> 12));
        bytes[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        bytes[n++] = (char)(0xF0 | (cp >> 18));
        bytes[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        bytes[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[n++] = (char)(0x80 | (cp & 0x3F));
    }
    for (int i = n - 1; i >= 0; i--) ungetc((unsigned char)bytes[i], self->fp);
}

void come_std__FILE__seek(come_std__FILE_t* self, long offset, int whence) {
    if (!self || !self->fp) return;
    fseeko(self->fp, (off_t)offset, whence);
}

long come_std__FILE__tell(come_std__FILE_t* self) {
    if (!self || !self->fp) return -1;
    return (long)ftello(self->fp);
}

void come_std__FILE__rewind(come_std__FILE_t* self) {
    if (!self || !self->fp) return;
    rewind(self->fp);
}

bool come_std__FILE__isopen(come_std__FILE_t* self) { return self && self->fp; }

bool come_std__FILE__eof(come_std__FILE_t* self) {
    return self && self->fp && feof(self->fp) != 0;
}

bool come_std__FILE__error(come_std__FILE_t* self) {
    return self && self->fp && ferror(self->fp) != 0;
}

void come_std__FILE__flush(come_std__FILE_t* self) {
    if (self && self->fp) fflush(self->fp);
}

void come_std__FILE__clearerr(come_std__FILE_t* self) {
    if (self && self->fp) clearerr(self->fp);
}

void come_std__FILE__setbuf(come_std__FILE_t* self, come_byte_array_t* buf, long size) {
    if (!self || !self->fp) return;
    (void)size;   // setbuf has no size parameter in C; see setvbuf
    if (buf && buf->count > 0) setbuf(self->fp, (char*)buf->items);
    else setbuf(self->fp, NULL);
}

void come_std__FILE__setvbuf(come_std__FILE_t* self, come_byte_array_t* buf, int mode, long size) {
    if (!self || !self->fp) return;
    char* b = (buf && buf->count > 0) ? (char*)buf->items : NULL;
    setvbuf(self->fp, b, mode, (size_t)(size > 0 ? size : 0));
}

void come_std__FILE__setlinebuf(come_std__FILE_t* self) {
    if (self && self->fp) setlinebuf(self->fp);
}

// Proc (Come_std.md section 5, COIL-115). The Proc object is a
// singleton, so the self pointer is unused. atexit defers to the
// C library: handlers registered later run earlier (reverse
// order), both on a normal return from main and on proc.exit.
void come_std__Proc__abort(void* self) { (void)self; abort(); }
void come_std__Proc__exit(void* self, int status) { (void)self; exit(status); }
void come_std__Proc__atexit(void* self, void (*cb)(void)) {
    (void)self;
    if (cb) atexit(cb);
}
come_string_t* come_std__Proc__getenv(void* self, come_string_t* name) {
    (void)self;
    if (!name) return come_string_new(NULL, "");
    const char* v = getenv(come_string_to_cstr(name));
    if (!v) {
        // Missing variable: empty string (never null) + ENOENT,
        // so ERR.set() reports it like other missing things.
        errno = ENOENT;
        return come_string_new(NULL, "");
    }
    return come_string_new(NULL, v);
}
int come_std__Proc__system(void* self, come_string_t* cmd) {
    (void)self;
    if (!cmd) return -1;
    return system(come_string_to_cstr(cmd));
}

// Module-level file operations (Come_std.md section 4)
bool come_std__remove(come_string_t* path) {
    if (!path) return false;
    return remove(come_string_to_cstr(path)) == 0;
}

come_std__FILE_t come_std__tmpfile(void) {
    come_std__FILE_t t;
    t.fp = tmpfile();   // auto-removed on close (C semantics)
    t.fd = t.fp ? fileno(t.fp) : -1;
    t.flags = 0;
    t.fname = come_string_new(NULL, "");
    return t;
}

come_string_t* come_std__tmpname(void) {
    static unsigned counter = 0;
    char buf[256];
    snprintf(buf, sizeof(buf), "/tmp/come-tmp-%ld-%u", (long)getpid(), ++counter);
    return come_string_new(NULL, buf);
}

int come_std__rand(void) { return rand(); }
void come_std__srand(unsigned int seed) { srand(seed); }

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

// ERR.fatal(msg): halt the program immediately, in every build.
// A plain runtime call - there is no build mode that removes it.
void come_ERR_fatal(come_string_t* msg) {
    fprintf(stderr, "Fatal error: %s\n", (msg && msg->data) ? msg->data : "unknown error");
    exit(1);
}
