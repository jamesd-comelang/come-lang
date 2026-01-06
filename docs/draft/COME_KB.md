# 📘 COME Language — Knowledge Base Summary

## 1. Overview

**COME (C Object and Module Extensions)** is a C-like programming language designed to:
- Retain **C syntax and semantics** while adding object and module features.
- Support **modules, coroutines, and event-driven I/O** (no threads).
- Enable **safe, modular memory management** using `talloc`.
- Compile efficiently to C with a one-to-one translation model.

The compiler can produce readable C code (`.co` → `.co.c` → native binary) if needed.

---

## 2. Core Language Design

### Syntax Highlights
- Optional semicolons (`;`).
- Object-like dot notation:  
  `obj.method(args)` → `type_method(obj, args)`
- Imports:  
  `import net.tcp` → include + namespace mapping.
- Variable declarations:  
  `net.tcp.Connection conn`
  → `net_tcp_Connection *conn` in C.
- String operations: built on UTF-8.

### Comparison
| COME | Equivalent C | Notes |
|------|---------------|-------|
| `a == b` | `strcmp(a, b) == 0` | Basic equality |
| `a.compare(b)` | `strcmp(a, b)` | Returns int |
| `a.equal(b, [ignore_case])` | `strcasecmp(a, b)` if ignore_case | Optional argument syntax |

---

## 3. Module System

COME organizes code into **modules**, similar to Go’s packages or Kotlin’s namespaces.

### Design
- Top-level module: `std`, `net`, `mem`, etc.
- Nested module via dots: `net.tcp`, `mem.talloc`, etc.
- Each module owns its **own memory context** and namespace.

### Recommended Directory Layout
```
src/
├── core/
├── net/
│   ├── tcp.c
│   ├── tcp.h
│   └── include/
├── mem/
│   ├── talloc.c
│   ├── talloc.h
│   └── include/
└── std/
    ├── string.c
    ├── string.h
    └── include/
```

Include flags:
```makefile
INCLUDES = -Isrc/include -Isrc/core/include -Isrc/net/include -Isrc/mem/include -Isrc/std/include
```

---

## 4. Network Module (`net.tcp`)

### Event Types
```c
enum {
    NET_TCP_EVENT_READABLE,
    NET_TCP_EVENT_WRITABLE,
    NET_TCP_EVENT_HUP,
    NET_TCP_EVENT_RDHUP,
    NET_TCP_EVENT_ERROR,
    NET_TCP_EVENT_ALL,
    NET_TCP_EVENT_NOTHING
};
```

### Core API

| COME Code | Translated C |
|------------|--------------|
| `net.tcp.Addr.make("127.0.0.1", 8080)` | `net_tcp_Addr_make("127.0.0.1", 8080)` |
| `conn = net.tcp.connect(addr)` | `net_tcp_connect(addr)` |
| `conn.on(READABLE) { ... }` | `net_tcp_on(conn, NET_TCP_EVENT_READABLE, callback)` |
| `conn.ignore(ALL)` | `net_tcp_ignore(conn, NET_TCP_EVENT_ALL)` |
| `conn.resume()` | `net_tcp_resume(conn)` |

### Example (COME)
```c
import std
import net.tcp

addr = net.tcp.Addr.make("127.0.0.1", 8080)
conn = net.tcp.connect(addr)

conn.on(READABLE) {
    std.printf("data ready!
")
}
```

### Translated (C)
```c
#include "std.h"
#include "net/tcp.h"

int main() {
    net_tcp_Addr* addr = net_tcp_Addr_make("127.0.0.1", 8080);
    net_tcp_Connection* conn = net_tcp_connect(addr);

    net_tcp_on(conn, NET_TCP_EVENT_READABLE, on_read);
}
```

---

## 5. Memory Management (`mem.talloc`)

COME uses **per-module memory contexts** via `talloc`.

### Design
- Each module owns a memory pool.
- Global memory root: `co_mem_root`
- Module context: `net_tcp_ctx`, `std_string_ctx`, etc.

### Example
```c
void* net_tcp_ctx;

void net_tcp_module_init(void) {
    net_tcp_ctx = talloc_new(co_mem_root);
}
```

### Benefits
- Automatic cleanup when a module unloads.
- Per-module isolation prevents leaks.
- Works naturally with hierarchical ownership (`talloc_parent()`).

---

## 6. Build System (Make)

### Root Makefile
- `src/Makefile` compiles the compiler.
- `examples/` and `tests/` built by root `Makefile`.

Example rules:
```makefile
SRC_DIR = src
BUILD_DIR = build
EXAMPLES_DIR = examples

TARGET = $(BUILD_DIR)/come
EXAMPLE_CO = $(wildcard $(EXAMPLES_DIR)/*.co)
EXAMPLE_BIN = $(EXAMPLE_CO:$(EXAMPLES_DIR)/%.co=$(EXAMPLES_DIR)/%)

all: $(TARGET)

$(TARGET):
    $(MAKE) -C $(SRC_DIR)

examples: $(EXAMPLE_BIN)

$(EXAMPLES_DIR)/%: $(EXAMPLES_DIR)/%.co $(TARGET)
    @echo "Building example $<"
    @$(TARGET) build $< -o $@
```

---

## 7. Runtime Initialization

Each module can register an initializer:
```c
void net_tcp_module_init(void);
void mem_talloc_module_init(void);

void co_runtime_init(void) {
    mem_talloc_module_init();
    net_tcp_module_init();
}
```

---

## 8. Coding Conventions

| Concept | Convention |
|----------|-------------|
| Module separator | `_` in C (`net.tcp` → `net_tcp_`) |
| Object vs method | Uppercase = type, lowercase = method |
| Namespace prefix | Required in all symbols |
| Imports | One per top-level domain |

---

## 9. Future Extensions

- Coroutines (Go-like `go func`).
- Event loop integration (epoll/kqueue).
- Async I/O for `net.tcp` and `fs`.
- Optional reference counting.
- Module auto-init registry.

---

## 10. Example Summary

```c
import net.tcp
import std

net.tcp.Addr addr = net.tcp.Addr.make("127.0.0.1", 8080)
conn = net.tcp.connect(addr)

conn.on(READABLE) {
    std.printf("Incoming data\n")
}

conn.ignore(ALL)
conn.resume()
```

→ Translates to idiomatic C using:
- namespaced structs (`net_tcp_Connection`)
- event loop + talloc context
- modular memory and I/O separation
