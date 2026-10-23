# Come Data Types

The Come language provides a set of fixed-width primitive types and composite types.

## Primitive Types

Come provides a set of fixed-width primitive types.

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
| `void` | N/A | 0 | Indicates no return value. |

### Numeric Literals
Numeric literals can use single quotes `'` as digit separators to improve readability.

```come
long big_num = 10'000'000'000L  // ' used as digit separator
```

## Composite Types

Composite types are structures built from other types.
Their memory lifetime is managed by **explicit ownership**: every buffer has an owner (a scope or a module), and buffers die deterministically with their owner. There is no manual allocation or freeing — `resize(n)` is the single primitive that changes a dynamic buffer's size.

| Type | Declaration Syntax | Initialization Syntax | Description |
|------|--------------------|-----------------------|-------------|
| `string` | `string name;` | `string name = "John"` | Immutable, UTF-8 encoded sequence of characters. Character-first: `s[i]` is a `wchar`, `.length()` counts characters, `.bytesize()` counts bytes. |
| `struct` / `union` | `Point p;` | `struct Point p = { .x = 5, .y = 10 }` | Custom aggregated data structure. Default layout is C-compatible; `packed struct` / `align(n) struct` adjust it. A struct is a value: assignment and pass-by-value copy the fields (buffer fields copy their headers, sharing data). A typed literal `struct Point{5, 10}` is a value in any expression position. |
| reference `&(struct T)` | `&(struct Vec3) r = &a` | `draw_line(&a, &a)` | A non-owning reference to a struct, declared on parameters and local variables; `&x` takes one at the call site, `.` accesses members through it. It does not own or extend the referent's lifetime. References to other types, reference fields, and reference returns do not exist (spec 6.2). |
| `array` | `int arr[10];` (fixed)<br>`int dyn[];` (dynamic) | `int numbers[] = {1, 2, 3}` | Fixed-size arrays are fixed; dynamic arrays grow/shrink via `resize(n)` only. Indexing is zero-based and bounds-checked. |
| `map` | `map{string, int} m;` | `map{string, int} m = {"a": 1, "b": 2}` | Unordered key-value collection. V1 key types: integer types, `bool`, `wchar`, `string` (hashed by contents), enums. |
| `module` | *N/A* | *N/A* | The top-level execution scope and lifetime container. |


## Built-in Methods for Composite Types

All composite variables automatically possess the following methods. These methods enable runtime introspection, memory size checks, and management of the object's memory lifetime via the ownership system.
 
| Method | Syntax | Return Type | Description |
| :--- | :--- | :--- | :--- |
| `type()` | `variable.type()` | `string` | Returns the **name of the composite variable's type** as a lowercase string (e.g., `"string"`, `"map"`, `"struct"`). |
| `length()` | `variable.length()` | `int` | Returns the **logical count** of elements/chars/pairs contained within the variable. |
| `bytesize()` | `variable.bytesize()` | `long` | Returns the size of the object's data in **bytes** (string content bytes; array elements × element size; excludes headers). Maps have no `bytesize()` — a map's byte size is an implementation detail. |
| `owner()` | `variable.owner()` | `composite variable` / `module` | Returns the **composite variable** or **module** that currently owns the object. |
| `chown()` | `variable.chown(new_owner)` | `void` | Explicitly transfers the object and its memory subtree to `new_owner` (a composite variable or a module name). |
| `dup()` | `variable.dup(depth)` | same type | Copies the object; `depth` counts freshly-copied levels: `.dup(0)` = new reference to the same buffer and owner; `.dup()` with no argument = full deep copy. |

Type-specific methods (array `.resize()`, string transforms, `link` channel ops, …) are documented with their types, not in this table.

## Dynamic Type Inference `var`

Come supports the dynamic placeholder type `var`, which allows variables to be declared without an explicit, concrete type.

### Declaration and Realization

The `var` keyword acts as a placeholder. The variable acquires and permanently locks its type based on the value of the **first assignment**. This is known as **type realization**.

#### Declaration

| Syntax | Description |
| :--- | :--- |
| `var x` | `x` is declared but has **no realized type** until the first assignment. |

#### Realization on First Assignment (Immediate)

If the variable is initialized immediately, its type is realized at the point of declaration.

| Syntax | Realized Type |
| :--- | :--- |
| `var ivar = 123` | `ivar` $\rightarrow$ `int` (`i32`) |
| `var svar = "hello"` | `svar` $\rightarrow$ `string` |
| `var bvar = true` | `bvar` $\rightarrow$ `bool` |

### Deferred Realization

If the variable is declared without an initial value, the type is realized upon the first explicit assignment.

```come
var lvar
// lvar currently has no concrete type
lvar = svar.dup()
// lvar type is now realized as string
```

### Rules of `var` Type Inference

The `var` keyword provides flexibility in declaration while preserving Come's nature as a **strongly and statically typed language**. The type is determined and locked at the first assignment.

| Rule | Description |
| :--- | :--- |
| **Type is Locked** | Once the variable receives its first assignment, its type is **permanently fixed**. |
| **Subsequent Assignments** | Any assignment after type realization **must match** the locked type. |
| **No Reassignment of Type** | Attempting to assign a value of a different type (e.g., assigning a `string` to a realized `int`) results in a **compile-time error**.  |
| **Full Type Support** | Type inference works for all primitive (`int`, `double`, `bool`, etc.) and composite types (`array`, `map`, `struct`). |

### Type Aliases in Come
```come
alias (i8 i16 i32 i64) = (byte short int long)
alias (u8 u16 u32 u64) = (ubyte ushort uint ulong)
```

### Examples

| Code | Realization | Error Check |
| :--- | :--- | :--- |
| `var x` <br> `x = 3.14` | `x` becomes `double` (`f64`) | `x = 10` (Allowed) <br> `x = "ten"` (Error) |
| `var y = {1, 2}` | `y` becomes `int[]` | `y = {3, 4, 5}` (Allowed) <br> `y = {"a", "b"}` (Error) |
| `var z` <br> `z = {"a": 1}` | `z` becomes `map{string, int}` | `z = {}` (Allowed) <br> `z = "map"` (Error) |

This behavior ensures that while declaration is flexible, Come remains a strongly and statically typed language throughout compilation.

### Usage Example

```come
int main() {
    byte b = 100
    ushort porta = 8080
    uint count = 1024
    long timestamp = 1678886400

    return 0
}
```


### Usage Example

```come
import std

int main(string args[]) {
    // Check array length
    if (args.length() > 0) {
        std.out.printf("First argument: %s\n", args[0])
    }

    // Array of integers
    int numbers[] = [1, 2, 3, 4, 5]
    std.out.printf("Count: %d\n", numbers.length())

    return 0
}
```
