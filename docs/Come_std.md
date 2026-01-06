 # Come std Module Specification

**Version:** 0.1

## Overview

The `std` module provides core I/O facilities in Come, replacing C's procedural stdio with object-based APIs. It emphasizes resource safety via ownership, deterministic cleanup, and compiler-enforced guarantees. No manual resource management required, but optional explicit methods available. Leaks are impossible due to auto-cleanup.

This spec focuses on file/stream I/O (`FILE`) as a complete replacement for C stdio. All character-related methods operate on `wchar` (wide char, Unicode-aware).

---

## 1. Design Principles

- **Object Exposure:** `std` exposes composite objects (e.g., `FILE`).
- **Ownership-Driven Lifetime:** Resources managed by scope/memory context.
- **Cleanup Options:** Auto via `.exit()`; optional manual `close()`.
- **Determinism:** All behavior compiler-driven; no GC, no undefined order.
- **C Compatibility:** Familiar signatures for migration, but safer (bounds-checked, type-enforced formats, Unicode via `wchar`).

---

## 2. FILE Type

### Overview

`FILE` is a object type representing a stream backed by an OS resource (file, socket, etc.).

- Memory owned by current context.
- OS resources acquired lazily during use.
- Auto-close in object `.exit()` or manual `close()`).

```come
FILE f;  // Declaration (uninitialized, underlying FD is set to -1)
```

### Lifecycle

| Method | Description |
|--------|-------------|
| `init()` | Internal: Initializes object (auto). |
| `exit()` | Auto: Releases OS resources on scope end (if not manually closed). |

`.exit()` invokes automatically at scope/memory context exit.

### Pre-instantiated Objects

These objects are pre-instantiated after `import std`. 

| Name | Description |
|------|-------------|
| `std.in` | Input stream (stdin). |
| `std.out` | Output stream (stdout). |
| `std.err` | Error stream (stderr). |

Example:

```come
std.out.printf("Hello\n");
std.err.printf("Error: %s\n", "Oops");
```

### Open/close Methods

Open underlying OS resource on a declared `FILE`. Return `bool` (success/failure).

| Method Signature | Description |
|------------------|-------------|
| `bool open(string path, string mode)` | Open file by path (modes: "r","w","a","r+","w+","a+"). |
| `void close()` | Manually close stream (optional; auto via `.exit()`). |
| `bool fdopen(int fd, string mode)` | Adopt existing FD. |
| `bool reopen(string ptah, string mode)` | close existing fd and reopen a new fd. |
| `int fileno()` | Return underlying fd. |

Example:

```come
FILE f
if (!f.open("data.txt", "r")) {
    // Handle error
}
```

### Methods
#### Formatted I/O

| Method | Description |
|--------|-------------|
| `int printf(string fmt, ...)` | Write formatted output to stream. |
| `int scanf(string fmt, ...)` | Read formatted input from stream. |
| `int vprintf(string fmt, va_list args)` | Write formatted output with va_list. |
| `int vscanf(string fmt, va_list args)` | Read formatted input with va_list. |

#### Reading/Writing

| Method | Description |
|--------|-------------|
| `long read(byte buf[], long n)` | Read up to n bytes into buf. Returns bytes read. |
| `long write(byte buf[], long n)` | Write n bytes from buf. Returns bytes written. |
| `int getc(void)` | Read one wchar (EOF as -1). |
| `void putc(wchar c)` | Write one wchar. |
| `string gets()` | Read line (incl. newline) as string. |
| `long puts(string s)` | Write string + newline. Returns chars written. |
| `string fname()` | Return the current file name. |
| `void ungetc(wchar c)` | Push back wchar. |

#### Positioning

| Method | Description |
|--------|-------------|
| `void seek(long offset, int whence)` | Reposition (whence: SEEK_SET=0, SEEK_CUR=1, SEEK_END=2). |
| `long tell(void)` | Get current offset. |
| `void rewind(void)` | Reset to start. |

#### Status & Control

| Method | Description |
|--------|-------------|
| `bool isopen()` | Check if stream is currently open. |
| `bool eof()` | End-of-file flag. |
| `bool error()` | Error flag. |
| `void flush()` | Flush buffers. |
| `void clearerr()` | Clear EOF/error flags. |
| `void setbuf(byte buf[], long size)` | Set buffer (unbuffered if NULL). |
| `void setvbuf(byte buf[], int mode, long size)` | Set buffering (modes: _IOFBF, _IOLBF, _IONBF). |
| `void setlinebuf()` | Line-buffered mode. |

### Full Example

```come
{
    FILE f;
    if (f.open("example.txt", "w")) {
        f.printf("Count: %d\n", 42);
        f.flush();
        f.close();  // Optional
    }
    f.seek(0, SEEK_SET);  // If reopened
    string line = f.gets();
    if (f.error()) {
        std.err.printf("Read failed: %s\n", ERR.str());
    }
}  // Auto .exit(): close if not manual
```

---

## 3. Error Handling

Use global `ERR` object for system errors (replaces `errno`/`strerror`).

| Method | Description |
|------------------|-------------|
| `int ERR.no()` | Get last error code. |
| `string ERR.str()` | Get error string. |

Example (replaces `perror`):

```come
if (ERR.no() != 0) {
    std.err.puts("Open failed: ");
    std.err.puts(ERR.str());
}
```

## 5. Process & Environment (`proc` Object)
The `std.proc` object provides methods to control the current process and interact with the host environment.

| Method | Description |
| :--- | :--- |
| **`void std.proc.abort()`** | [cite_start]Terminates execution abnormally[cite: 14]. |
| **`void std.proc.exit(int status)`** | [cite_start]Terminates program with status code[cite: 14, 16]. |
| **`void std.proc.atexit(method cb)`** | [cite_start]Registers a callback for program exit[cite: 19]. |
| **`string std.proc.getenv(string name)`** | [cite_start]Retrieves an environment variable[cite: 4, 12]. |
| **`int std.proc.system(string cmd)`** | [cite_start]Executes an external system command[cite: 12, 14]. |

---

## 4. Module methods

### File Operations

| Function Signature | Description |
|--------------------|-------------|
| `bool remove(string path)` | Remove file/directory entry. |
| `FILE tmpfile()` | Create and open temp FILE. |
| `string tmpname()` | Generate unique temp filename. |
| **`int rand()`** | [cite_start]Generates a pseudo-random number[cite: 16]. |
| **`void srand(uint seed)`** | [cite_start]Sets the `rand()` seed[cite: 16]. |
| **`qsort(byte arr[], method cmp)`** | [cite_start]Sorts an array using Quick Sort. |

Example:

```come
if (!std.remove("temp.txt")) {
    // Handle error
}
string name = std.tmpname();
FILE temp;
temp.open(name, "w");  // Or use tmpfile() method

FILE temp1 = std.tmpfile()
```
---

## 5. Obsoleted C stdio functions 

| C Function | Come Equivalent |
|------------|-----------------|
| `fgetpos` | Use `f.tell()` / `f.seek()` |
| `fsetpos` | Use `f.seek()` |
| `fpurge` | `f.flush()` |
| `getw` | Obsoleted |
| `putw` | Obsoleted |
| `mktemp` | `std.tmpname()` |
| `perror` | `std.err.puts(prefix); std.err.puts(ERR.str())` |
| `strerror` | `ERR.str()` |
| `sys_errlist` | Obsoleted |
| `sys_nerr` | Obsoleted |
| `malloc`   | Use `arr.resize(n)` |
| `calloc`   | Use `arr.resize(n)` |
| `realloc`  | Use `arr.resize(new_n)` |
| `free`     | Obsoleted |
| `memcpy`   | Use `arr.cpy(src)` |
| `memset`   | Use `arr.set(value)` |
---

## 5. Other C stdlib functions 

| C Function | Come Equivalent |
|------------|-----------------|
| `atof()` | `string.tof()` |
| `atoi()` | `string.toi()` |
| `atol()` | `string.toi()` |
| `atoll()` | `string.tol()` |
| `abs()` | `math.abs()` |
| `div()` | `math.div()` |
## 6. Ownership & Cleanup Rules

- `FILE` owned by: local scope and memory context.
- `.exit()` auto-called on end (closes fd if not manual).
- Manual `close()` optional;
- Temp files auto-removed on close/exit.

Example:

```come
{
    FILE f;
    f.open("temp.txt", "w");
    f.puts("Data");
    f.close();  // Optional
}  // f cleaned if not closed
```

---

# 16. Built-in Global Objects

Come provides a few built-in global objects that are available without explicit import.

## 16.1 ERR Object

The `ERR` object provides access to system error codes and messages, replacing C's `errno`, `strerror`, and `perror`.

| Method | Description |
|---|---|
| `ERR.no()` | Returns the last system error code (int). 0 means no error. |
| `ERR.str()` | Returns the error message string for the current error code. |

Example:

```come
if (ERR.no() != 0) {
    std.err.printf("Error: %s\n", ERR.str())
}
```
## 7. Guarantees

- **No Leaks:** Compiler ensures `.exit()` runs.
- **Deterministic Order:** Stack-based cleanup (LIFO).
- **No GC:** Explicit ownership.
- **Bounds Safety:** Auto-resize; type-checked formats; Unicode via `wchar`.
- **Thread-Safety:** Per-object locks (future).

---

## 8. Summary

`std` provides a complete, safe replacement for C stdio in Come: Objects over functions, Unicode-aware, optional manual control. Familiar for C devs, zero-cost abstractions. Integrates with ownership for leak-proof I/O.
