# Language Facilities Comparison (COME vs C vs Go vs Kotlin)

## 1. Basic Syntax

| Facility | **COME** | **C** | **Go** | **Kotlin** |
|---|---|---|---|---|
| **Entry Point** | `int main(void)` or `int main(int argc, char **argv)` | `int main(void)` or `int main(int argc, char **argv)` | `func main()` in `package main` | `fun main() { }` |
| **Modules / Packages** | None — split into `.h` and `.c` with manual linking | None — split into `.h` and `.c` with manual linking | `package` keyword, imports with `import` | `package` keyword, imports with `import` |
| **Header Files** | Required for declarations (`.h`) | Required for declarations (`.h`) | No — package imports handle declarations | No — package imports handle declarations |
| **Comments** | `//` and `/* ... */` | `//` and `/* ... */` | Same as C | `//` and `/* ... */` |
| **Semicolons** | Required after each statement | Required after each statement | Optional at line end | Optional at line end |
| **Case Sensitivity** | Yes | Yes | Yes | Yes |

## 2. Data Types

| Facility | **COME** | **C** | **Go** | **Kotlin** |
|---|---|---|---|---|
| **Primitive Types** | `char`, `int`, `short`, `long`, `float`, `double` | `char`, `int`, `short`, `long`, `float`, `double` | `bool`, `string`, `int`, `int8..int64`, `uint`, `uint8..uint64`, `float32`, `float64`, `complex64`, `complex128`, `byte`, `rune` | `Byte`, `Short`, `Int`, `Long`, `Float`, `Double`, `Boolean`, `Char` |
| **Strings** | Null-terminated char arrays | Null-terminated char arrays | Immutable UTF-8 strings | Immutable UTF-16 strings |
| **Booleans** | No dedicated type (use `_Bool` in C99) | No dedicated type (use `_Bool` in C99) | Built-in `bool` | Built-in `Boolean` |
| **Void** | `void` for no value or generic pointer | `void` for no value or generic pointer | No direct equivalent | `Unit` for no return value |
| **Structs** | Yes | Yes | Yes, with methods | Classes & data classes |
| **Arrays** | Fixed-size only | Fixed-size only | Fixed-size and slices | Fixed-size (`Array`) and dynamic (`List`, `MutableList`) |
| **Pointers** | Full pointer arithmetic | Full pointer arithmetic | Pointers allowed but no arithmetic | No raw pointers |
| **Enums** | Yes (`enum`) | Yes (`enum`) | No native enums | Full-featured enums with properties & methods |
| **Type Aliases** | `typedef` | `typedef` | `type` keyword | `typealias` keyword |
| **Generics** | None until C23 `_Generic` | None until C23 `_Generic` | Yes | Yes |
| **Complex Numbers** | C99 `complex` | C99 `complex` | Built-in `complex64` / `complex128` | No native support |

## 3. Variables & Constants

| Facility | **COME** | **C** | **Go** | **Kotlin** |
|---|---|---|---|---|
| **Declaration** | `int x = 5;` | `int x = 5;` | `var x int = 5` or `x := 5` | `var x: Int = 5` or `val x = 5` |
| **Constants** | `#define PI 3.14` or `const double PI = 3.14;` | `#define PI 3.14` or `const double PI = 3.14;` | `const Pi = 3.14` | `const val PI = 3.14` |
| **Scope** | Block, file (static), global | Block, file (static), global | Block, package, global | Block, file, package |
| **Mutable Variables** | Yes | Yes | Yes | `var` |
| **Immutable Variables** | No | No | Yes via `const` | Yes via `val` |

## 4. Operators

| Facility | **COME** | **C** | **Go** | **Kotlin** |
|---|---|---|---|---|
| **Arithmetic** | `+ - * / %` | `+ - * / %` | Same | Same |
| **Bitwise** | `& | ^ << >> ~` | `& | ^ << >> ~` | Same except `~` replaced by `^` | Same as C |
| **Logical** | `&& || !` | `&& || !` | Same | Same |
| **Pointer** | `*` dereference, `&` address-of | Same | Same | No pointers |
| **Increment / Decrement** | `++` and `--` | Same | Only as statements | `++` and `--` |
| **Ternary Operator** | `cond ? a : b` | Same | Not available | `if (...) a else b` |
| **Assignment Shorthand** | `+=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>=` | Same | Same | Same |

## 5. Control Flow

| Facility | **COME** | **C** | **Go** | **Kotlin** |
|---|---|---|---|---|
| **If / Else** | Yes | Yes | Yes | Yes |
| **Switch** | Fallthrough by default | Fallthrough by default | No fallthrough unless `fallthrough` keyword | `when` expression (no fallthrough) |
| **For Loop** | `for(init; cond; step)` | `for(init; cond; step)` | Single `for` form | `for (item in collection)` |
| **While Loop** | Yes | Yes | No | Yes |
| **Do-While Loop** | Yes | Yes | No | Yes |
| **Goto** | Yes | Yes | Yes | No |
| **Break / Continue** | Yes | Yes | Yes | Yes |
| **Labels** | Yes | Yes | Yes | Yes (`@label`) |

## 6. Functions & Procedures

| Facility | **COME** | **C** | **Go** | **Kotlin** |
|---|---|---|---|---|
| **Definition** | `int add(int a, int b) { return a+b; }` | Same | `func add(a int, b int) int { return a+b }` | `fun add(a: Int, b: Int): Int = a + b` |
| **Forward Declaration** | Needed | Needed | Not needed | Not needed |
| **Multiple Return Values** | No | No | Yes | Yes via `Pair`/`Triple`/data class |
| **Default Arguments** | No | No | No | Yes |
| **Named Parameters** | No | No | No | Yes |
| **Nested Functions** | No | No | Yes | Yes |
| **Variadic Functions** | Yes (`...`) | Yes (`...`) | Yes (`...`) | Yes (`vararg`) |
| **Recursion** | Yes | Yes | Yes | Yes |
| **Inline Functions** | Yes (`inline`) | Yes (`inline`) | Compiler decides | Yes (`inline`) |

## 7. Memory Management

| Facility | **COME** | **C** | **Go** | **Kotlin** |
|---|---|---|---|---|
| **Manual Allocation** | `malloc` / `free` | Same | No | No |
| **Stack Allocation** | Yes | Yes | Limited | Automatic |
| **Garbage Collection** | No | No | Yes | Yes |
| **Smart Pointers** | No | No | No | No (but automatic memory management) |
| **RAII** | No | No | No | Yes via `use` blocks |

## 8. Concurrency

| Facility | **COME** | **C** | **Go** | **Kotlin** |
|---|---|---|---|---|
| **Threads** | POSIX threads | POSIX threads | Goroutines | Threads, coroutines |
| **Channels / Queues** | Manual | Manual | Built-in channels | Channels via coroutines |
| **Async/Await** | No | No | No | Yes |
| **Mutex / Locks** | pthread mutex | pthread mutex | `sync.Mutex` | `ReentrantLock`, coroutines Mutex |

## 9. Error Handling

| Facility | **COME** | **C** | **Go** | **Kotlin** |
|---|---|---|---|---|
| **Return Codes** | Yes | Yes | Yes | Yes |
| **Errno** | Yes | Yes | Yes (`os.Err*`) | No |
| **Exceptions** | No | No | No | Yes (`try/catch/finally`) |
| **Defer Cleanup** | No | No | Yes (`defer`) | Yes (`use`) |

## 10. Object-Oriented Features

| Facility | **COME** | **C** | **Go** | **Kotlin** |
|---|---|---|---|---|
| **Classes** | No | No | No | Yes |
| **Interfaces** | No | No | Yes | Yes |
| **Methods** | No | No | Yes (receiver functions) | Yes |
| **Inheritance** | No | No | No | Yes |
| **Polymorphism** | No | No | Yes via interfaces | Yes |
| **Encapsulation** | Manual via struct visibility | Manual via struct visibility | Exported/unexported | `private`, `protected`, `public`, `internal` |

## 11. Tooling & Build

| Facility | **COME** | **C** | **Go** | **Kotlin** |
|---|---|---|---|---|
| **Compiler** | `comecc` (hypothetical) | `gcc`, `clang` | `go build` | `kotlinc` |
| **Package Manager** | None | None | Built-in `go get` | Gradle, Maven |
| **Testing Framework** | Manual | Manual | Built-in `go test` | JUnit, KotlinTest |
| **Cross Compilation** | Manual | Manual | Easy with `GOOS`/`GOARCH` | Limited, usually via JVM/Native |

