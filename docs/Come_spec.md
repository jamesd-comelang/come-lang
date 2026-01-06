# 1. Overview

Come(C Object and Module Extensions) is a systems programming language inspired by C. It preserves C’s mental model while removing common pitfalls.

Key design goals:

* Familiar to C programmers
* UTF-8 native string model
* No raw pointers in user code
* Explicit ownership for composite types
* Safe-by-default control flow
* Minimal syntax extensions

# 2. Source File Structure

Each source file must declare exactly one module.
main is the program entry module.
```come
module main
```

## 2.1 Comments

Come supports both single-line and multi-line comments.

```come
// single-line comment

/*
   multi-line comment
*/
```

# 3. Modules and Imports

Imports expose symbols under a module namespace.
`as` forces system-module import.
Local modules cannot use `as`.

## Module Resolution

`import X` is resolved in order:
1. `./X.co`
2. `./X/*.co`
3. `./modules/X/*.co`
4. System module depot

Found in (1–3): local module.
Found in (4): system module.

## Local vs System Modules

### Local modules
* Have private memory contexts
* May define lifecycle functions (see [Module Lifecycle](#32-module-lifecycle-init-and-exit))
* Lifecycle is managed by `main`

### System modules
* API-only
* No memory context
* No lifecycle functions

## Export

Symbols are private to the current module by default. Public symbols must be explicitly exported:
```come
export (PI, Point, add)
```

## Namespaces

Access via:
```come
module.symbol
```

No implicit imports into global scope.

Aliases can be used to simplify symbol references:
```come
alias printf = std.out.printf
// printf("msg") is transparently rewritten to std.out.printf("msg")
```

## 3.1 Importing Modules

```come
import std
import (net, conv)
import (
    string,
    mem
)
```

Imported modules are referenced using dot notation:

```come
std.out.printf("hello")
net.hton(port)
```

## 3.2 Module Lifecycle: `.init()` and `.exit()`

In **Come**, every module follows a strictly deterministic lifecycle to ensure memory contexts are established and dependencies are resolved before execution begins.

---

### 1. The Initialization Protocol
The `.init()` method serves as the module's constructor. 

* **Automatic Invocation:** The compiler automatically triggers `.init()` for every imported module.
* **Import Ordering:** Initialization follows the exact sequence defined in the `import (...)` block.
* **Dependency Guarantee:** A module's dependencies are guaranteed to be fully initialized before its own `.init()` executes.

---

### 2. The Finalization Protocol
The `.exit()` method serves as the module's destructor.

* **Reverse Order:** To prevent dangling dependencies, modules are finalized in the **exact reverse order** of their initialization.
* **Context Cleanup:** This is the primary location for releasing memory anchored to the **Module Context** (e.g., globals initialized with `.size()`).

| Stage | Execution Order | Purpose |
| :--- | :--- | :--- |
| **`.init()`** | Top-to-Bottom | Setup memory headers, pre-allocate globals. |
| **`main()`** | Entry Point | Execute application logic. |
| **`.exit()`** | Bottom-to-Top | Close handles, free contexted memory. |

---

### 3. Design Pattern: The "Skeleton" Init
Because `.init()` is automatic, use it to establish a **Known Safe State** (Skeleton) while deferring heavy resource allocation to manual methods.

# 4. Constants and Enumerations

## 4.1 Constants

Single constant:

```come
const PI = 3.14
```

Grouped constants:

```come
const (
    a = 1,
    b = 2,
)
```

Trailing commas are allowed.

## 4.2 Enumerations

Come uses `enum` as a value generator within `const` blocks.

```come
const ( 
    RED = enum, 
    YELLOW, 
    GREEN, 
    UNKNOWN,
//Explicit starting value:
    HL_RED = enum(8),
    HL_YELLOW,
    HL_GREEN
)
```

* Enum values auto-increment
* Enum constants are integers

# 5. Aliases

The `alias` keyword provides C-like typedef and macro behavior.

## 5.1 Type Alias

```come
alias tcpport_t = ushort
alias Point = struct Point
```
## 5.2 Constant Alias (Define)

```come
alias MAX_ARRAY = 5
```

## 5.3 Macro Alias

```come
alias SQUARE(x) = ((x) * (x))
```

# 6. Types

## 6.1 Primitive Types

* `bool`
* `byte` (`i8`), `ubyte` (`u8`)
* `short` (`i16`), `ushort` (`u16`)
* `int` (`i32`), `uint` (`u32`)
* `long` (`i64`), `ulong` (`u64`)
* `float` (`f32`), `double` (`f64`)
* `wchar` (32-bit Unicode scalar)

Character literals:

```come
byte c = 'A'     // ASCII
wchar w = '🤔'   // Unicode
```

## 6.2 Composite Types

### 6.2.1 Struct

```come
struct Point {
    int x;
    int y;
};
```

Initialization:

```come
struct Point p = { .x = 5, .y = 10 }
```

### 6.2.2 Union

```come
union TwoBytes {
    short signed_s
    ushort unsigned_s
    byte first_byte
}
```

### 6.2.3 Strings and Arrays

String and arrays in Come are implemented as dynamic headered buffers.
`[ uint size_in_bytes | uint element_count | element data... ]`
size_in_bytes includes the header and payload.
element_count is the number of elements currently stored.
Elements are stored contiguously in memory.

```come
string name = "John"
string json
string filebuf = ""
int arr[10]
int dyn[]
```

### 6.2.4 Map

Maps are dynamic key-value associations.

```come
map m = {}
```

# 6.3 Dynamic Promotion

Composite type variables in Come are initially allocated in the most efficient storage available (stack or static data).
When a composite type variable requires dynamic behavior—such as being passed, resized, stored, or transferred—it is automatically promoted to a dynamic, headered buffer allocated in the current module memory context.
Promotion is deterministic and performed by the compiler; it is invisible to the programmer.

# 7. Methods and Ownership

## 7.1 Built-in Type Methods

All types support:

| Method | Description |
|---|---|
| `.type()` | Returns type name as a string (compile-time). The compiler tracks and lists all types used. |
| `.size()` | Memory size in bytes |

**Possible type name strings returned by `.type()`:**

Primitives: `"bool"`, `"byte"`, `"ubyte"`, `"short"`, `"ushort"`, `"int"`, `"uint"`, `"long"`, `"ulong"`, `"float"`, `"double"`, `"wchar"`, `"void"`

Managed types: `"string"`, `"map"`, `"array"`

Composite types: `"struct"`, `"union"`)

Composite types additionally support:

| Method | Description |
|---|---|
| `.length()` | Logical element count |
| `.owner()` | Current owner |
| `.chown()` | Transfer ownership |
| `.dup(depth)` | Creates a copy with specified recursion depth |

### 7.1.1 The `.dup()` Method

The `.dup()` method is a built-in for all composite types (strings, arrays, maps, structs, and unions) used to create copies of objects.

**Signature:** `obj.dup(ubyte depth)`

* **Argument:** Accepts a `ubyte` (0-255).
* **Default:** Calling `.dup()` without an argument defaults to `.dup(0)`.

#### Depth Logic

| Depth | Type | Description |
|---|---|---|
| `0` | **Deep Copy** | Performs a full recursive clone of the entire object tree (Infinite Depth). |
| `1` | **Shallow Copy** | Allocates a new top-level Headered Buffer, but all nested composite members remain as references (aliases) to the original children. |
| `n` | **N-Level Copy** | Recursively clones down to the specified level *n*. |

> [!NOTE]
> Maximum manual depth is 255. Depths beyond this are considered functionally equivalent to a deep copy (0) or are structurally impossible in standard memory contexts.

#### Memory & Ownership Integration

* **Allocation:** Every `.dup` call where $n > 0$ (or $n=0$) triggers a new memory allocation for the top-level object.
* **Ownership:** The new object is anchored to the **Local Scope** by default.
* **Identity:** While `b = a` creates an identity (`a == b`), `b = a.dup()` creates a new identity (`a != b`), even if the content is identical.


Structs may declare methods:

```come
struct TCP_ADDR {
    tcpport_t portnumber
    byte ipaddr[16]
}
```

Method implementation:

```come
byte TCP_ADDR.nport() {
    return net.hton(self.portnumber)
}
```

* `self` refers to the struct instance
* Methods are namespaced to the struct

# 8. Functions

## 8.1 Function Declaration

Prototype:

```come
int add(int a, int b)
```

Definition:

```come
int add(int a, int b) {
    return a + b
}
```

## 8.2 Multiple Return Values

Functions may return tuples:

```come
(int, string) add_n_compare(int a, int b) {
    return (a + b), (a > b) ? ">" : "<="
}
```

Call site:

```come
int sum
string cmp
(sum, cmp) = add_n_compare(i, s)
```

# 9. Variables and Type Inference

## 9.1 `var` Keyword

```come
var x
x = 123        // x becomes int
```

Immediate realization:

```come
var y = "hello"   // y becomes string
```

Rules:

* Type is locked at first assignment
* Cannot change type later
* Still statically typed

# 10. Control Flow

## 10.1 If / Else

```come
if (flag) {
    ...
} else {
    ...
}
```

## 10.2 Switch Statement

Come switch does NOT fall through by default.

```come
switch (value) {
    case RED:
        std.out.printf("Red\n")
    case GREEN:
        std.out.printf("Green\n")
    case UNKNOWN:
        fallthrough
    default:
        std.out.printf("UNKNOWN\n")
}
```

* `fallthrough` must be explicit
* `default` is optional

## 10.3 Loops

```come
for (int i = 0; i < 10; i++) { }

while (cond) { }

do { } while (cond)
```

# 11. Memory Management

## 11.1 Allocation

```come
int dyn[]
dyn.alloc(3)
```

## 11.2 Ownership Propagation

```come
byte buf[]
buf.resize(512)
buf.chown(dyn)
```

* `buf` is owned by `dyn`
* Freeing `dyn` frees all children

```come
dyn.free()
```

# 12. Expressions and Operators

Come supports:

* Arithmetic: `+` `-` `*` `/` `%`
* Bitwise: `&` `|` `^` `~` `<<` `>>`
* Logical: `&&` `||` `!`
* Relational: `==` `!=` `<` `>` `<=` `>=`

# 13. Exports

Symbols exported from a module:

```come
export (PI, Point, add)
```

* Only exported symbols are visible externally
* Unexported symbols remain module-private

# 14. Semicolons

* `;` is optional. Acts as a line separator. Useful for multiple statements on one line.

# 15. Design Summary

| Feature | Decision |
|---|---|
| Pointers | Removed |
| UTF-8 | Default |
| Switch | No fallthrough |
| Memory | Ownership-based |
| Strings, Arrays, Maps | Added |
| Typing | Static with inference |
| Methods | Object-style |
| Compatibility | C mental model |
| Module Lifecycle | Deterministic (.init/.exit) |

# 16. Come Language Keywords

Below is the definitive list of keywords currently utilized by the **Come** parser and lexer.

## 1. Program Structure & Scope
These keywords manage the modularity, visibility, and declaration patterns of the source code.

| Keyword | Description |
| :--- | :--- |
| `module` | Defines the current source file's namespace. |
| `main` | Entry point of the program. |
| `import` | Declaration for accessing external modules. |
| `export` | Marks variables, types, or functions as public. |
| `alias` | Unified syntax for type aliasing and macro defines. |
| `const` | Declares immutable values or enumerations. |
| `enum` | Declares incremental constant sets with `const`. |

## 2. Type System
Come uses a robust type system ranging from low-level primitives to high-level managed objects.

### Primitives & Objects
* **Special**: `void`, `bool`
* **Local inference**: `var` (Enables local type inference)
* **Managed**: `string`, `map`, array in T v[] format
* **Character**: `wchar` (Unicode support)

### Integer Types
| Signed | Unsigned | Alias |
| :--- | :--- | :--- |
| `byte` | `ubyte` | `i8` / `u8` |
| `short` | `ushort` | `i16` / `u16` |
| `int` | `uint` | `i32` / `u32` |
| `long` | `ulong` | `i64` / `u64` |

### Floating Point
* `float` (f32)
* `double` (f64)

### Composite & Behavioral Types
* `struct`: Composite data structure.
* `union`: Overlapping memory structure.

## 3. Control Flow
Standard procedural logic enhanced with explicit safety keywords.

### Selection
* `if`, `else`
* `switch`, `case`, `default`
* `fallthrough` (Explicit case bleeding)

### Iteration
* `for`
* `while`
* `do`
* `break`
* `continue`

### Subroutine
* `return` (Supports multiple return values)

## 4. Values
* `true`, `false`
* `null`

