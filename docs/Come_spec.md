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

as forces system-module import.

Local modules cannot use as.

Module Resolution

import X is resolved in order:

./X.co
./X/*.co
./modules/X/*.co

System module depot

Found in (1–3): local module

Found in (4): system module

Local vs System Modules

Local modules

Have private memory contexts

May define:

void module_init()
void module_exit()


Lifecycle is managed by main

Init order follows import order

Exit order is reversed

System modules

API-only

No memory context

No lifecycle functions

Export

Symbols are private by default

Explicit export required:

export (PI, Point, add)

Namespaces

Access via:

module.symbol


No implicit imports into global scope

## 3.1 Importing Modules

Single module:

```come
import std
```

Multiple modules (single line):

```come
import (net, conv)
```

Multiple modules (multi-line):

```come
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
* `byte`, `ubyte`
* `short`, `ushort`
* `int`, `uint`
* `long`, `ulong`
* `float`, `double`
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
string name = "John" //Initially allocated on stack or heap with fixed size
string json //Initially empty string, default dynamic 
string filebuf="" //Initially empty string, default dynamic 
int arr[10]       // Initially allocated on stack or heap  with fixed size
int dyn[]         // Initially empty array, default dynamic 
```

**Dynamic Promotion**

A fixed-size string/array may be created on stack or heap initially, and later being promoted
to a fully dynamic array and relocated into the current memory arena when required.
Dynamic promotion occurs under the following conditions:
1. The array is assigned to another array variable.
2. The array is passed as a function argument.
3. The .resize(n) method is invoked.
4. The .chown() method is invoked.

Promotion is transparent to the programmer.
The compiler guarantees array validity across promotions.
Memory ownership follows the active arena and ownership rules.

# 7. Methods and Ownership

## 7.1 Built-in Composite Methods

All composite types support:

| Method | Description |
|---|---|
| `.length()` | Logical element count |
| `.size()` | Memory size in bytes |
| `.type()` | Runtime type name |
| `.owner()` | Current owner |
| `.chown()` | Transfer ownership |

## 7.2 Struct Methods

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
buf.alloc(512, dyn)
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

* `;` is optional
* Acts as a line separator
* Useful for multiple statements on one line

# 15. Design Summary

| Feature | Decision |
|---|---|
| Pointers | Removed |
| UTF-8 | Default |
| Switch | No fallthrough |
| Memory | Ownership-based |
| Arrays | Static + dynamic |
| Typing | Static with inference |
| Methods | Object-style |
| Compatibility | C mental model |

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
* **Managed**: `string`, `map`, array in T v[] format (Headered Buffer objects)
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
