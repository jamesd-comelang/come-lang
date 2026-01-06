# Concise COME Language Specification

## 1. Lexical Elements

* **Identifiers:** letters, digits, `_`, must not start with a digit
* **Keywords:** `break case const continue default do double else enum float for if int long return short struct switch union void  while bool`
* **Constants:** integer, floating, character, string
* **Operators:** arithmetic, relational, logical, bitwise, assignment, conditional
* **Separators:** `; , { } [ ] ( )`, `;` is optional at line end and may be used to separate multiple statements on the same line.
* **Comments:** `/* ... */`, `// ...`

---

## 2. Data Types

### Primitive Types

| Type | Alias | Size (Bytes) | Description |
| :--- | :--- | :--- | :--- |
| `bool` | N/A | 1 | Boolean true/false value. |
| `wchar` | N/A | 4 | A single **Unicode character**, declared with single quotes, e.g., `'A'`, `'🤔'`. |
| `byte` | `i8` | 1 | Signed 8-bit integer. |
| `ubyte` | `u8` | 1 | Unsigned 8-bit integer. |
| `short` | `i16` | 2 | Signed 16-bit integer. |
| `ushort` | `u16` | 2 | Unsigned 16-bit integer. |
| `int` | `i32` | 4 | Signed 32-bit integer. |
| `uint` | `u32` | 4 | Unsigned 32-bit integer. |
| `long` | `i64` | 8 | Signed 64-bit integer. |
| `ulong` | `u64` | 8 | Unsigned 64-bit integer. |
| `float` | `f32` | 4 | IEEE 754 Single-precision (32-bit) float. |
| `double` | `f64` | 8 | IEEE 754 Double-precision (64-bit) float. |
| `void` | N/A | 0 | Indicates no return value or a generic pointer. |

### Composite Types

Composite types are structures built from other types.
Their memory lifetime is managed by **explicit ownership**, tied to the parent composite variable or the containing module.

| Type      | Declaration Syntax    | Initialization Syntax                           | Description                                                        |
|-----------|------------------------|--------------------------------------------------|--------------------------------------------------------------------|
| `string` | `string name;`         | `string name = "John"`                           | Immutable, UTF-8 encoded sequence of characters.                   |
| `struct/union` | `MyStruct data;`       | `data = MyStruct{ field: value };`               | Custom aggregated data structure.                                  |
| `array`  | `T name[];`            | `int numbers[] = [10, 20, 30]`                   | Fixed-size collection. Size inferred from the initializer list `[]`. |
| `map`    | `map map_name{};`      | `map students = { "name" : "John", "age" : 16 }` | Unordered key-value collection using `{}` for initialization.       |
| `module` | *N/A*                  | *N/A*                                            | The top-level execution scope and lifetime container.               |

---

## 3. Declarations

```COME
int x
int a[10]
struct Point { int x, y; }
alias tcpport = ushort;
```

---

## 4. Expressions and Statements

### Statements

* Expression: `x = y + 1;`
* Compound: `{ stmt1; stmt2; }`
* Selection: `if`, `if-else`, `switch`
* Iteration: `for`, `while`, `do-while`
* Jump: `break`, `continue`, `return`, `goto`

---

## 5. Operators

### Operator Categories

| Category         | Operators                       |            |    |
| ---------------- | ------------------------------- | ---------- | -- |
| Arithmetic       | `+ - * / % ++ --`               |            |    |
| Relational       | `< <= > >= == !=`               |            |    |
| Logical          | `&&                             |            | !` |
| Bitwise          | `&                              | ^ ~ << >>` |    |
| Assignment       | `= += -= *= /= %= <<= >>= &= ^= | =`         |    |
| Conditional      | `?:`                            |            |    |
| Comma            | `,`                             |            |    |
| Pointer / Member | `* & -> .`                      |            |    |

---

## 6. Control Flow

```c
if (cond) { ... } else { ... }

switch (expr) {
  case 1: ...; break;
  default: ...;
}

for (init; cond; inc) { ... }
while (cond) { ... }
do { ... } while (cond);
```

---

## 7. Functions

```c
int add(int a, int b) {
    return a + b;
}
```

* Parameters passed by value
* Use pointers for reference-like behavior
* `void` for no return value

---

## 8. Storage Classes

| Keyword         | Meaning                               |
| --------------- | ------------------------------------- |
| `auto`          | default local                         |
| `register`      | request register storage              |
| `static`        | internal linkage / persistent storage |
| `extern`        | external linkage                      |
| `_Thread_local` | thread-local storage                  |

---

## 9. Type Qualifiers

| Qualifier  | Meaning                  |
| ---------- | ------------------------ |
| `const`    | read-only                |
| `volatile` | may change unexpectedly  |
| `restrict` | exclusive pointer access |

---

## 10. Preprocessor

```c
#define MAX 100
#include <stdio.h>
#if defined(DEBUG)
  ...
#endif
```

* Macro expansion is textual
* No type checking

---

## 11. Memory Model

* Address-of: `&x`
* Dereference: `*p`
* Pointer arithmetic allowed within same array
* Dynamic allocation: `malloc`, `calloc`, `realloc`, `free`

---

## 12. Standard

