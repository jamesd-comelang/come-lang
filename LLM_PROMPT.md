# Come — LLM Prompt

**For humans:** copy everything below the line into any LLM
(chat, API system prompt, or project instructions). It teaches
the model the Come language well enough to write correct,
compilable Come source code. It is self-contained: the model
needs nothing else. Keep it with the project
(`USER_GUIDE.md` is the human-readable version of the same
material).

---

You are writing code in **Come**, a small compiled language
that transpiles to C. Follow this specification exactly. Come
looks like C but differs in ways that will make C, Go, Rust,
or Python habits produce code that does not compile. The
"Traps" section at the end lists the differences that matter
most — read it before writing code.

## Output rules

- Return complete `.co` files unless told otherwise. Every
  file starts with `module <name>` and its imports.
- Do not invent syntax, methods, or library functions. If a
  capability is not described here, say so instead of
  guessing — do not fall back to C, Go, or Rust idioms.
- Format like the examples: 4-space indentation, one
  statement per line, `return` values in parentheses only
  for tuples.

## Program structure

```come
module main
import std

int main() {
    std.out.printf("Hello world!\n")
    return 0
}
```

- Entry points (exactly one per program):
  `int main()`, `int main(string args[])`, `void main()`,
  `void main(string args[])`. `args[0]` is the program name.
- Newlines end statements; `;` separates statements on one
  line. `//` line comments, `/* ... */` block comments.
- Output is `std.out.printf(fmt, ...)` with C-style `%d`,
  `%s`, `%f`, `%c` verbs. There is no `print` statement and
  no string interpolation.

## Types

| Type | Meaning |
|---|---|
| `byte` | 8-bit **signed** integer |
| `short`, `int`, `long` | 16/32/64-bit signed; `int` is 64-bit |
| `float`, `double` | IEEE floating point |
| `bool` | `true` / `false` |
| `wchar` | one Unicode codepoint; char literals: `'A'` |
| `string` | immutable text (below) |
| `T[]` | dynamic array of T; `T[N]` fixed-size array |
| `map{K, V}` | hash map (below) |
| `struct`, `union` | aggregates (below) |
| `link` | coroutine channel end (below) |

- Literals: `42`, `10'000'000` (digit separators),
  `9000000000L`, `3.14`, `"text"` (escapes `\n`, `\"`, `\\`;
  avoid `\xNN` — it is buggy when followed by a hex letter),
  `'c'`, `true`. Float literals have **no exponent form**
  (write `1500.0`, not `1.5e3`).
- `var x = expr` infers and then **locks** the type; later
  assignments must match.
- **No implicit narrowing.** Assigning a wider integer to a
  narrower variable is a compile error. Cast explicitly:
  `byte b = byte(x)`. Widening is implicit. Casts truncate
  toward zero. There is no `null` in the language.

## Control flow

```come
if (cond) {
    // ...
} else if (other) {
    // ...
} else {
    // ...
}

while (cond) { }
for (int i = 0; i < n; i++) { }       // update clause: only
                                      // ++/-- style, no i = i + 1
switch (v) {
    case 1:
        // runs only for 1: switch does NOT fall through
    case 2:
        // ...
    default:
        // ...
}
```

- `switch` falls through only if a case ends with the
  keyword `fallthrough`.
- `break` / `continue` bind to the innermost loop.
- Ternary exists: `cond ? a : b`.
- Operators: arithmetic `+ - * / %`, comparison `== != < <=
  > >=`, logic `&& || !`, bitwise `& | ^ ~ << >>`, compound
  assignment `+= -= *= /= %=`, prefix and postfix `++ --`.
  Integer `/` truncates toward zero; `%` takes the
  dividend's sign. **Division or remainder by zero crashes
  the program** — guard denominators.

## Functions

```come
int add(int a, int b) {
    return a + b
}

// Multiple results: return a tuple, unpack at the call site.
(int, int) divmod(int a, int b) {
    return (a / b, a % b)
}
// (q, r) = divmod(17, 5)

// Functions as arguments ONLY (no function variables, no
// lambdas, no closures). The parameter is written in
// C declaration shape — there is no fn-type syntax:
int apply(int x, int f(int v)) {
    return f(x)
}
// apply(21, double_it)   // pass a named function
```

- Methods: declare with the type name; `self` is the
  receiver. `int Vec3.sum() { return self.x + self.y }`.
  A method mutates through `self` explicitly.
- Recursion is fine.

## Arrays

```come
int a[] = {1, 2, 3}            // literals use { }, NEVER [ ]
a.append(4)                    // grow by one
a.insert(0, 99)                // insert at index (sets ERR if out of range)
a.remove(0)                    // remove element at index
a.resize(10)                   // set length; new elements are zero
int n = a.length()
int x = a[0]                   // indexing is SAFE: out-of-range
                               // reads give 0 and set ERR;
                               // out-of-range writes are dropped
int part[] = a.slice(0, 2)     // copy of elements 0..1
bool has = a.contains(2)
int at = a.index(2)            // -1 when absent
a.reverse()
a.sort(asc)                    // sort REQUIRES a comparator:
int asc(int x, int y) {        // negative = x first, 0 = equal,
    if (x < y) { return -1 }   // positive = y first
    if (x > y) { return 1 }
    return 0
}
int fixed[12]                  // fixed array, zero-filled,
                               // can never grow
```

## Strings

Strings are **immutable**; every operation returns a new
string.

```come
string s = "Hello, World"
int chars = s.length()         // CHARACTERS, not bytes
int bytes = s.bytesize()       // bytes
wchar c = s[0]                 // character at index (safe, like arrays)
string sub = s.substr(0, 5)    // clamps to the string
string up = s.upper()          // also .lower()
string cat = s + "!"           // concatenation with +
string parts[] = s.split(",")  // separator is a string
string j = "-".join(parts)     // join is called on the separator
string t = "  pad  ".trim()    // also .ltrim() .rtrim()
string r = "aaa".replace("a", "bb")
int cmp = s.cmp(other)         // content compare: <0, 0, >0
string msg = sprintf("x=%d", 42)   // bare sprintf returns a string
```

- **Comparison trap:** `s == "literal"` compares content.
  `s1 == s2` (two variables) compares **identity**, not
  content. For content between variables use `s1.cmp(s2) == 0`.

## Maps

```come
map{string, int} ages = {"Ada": 36, "Grace": 85}
ages["Edsger"] = 72            // insert or update
int a = ages["Ada"]
int missing = ages["Nobody"]   // 0, NO exception; ERR set to ENOENT
string keys[] = ages.keys()    // iterate via keys()
int pairs = ages.length()
```

- Only `map{string, int}` and `map{string, string}` are
  supported in this version. Do not use other key/value
  types, and do not rely on `map.remove()` for typed maps
  (it is currently broken — avoid it).

## Structs, unions, references

Structs are **values**: assignment and pass-by-value copy
the fields.

```come
struct Vec3 {
    int x
    int y
    int z
}

int Vec3.sum() {
    return self.x + self.y + self.z
}

var a = struct Vec3{1, 2, 3}   // typed literal, a value
var b = a                      // independent copy of the fields
```

- If a field is an array/string/map, copying the struct
  shares that field's **elements** (the field's handle is
  copied, not its contents).
- To let a function **modify** the caller's struct, use a
  reference — `&` on both sides, and member access stays `.`:

```come
void move_x(&(struct Vec3) v, int dx) {
    v.x = v.x + dx
}
// call site:
move_x(&p, 10)                 // the & is REQUIRED; omitting
                               // it is a compile error
```

- References are structs-only, non-owning, and never written
  with `->`. No reference fields, no reference return types.
- Unions declare the same way and share storage between
  members; read a different member to reinterpret the bits:

```come
union Bits {
    int i
    float f
}
```

## Errors: ERR (no exceptions)

There are **no exceptions** and no `try`. Fallible
operations leave a defined result and set the global `ERR`
object:

- missing map key → zero value, `ERR` = ENOENT
- out-of-range array/string read → zero value, `ERR` = ERANGE
- read on a drained, closed link → empty buffer (end of stream)

```come
int v = a[10]
if (ERR.set()) {                       // error pending?
    std.out.printf("%s\n", ERR.str())  // the message
    ERR.clear()                        // reset after handling
}
ERR.fatal("cannot continue")           // print and halt, always
```

- `ERR.no()` the errno-style code, `ERR.str()` the message,
  `ERR.set()` true while an error is pending, `ERR.clear()`
  resets. Success does **not** clear ERR — check immediately
  after the operation you care about.
- Every error is also recorded with its source location.
  `ERR.where()` returns `"<file>:<line>"` (your call line
  for library failures). `ERR.depth()` counts the recorded
  errors since the last clear; `ERR.no(i)` / `ERR.str(i)` /
  `ERR.where(i)` read entry `i` — 0 (the default) is the
  root cause, later errors append. An out-of-range `i`
  returns 0 or `""` and records a new error. Records are
  per coroutine: one coroutine's errors are invisible in
  another. `ERR.fatal(msg)` prints the message and the
  recorded root location.

## Memory and ownership

There is no `malloc`/`free` and no garbage collector. The
rules:

1. Every array/string/map is owned by a scope, a module, or
   another variable. It is released when its owner dies
   (usually at the end of the enclosing block).
2. Assignment shares: `int b[] = a` — both names see the
   same elements; writes through one show in the other.
3. **Growth forks:** when a shared value grows (`append`,
   `resize`, inserting into a full map), the grower gets a
   fresh data block; the other name keeps a valid snapshot
   of the old contents and length. They diverge from that
   point — this is intended.
4. Function parameters are shared handles: the callee can
   write the caller's elements, but if it grows the array,
   the caller's length does **not** change. Return the
   array to hand back a grown value.
5. Returning a value, or sending it to a coroutine,
   transfers ownership automatically — build locally and
   return freely.
6. `.chown(owner)` re-parents a value to a longer-lived
   owner (e.g. a variable that outlives the current block).
7. `.dup(0)` = shared copy (like assignment), `.dup(1)` =
   fresh top level with shared elements, `.dup()` = full
   deep copy.
8. Universal methods on arrays/strings/maps/structs:
   `.length()`, `.bytesize()`, `.owner()`, `.chown(x)`,
   `.dup(...)`, `.type()`. (Maps have no `.bytesize()`.)

## Modules

```come
module helpers          // first line of helpers.co

int answer() {          // plain functions are exported
    return 42
}
```

- Other files: `import helpers`. Resolution: beside the
  importing file (`helpers.co` or a `helpers/` directory
  whose files form one module), then `modules/`, then
  `src/`, then system modules. No paths, no build files.
- A module may define `void init()` (runs once before main,
  after its imports' inits) and `void exit()` (reverse
  order at program end).
- `import std` is the standard library: `std.out.printf`,
  `std.file` (file objects), `std.proc` (environment and
  processes), `std.srand`/`std.rand`, time helpers.
  `import string` adds the string toolbox above.
- Current limitation: another module's constants and struct
  literals are not fully usable cross-module yet; pass
  structs by value/reference between modules instead of
  constructing foreign literals.
- **Instances.** A module with module-level variables has
  state. A function that uses a module variable (or calls
  one that does) is *stateful* and can ONLY be called on
  an instance: `var w = window_module.create()`, then
  `w.set_title("B")`. Calling a stateful function through
  the namespace (`window_module.set_title("x")`) is a
  compile error - the namespace never implies a singleton.
  Stateless functions work both ways (`mod.f()` and
  `w.f()`). `w.destroy()` runs the module's `exit()`,
  frees the instance's memory, and is safe to repeat; any
  later call on `w` sets ERR ("module instance has been
  destroyed") and returns the zero value - it never
  crashes. `create()` on a module with no variables is a
  compile error. An instance's type is written as the
  module's name (`void use(window_module w) {...}`).
  Instances live in locals and parameters; functions
  cannot return them. A module defining `main()` is a
  program: its variables are plain globals, no instance
  needed. In a stateful module, `init()`/`exit()` run per
  instance (at create/destroy), not at import/program end.

## Coroutines and links

```come
module main
import std

coroutine producer(link out) {
    for (int i = 1; i <= 3; i++) {
        out.write(sprintf("item %d", i))
    }
}

int main() {
    (left, right) = colink()       // two ends of one channel
    spawn producer(left)
    string got = right.read()      // blocks until an item
    while (got.length() > 0) {     // empty = end of stream
        std.out.printf("got: %s\n", got)
        got = right.read()
    }
    return 0
}
```

- `coroutine name(args) { }` declares; `spawn name(args)`
  starts it. Coroutines cannot be called directly and
  cannot return values — results travel over links.
- `link.write(buf)` moves the buffer into the channel (do
  not use it afterwards). `link.read()` waits and takes
  ownership. `link.close()` closes an end; a coroutine's
  links close automatically when it ends.
- `wait(a, b)` blocks until any listed link is readable.
- Coroutines **cannot access module-level variables** — the
  compiler rejects it. Pass everything as arguments.
- Scheduling is cooperative on one thread: a blocking C
  call inside a coroutine stalls the others.

## Style

- `come fmt` is the canonical formatter: 4-space indent,
  canonical spacing, never adds/removes lines. Match its
  output in what you generate.
- Snake_case for functions and variables, CamelCase for
  struct/union type names, modules lowercase.

## Traps — where your other-language habits will break

1. Array and map literals use `{ }`, never `[ ]`.
2. No `null`, no exceptions, no `try`/`throw`, no classes,
   no interfaces, no generics, no lambdas/closures, no
   string interpolation, no `->`.
3. `byte` is signed. `int` is 64-bit, but integer *literal*
   arithmetic is evaluated at 32-bit width — put big values
   in `long` variables before combining them.
4. No implicit narrowing — cast explicitly (`byte(x)`).
5. `switch` does not fall through (keyword `fallthrough`).
6. Function arguments that are functions use C declaration
   shape (`int f(int v)`), and only named functions can be
   passed.
7. `sort` needs a comparator argument; there is no default.
8. String `==` between two variables is identity — use
   `.cmp()` for content.
9. Map lookup never throws; missing key = zero value + ERR.
10. Growing a shared array forks it (rule 3) — do not
    expect two names to stay in sync after one grows.
11. A callee growing your array does not change your
    length (rule 4) — return grown arrays.
12. References need `&` at the call site and `&(struct T)`
    in the declaration, and are structs-only.
13. Float literals have no exponent form.
14. Division/remainder by zero crashes — guard it.
15. Coroutines cannot see module variables; pass arguments.
16. Do not call `map.remove()` on typed maps (broken), do
    not use `struct.dup()` (broken), do not store structs
    in arrays or maps or send them over links (unsupported
    in this version) — use flat scalar collections and
    indexes instead.
