# The Come Language Specification

**Draft v2 — 2026-10-19**
Status: draft for founder review. Supersedes `docs/Come_spec.md` v1.

## Changes in this draft

Decisions walked through with the founder (2026-10-19), all applied here:

- **Entry point:** 4 accepted signatures — `int main()`, `int main(string args[])`,
  `void main()`, `void main(string args[])`. `args` is C-style: `args[0]`
  is the program name, `args[1]` the first argument. `void` form implies
  exit code 0.
- **Modules:** corrected — a module is all files in *one directory*
  declaring the same module name (a single `X.co`, or every `.co` file in
  `X/`); having both is a compile error.
- **Errors:** the global `ERR` object (`ERR.no()` / `ERR.str()` /
  `ERR.set()`, see `Come_std.md`) is the error mechanism. No
  `(value, err)` tuples. No C-style `assert`. New: `ERR.fatal(msg)` —
  always-active halt for unrecoverable states.
- **Memory:** no `.alloc()`, no `.free()`. `resize(n)` is the single primitive;
  `resize(0)` releases storage. Buffers die with their owner (scope / module
  exit). `resize(0)` never yields null — the header survives, so empty is a
  valid state. New rule: a reference may not outlive its buffer's owner.
- **`.dup()` depth:** the number is literal — `.dup(0)` copies only the
  reference (same buffer, same owner), `.dup(1)` fresh top level, `.dup()`
  with no argument is a full deep copy.
- **Maps:** statically typed as `map{K, V}`, e.g. `map{string, int} m`.
  Literals use braces + `:` pairs: `{"a": 1}` (matches existing docs).
  Access is `m["key"]`. V1 key types: integer types, `bool`, `wchar`,
  `string` (hashed by contents), and enums. `float`/`double` keys are
  excluded (NaN breaks the hash contract); struct keys are deferred.
- **Strings:** character-first. `s[i]` returns a `wchar`, `.length()` counts
  characters, `.size()` counts bytes. Strings are immutable; methods return
  new strings. `+` concatenates.
- **Coroutines (new in v1):** `coroutine name(args) { }` declaration,
  `spawn name(args)` launch. No module-variable access inside; argument
  buffers change owner to the coroutine — data races impossible by
  construction. `colink()` returns `(a, b)`; each `link` reads and writes
  both ways; `write` moves buffer ownership; `read` pauses until data;
  empty read means the peer closed. `wait(...)` pauses until any of several
  links is ready (epoll-style, without the weight). `yield` pauses
  cooperatively.
- **FFI:** `extern "C"` may appear only in system modules.
- **Functions as arguments:** v1 allows functions only as arguments, written
  in function-declaration shape (`void sort(int data[], int compare(int a, int b))`).
  No `fn` type, no function variables/fields/returns in v1.
- **Numbers:** no implicit narrowing, ever. Explicit cast `T(value)` to go
  smaller; implicit widening only when lossless.
- **Interfaces:** not in v1.
- **Layout:** default struct layout is C-compatible (needed for FFI).
  `packed struct` removes padding; `align(n) struct` sets alignment.
  No `@` sigil.
- **Built-in methods:** universal are `.type()`, `.size()`, `.length()`,
  `.owner()`, `.chown()`, `.dup()`. `.alloc()` / `.resize()` / `.free()` are
  type-specific (array) — `.alloc()` and `.free()` removed per above.
- **`null` removed** from the language (no `.co` example uses it; the
  design has no nulls). The C backend may still emit `NULL` internally.
- **`export`:** the two duplicate sections merged into one.
- **Lifecycle hooks:** `module_init()` / `module_exit()` (matches the
  implementation, which already synthesizes `module_init`).
- **Method syntax:** `Type.name()` with implicit `self` (the `method`
  keyword is dropped).
- **String length:** `.length()` everywhere (string docs' `.len()` renamed,
  the demo's `s.len()` updated to match).
- **Array literals:** `{1, 2, 3}` (braces = values, per the one-bracket-one-job
  rule; the types doc's `[1, 2, 3]` should be updated).
- **Struct init:** `{ .x = 5, .y = 10 }` (C designated form; the types doc's
  `MyStruct{ field: value }` should be updated).
- **`void`** means "no value" only (the "generic pointer" meaning is gone).
- **Missing map key:** read returns the zero value for `V` and sets `ERR`.

Follow-up doc work (not language changes): `docs/draft/` folder, EBNF
grammar rewrite, `Come_std.md` updates (`ERR.fatal`, `ERR.set()`,
`.len()` → `.length()`, `method` parameter type → declarator shape),
`Come_types.md` updates (array literals `{1, 2, 3}`, struct init
`{ .x = 5 }`, map `:` literals).

Post-v1: interfaces, `fn` types, struct map keys, `wait()` timeouts.

---

# 1. Overview

Come (C Object and Module Extensions) is a systems programming language
inspired by C. It preserves C's mental model while removing common pitfalls.

Key design goals:

- Familiar to C programmers
- No raw pointers in user code
- No manual memory management — ownership is explicit and hierarchical
- UTF-8 native string model, character-first
- Safe-by-default control flow
- Race-free coroutines with ownership-based isolation
- Static typing with local inference (`var`)
- Minimal syntax drift from C

The design consistently favors:

- **Predictability over magic**
- **Compile-time guarantees**
- **Explicitness in memory and visibility**
- **C mental model without C footguns**

# 2. Source File Structure

Each source file declares exactly one module, and `main` is the program
entry module:

```come
module main
```

## 2.1 Entry point

The program starts by running `main`. Four signatures are accepted:

```come
int main()
int main(string args[])
void main()
void main(string args[])
```

- `args` holds the command-line arguments, C-style: `args[0]` is the
  program name, `args[1]` is the first argument.
- Returning an `int` sets the process exit code; the `void` forms imply 0.

## 2.2 Comments

```come
// single-line comment

/*
   multi-line comment
*/
```

# 3. Modules and Imports

Each file declares exactly one module. A module may span multiple files:
all `.co` files in the same directory declaring the same module name
together form that module — either a single `X.co` file, or every `.co`
file in directory `X/`. Having both is a compile error. Modules are the
only compilation unit — there are no headers.

## 3.1 Importing modules

```come
import std

import (net, conv)

import (
    string,
    mem
)
```

Imported modules are referenced with dot notation; nothing is imported
into the global scope:

```come
std.out.printf("hello")
net.hton(port)
```

Aliases can shorten references:

```come
alias printf = std.out.printf
// printf("msg") is transparently rewritten to std.out.printf("msg")
```

## 3.2 Module resolution

`import X` is resolved in order:

1. `./X.co`
2. `./X/*.co`
3. `./modules/X/*.co`
4. System module depot

Found in (1–3): local module. Found in (4): system module.

`as` forces system-module import (skips local resolution).
Local modules cannot use `as`.

## 3.3 Local vs system modules

**Local modules**

- Have private memory contexts
- May define lifecycle functions (see 3.5)
- Lifecycle is managed by `main`

**System modules**

- API-only
- No memory context
- No lifecycle functions
- The only place `extern "C"` declarations may appear (see 13)

This is a clean separation between *code* and *runtime authority*.

## 3.4 Export

Symbols are private to the current module by default. Public symbols must
be explicitly exported. Only exported symbols are visible externally:

```come
export (PI, Point, add)
```

## 3.5 Module lifecycle: `module_init()` and `module_exit()`

Every local module follows a deterministic lifecycle so memory contexts are
established and dependencies resolved before execution begins.

```come
module net

void module_init() {
    // runs once, before main(); establish a known safe state
}

void module_exit() {
    // runs once, after main(); release module-context resources
}
```

Rules:

- **Automatic invocation:** the compiler calls `module_init()` for every
  imported local module; the programmer never calls it directly.
- **Import ordering:** initialization follows the exact sequence of the
  `import (...)` block.
- **Dependency guarantee:** a module's dependencies are fully initialized
  before its own `module_init()` runs.
- **Finalization is reverse:** `module_exit()` runs in the exact reverse
  order of initialization, so no module outlives its dependencies.

| Stage | Order | Purpose |
| :--- | :--- | :--- |
| `module_init()` | Top-to-bottom (import order) | Set up memory contexts, known safe state |
| `main()` | Entry point | Application logic |
| `module_exit()` | Bottom-to-top | Release module-context resources |

Design pattern: because `module_init()` is automatic, use it to establish a
**known safe state** and defer heavy resource allocation to explicit calls.

# 4. Constants and Enumerations

## 4.1 Constants

```come
const PI = 3.14

const (
    a = 1,
    b = 2,
)
```

Trailing commas are allowed. The type is inferred from the value and the
constant is immutable.

## 4.2 Enumerations

Come uses `enum` as a value generator inside `const` blocks:

```come
const (
    RED = enum,
    YELLOW,
    GREEN,
    UNKNOWN,

    // explicit starting value:
    HL_RED = enum(8),
    HL_YELLOW,
    HL_GREEN
)
```

- Enum values auto-increment from the given (or implied 0) start
- Enum constants are integers

# 5. Aliases

The `alias` keyword provides C-like typedef and macro behavior.

## 5.1 Type alias

```come
alias tcpport_t = ushort
alias Point = struct Point
```

Grouped form:

```come
alias (i8, i16, i32, i64) = (byte, short, int, long)
```

## 5.2 Constant alias

```come
alias MAX_ARRAY = 5
```

## 5.3 Macro alias

```come
alias SQUARE(x) = ((x) * (x))
```

# 6. Types

## 6.1 Primitive types

| Type | Aliases | Size | Description |
| :--- | :--- | :--- | :--- |
| `bool` | — | 1 byte | `true` / `false` |
| `byte` | `i8` | 1 | Signed 8-bit integer |
| `ubyte` | `u8` | 1 | Unsigned 8-bit integer |
| `short` | `i16` | 2 | Signed 16-bit integer |
| `ushort` | `u16` | 2 | Unsigned 16-bit integer |
| `int` | `i32` | 4 | Signed 32-bit integer |
| `uint` | `u32` | 4 | Unsigned 32-bit integer |
| `long` | `i64` | 8 | Signed 64-bit integer |
| `ulong` | `u64` | 8 | Unsigned 64-bit integer |
| `float` | `f32` | 4 | IEEE 754 single precision |
| `double` | `f64` | 8 | IEEE 754 double precision |
| `wchar` | — | 4 | Single Unicode scalar value |
| `void` | — | 0 | No value (return type only) |

Character literals:

```come
byte c = 'A'     // ASCII
wchar w = '🤔'   // Unicode scalar
```

Numeric literals may use `'` as a digit separator; an `L` suffix selects
`long`:

```come
long big = 10'000'000'000L
```

## 6.2 Structs

```come
struct Point {
    int x
    int y
}
```

Initialization uses C-style designated form:

```come
struct Point p = { .x = 5, .y = 10 }
```

**Memory layout.** The default layout is C-compatible: fields in
declaration order with platform ABI alignment and padding. This is what
makes `extern "C"` calls sound (see 13).

Two modifiers adjust layout, written as plain keywords:

```come
packed struct Header {   // no padding; fields back-to-back
    byte kind
    int length
}

align(16) struct Line {  // struct starts on a 16-byte boundary
    byte data[64]
}
```

Structs may declare methods, written `Type.name()` with an implicit
`self` referring to the instance:

```come
byte TCP_ADDR.nport() {
    return net.hton(self.portnumber)
}
```

## 6.3 Unions

Overlapping storage, C-compatible layout (size and alignment are those of
the largest member):

```come
union TwoBytes {
    short signed_s
    ushort unsigned_s
    byte first_byte
}
```

## 6.4 Arrays

Fixed-size arrays:

```come
int arr[10]
```

Dynamic arrays (always headered buffers, see 7):

```come
int dyn[]
int nums[] = {1, 2, 3, 4, 5}
```

Indexing is zero-based and bounds-checked: `arr[i]`, `dyn[i]`.

## 6.5 Strings

Strings are immutable, UTF-8 encoded sequences of characters:

```come
string name = "John"
string empty = ""
```

Character-first semantics:

- `s[i]` returns the i-th character as a `wchar` (decoded Unicode scalar)
- `s.length()` counts characters
- `s.size()` counts bytes

There is no in-place character mutation (`s[0] = 'x'` is an error).
Methods return new strings; reassignment replaces the buffer:

```come
string t = s.upper()   // new string; s unchanged
```

`+` concatenates, producing a new string:

```come
string greeting = "Hello, " + name
```

Byte-level access is available through explicit methods (see
`Come_string.md`).

Strings are immutable: there is no in-place mutation and `resize()` does
not apply. (`s[i]` returns a decoded character *value*, not a fixed
memory slot — indexed store would hide variable-length byte surgery.)
For mutable character data use `wchar[]`; for mutable bytes use `byte[]`.

## 6.6 Maps

Maps are statically typed key-value collections:

```come
map{string, int} scores
map{string, int} m = {"a": 1, "b": 2}
```

Literals use braces with `:` pairs. Access and assignment use `[]`:

```come
m["c"] = 3
int v = m["a"]       // 1
```

Reading a missing key returns the zero value for `V` and sets `ERR`
(see 12). `m.length()` returns the pair count; `m.keys()` returns the
keys as an array.

## 6.7 The `link` type

`links` are the endpoints of coroutine channels (see 9):

```come
(a, b) = colink()   // a, b are links
```

A `link` supports `.read()`, `.write(buf)`, and `.close()`.

## 6.8 Numeric conversions

There is **no implicit narrowing**, ever. Converting to a smaller type
requires an explicit cast written `T(value)`:

```come
long big = 1000
int x = int(big)      // explicit; truncates if out of range
```

Implicit widening is allowed only where no value can be lost:

```come
int i = 42
long l = i            // fine: int -> long is lossless
double d = i          // fine
int j = d             // compile error: needs int(d)
```

One rule, easy to teach: the compiler never silently discards information.

# 7. Ownership and Memory

## 7.1 Principles

- **No raw pointers** in user code.
- **No manual allocation or freeing.** There is no `.alloc()` and no
  `.free()`.
- Arrays, strings, and maps are **headered buffers**:
  `[ size_in_bytes | element_count | data... ]`.
- Every buffer has an **owner**: the scope or module whose lifetime bounds
  it. When the owner dies, its buffers die with it — deterministically,
  without a garbage collector.
- `resize(n)` is the single primitive that changes a dynamic buffer's
  size (see 7.3).

## 7.2 Ownership rules

- Assignment aliases: `b = a` makes `b` a second reference to the same
  buffer (same owner). Use `.dup()` to copy.
- `.chown(new_owner)` explicitly re-parents a buffer (and its subtree)
  to a longer-lived owner — a composite variable or a module.
- **A reference may not outlive its buffer's owner.** The compiler rejects
  it, or promotes the buffer to the outer context:

```come
byte r[]
{
    byte buf[]
    buf.resize(100)
    r = buf   // compile error (or promotion): buf's buffer dies at }
}
```

- **Promotion** (stack/static → dynamic, or inner → outer context) is
  deterministic and compiler-managed, triggered only by clear events:
  the buffer is passed somewhere, stored somewhere longer-lived, or
  transferred. It is invisible to the programmer and never a hidden
  heap allocation surprise — the events are enumerable.

## 7.3 `resize()`

`resize(n)` grows or shrinks a dynamic buffer in place, in the current
memory context:

```come
int dyn[]
dyn.resize(3)    // length 3, storage allocated
dyn.resize(512)  // grows
dyn.resize(0)    // releases storage
```

- `resize(0)` releases the body but the header survives: the buffer is
  empty, never null. Empty is a valid state, not a missing object.
- Resizing a fixed-size array (`int arr[10]`) is a compile error.
- `resize` on strings is not applicable (strings are immutable; build new
  ones instead).

## 7.4 Universal built-in methods

Every composite type supports:

| Method | Description |
| :--- | :--- |
| `.type()` | Type name as a string, e.g. `"string"`, `"map"`, `"struct"` (compile-time) |
| `.size()` | Total memory size in bytes, including header |
| `.length()` | Logical element count (array elements, string characters, map pairs, struct fields) |
| `.owner()` | The composite variable or module that currently owns the buffer |
| `.chown(o)` | Re-parent the buffer (and its subtree) to owner `o` |
| `.dup(depth)` | Copy — see 7.5 |

Type-specific methods (array `.resize()`, string transforms, `sock`
channel ops, …) are documented with their types, not here.

## 7.5 The `.dup()` method

`.dup()` copies a composite. The depth argument is literal — it counts how
many levels are freshly copied:

| Call | Meaning |
| :--- | :--- |
| `b = a.dup(0)` | New reference to the **same** buffer; same owner as the origin |
| `b = a.dup(1)` | Fresh top-level buffer; nested children shared |
| `b = a.dup(n)` | Fresh down to level `n` |
| `b = a.dup()` | Full deep copy |

Every `.dup()` except `.dup(0)` allocates; the new top-level buffer is
owned by the current scope. `b = a` (no `.dup()`) is also a same-buffer
reference, like `.dup(0)`.

# 8. Functions

## 8.1 Declaration and definition

```come
int add(int a, int b)        // prototype

int add(int a, int b) {
    return a + b
}
```

## 8.2 Multiple return values

Functions may return tuples; call sites destructure explicitly — no magic
unpacking:

```come
(int, string) add_n_compare(int a, int b) {
    return (a + b), (a > b) ? ">" : "<="
}

int sum
string cmp
(sum, cmp) = add_n_compare(i, s)
```

## 8.3 Functions as arguments

In v1, functions can be passed **only as arguments** — there are no
function variables, struct fields, or returns holding functions. The
parameter is written in function-declaration shape (as in C):

```come
void sort(int data[], int compare(int a, int b)) {
    // ... compare(data[i], data[j]) ...
}

sort(nums, ascending)   // pass a named function directly
```

The signature must match exactly or it is a compile error. Only named
functions — no lambdas, no captured variables. (A function value is just
a reference to code, so there are no lifetime or ownership questions.)

# 9. Coroutines

Coroutines are isolated, cooperatively scheduled tasks. Data races are
impossible by construction: a coroutine sees only its own locals, and
buffers are never shared between coroutines.

## 9.1 Declaration and launch

A coroutine is declared with the `coroutine` keyword and started with
`spawn` (a statement):

```come
coroutine worker(link s) {
    req = s.read()
    s.write(process(req))
}

(a, b) = colink()
spawn worker(a)     // a's ownership moves into the coroutine
b.write(request)    // main -> worker
reply = b.read()    // worker -> main
```

Rules:

- **Isolation:** inside a coroutine, module variables are not visible —
  only locals and parameters. The compiler enforces this.
- **Ownership transfer:** buffer arguments change owner to the coroutine
  on `spawn`. Value types (ints, bools, …) are copied.
- **No shared state:** two coroutines can never hold the same buffer, so
  no locks are needed and none exist.
- **Results travel through links** (see 9.2). Coroutines do not return
  values (`return x` is a compile error; bare `return` is allowed).
- Only `coroutine`-declared functions may be spawned; they cannot be
  called as ordinary functions.

## 9.2 `colink()` and `link`

`colink()` creates a bidirectional channel, returning its two ends via
tuple destructuring. Each `link` supports:

| Method | Description |
| :--- | :--- |
| `.write(buf)` | Moves `buf`'s ownership into the channel (no copy, no sharing) |
| `.read()` | Pauses the coroutine until data arrives, then takes ownership of it |
| `.close()` | Closes this end |

- A `read()` on an empty channel **pauses** the coroutine until data
  arrives or the peer closes.
- Reading after the peer closed returns an **empty buffer** — end of
  data, never null.
- Writing after the peer closed sets `ERR`.

### Waiting on several links

`wait(...)` pauses the coroutine until any of the given links is readable
(or its peer closed), and returns which link is ready — the light,
userland version of the epoll idea:

```come
(a, b) = colink()
(c, d) = colink()
spawn worker1(a)
spawn worker2(c)

while (true) {
    link ready = wait(b, d)  // pauses until either worker replies
    reply = ready.read()
    if (reply.length() == 0) { break }  // empty = that worker closed
    handle(reply)
}
```

- The returned link is another reference to a link the caller already
  owns (same owner — the `.dup(0)` rule), so no ownership questions arise.
- `wait` wakes when a peer closes too; the following `read()` returning
  empty tells you which one finished. A coordinator never hangs on a
  dead worker.
- If a link is already readable, `wait` returns immediately.
- At least one link is required. No timeouts in v1.

## 9.3 Scheduling and lifecycle

- Scheduling is cooperative: a coroutine yields when it `read()`s an
  empty sock, or explicitly with the `yield` statement (for long
  computations with no channel activity).
- When a coroutine ends, its socks close automatically and its memory is
  freed — a reader never waits forever on a dead writer.
- When `main` returns, the program ends; remaining coroutines are stopped
  and their contexts freed.

# 10. Variables and Type Inference

## 10.1 `var`

`var` is *inference*, not dynamic typing:

```come
var x
x = 123        // x becomes int, permanently

var y = "hello"   // y becomes string
```

Rules:

- The type is locked at the **first** assignment and can never change.
- Assigning a different type afterwards is a compile error.
- Everything remains statically typed; `var` only saves writing the type.

# 11. Control Flow

## 11.1 If / else

```come
if (flag) {
    ...
} else {
    ...
}
```

## 11.2 Switch

Come `switch` does **not** fall through. `fallthrough` must be explicit:

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

`default` is optional.

## 11.3 Loops

```come
for (int i = 0; i < 10; i++) { }

while (cond) { }

do { } while (cond)
```

`break` and `continue` behave as in C.

# 12. Errors

Runtime failures are reported through the global `ERR` object
(full reference in `Come_std.md`):

```come
if (ERR.set()) {
    std.err.printf("Error: %s\n", ERR.str())
}
```

- `ERR.no()` — last system error code; 0 means no error.
- `ERR.str()` — human-readable message for the current code.
- `ERR.set()` — true when an error is recorded (`ERR.no() != 0`).

Guidance: `ERR` is for **expected failures** the program should handle
(missing file, refused connection). Check it; do not ignore it.

For states the program genuinely **cannot continue from**, use:

```come
ERR.fatal("index out of range in module table")
```

- `ERR.fatal(msg)` halts the program immediately, in every build —
  it is never compiled out.
- Rule of thumb: if it can happen in production, handle it with `ERR`;
  `fatal` is only for the truly unrecoverable. (There is no C-style
  `assert`: it is too easy to misuse for things that do happen.)

There are no exceptions and no `(value, err)` tuples.

# 13. Foreign Functions

A Come program can call C libraries, but raw pointers must never leak
into user code. Therefore `extern "C"` declarations may appear **only
in system modules**:

```come
// in a system module only:
extern "C" int c_open(string path, int flags)
```

User code calls the system module's functions normally; the system
module author performs the unsafe translation behind the boundary.
Structs crossing the boundary use C-compatible layout (see 6.2).

# 14. Expressions and Operators

- Arithmetic: `+` `-` `*` `/` `%` (`+` also concatenates strings)
- Bitwise: `&` `|` `^` `~` `<<` `>>`
- Logical: `&&` `||` `!`
- Relational: `==` `!=` `<` `>` `<=` `>=`
- Ternary: `cond ? a : b`
- Increment/decrement: `++` `--` (prefix and postfix)
- Assignment: `=` and compound `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=`

`==` on composite values tests **identity** (same buffer), not contents —
use type methods such as string `.cmp()` for content comparison.

# 15. Semicolons

`;` is optional. A newline terminates a statement; `;` separates multiple
statements on one line.

# 16. Keywords

| Keyword | Description |
| :--- | :--- |
| `module` | Declares the file's module |
| `main` | Program entry module |
| `import` | Access another module |
| `as` | Force system-module import |
| `export` | Mark symbols public |
| `alias` | Type / constant / macro alias |
| `const` | Immutable values |
| `enum` | Value generator in `const` blocks |
| `struct` | Composite type |
| `packed` | Struct layout modifier: no padding |
| `align` | Struct layout modifier: alignment |
| `union` | Overlapping composite type |
| `map` | Key-value collection type |
| `string` | UTF-8 string type |
| `link` | Coroutine channel endpoint |
| `coroutine` | Declare a coroutine |
| `spawn` | Launch a coroutine |
| `yield` | Cooperative pause inside a coroutine |
| `var` | Type inference placeholder |
| `void` | No value |
| `bool` | Boolean type |
| `byte` `ubyte` `short` `ushort` `int` `uint` `long` `ulong` `float` `double` | Numeric primitives |
| `wchar` | Unicode scalar type |
| `if` `else` | Conditional |
| `switch` `case` `default` `fallthrough` | Selection |
| `for` `while` `do` `break` `continue` | Iteration |
| `return` | Return (supports tuples) |
| `true` `false` | Boolean literals |
| `extern` | Foreign declarations (system modules only) |

# 17. Design Summary

| Feature | Decision |
| :--- | :--- |
| Pointers | Removed from user code |
| Memory management | No `alloc`/`free`; `resize()` only; owner-based lifetime |
| Null | None — empty is a valid state |
| UTF-8 | Default; character-first strings |
| Switch | No implicit fallthrough |
| Typing | Static, with `var` inference; no implicit narrowing |
| Strings, Arrays, Maps | Headered-buffer objects; maps are `map{K, V}` |
| Functions | Multiple returns via tuples; passable as arguments only |
| Coroutines | `coroutine`/`spawn`/`colink`/`wait`; race-free by construction |
| Errors | Global `ERR`; `ERR.fatal` for unrecoverable states; no exceptions |
| Concurrency | Coroutines in-language; OS threads via system modules |
| FFI | `extern "C"` confined to system modules |
| Struct layout | C-compatible default; `packed` / `align(n)` modifiers |
| Interfaces | Not in v1 |
| Modules | One per file; private by default; deterministic lifecycle |
| Compatibility | C mental model, without C footguns |
