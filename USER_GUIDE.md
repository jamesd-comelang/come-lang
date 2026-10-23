# Come — User Guide

This guide teaches you to **write programs in Come**: the language,
the standard library, and the everyday workflow of building,
running, and testing your code. Every example is a real program
you can save and run.

If you want to work on the compiler itself, see
[`DEVELOPER.md`](DEVELOPER.md) instead. For the precise rules,
the normative reference is [`docs/Come_spec.md`](docs/Come_spec.md),
with [`docs/Come_types.md`](docs/Come_types.md) as the type summary.

---

## 1. What Come is

Come is a small compiled language that turns into C. Three ideas
shape everything you will write:

- **No garbage collector, no manual memory management.** Memory
  belongs to *owners* — usually the scope that created a value —
  and is released automatically when the owner goes away. You
  think about *who owns this*, once, and the compiler handles
  the rest.
- **Mistakes are values, not crashes.** Reading past the end of
  an array gives you `0` and sets the error object; a missing
  map key gives you the zero value. Nothing throws, nothing
  segfaults on ordinary errors.
- **Concurrency is built in.** Coroutines with typed channels
  are part of the language, not a library afterthought.

A first taste — a complete program:

```come
module main
import std

int main() {
    string names[] = {"Ada", "Grace", "Edsger"}
    for (int i = 0; i < names.length(); i++) {
        std.out.printf("Hello, %s!\n", names[i])
    }
    return 0
}
```

## 2. Installing Come

**From the Debian package** (Ubuntu/Debian, amd64): download
`come_1.0.0_amd64.deb` from the GitHub release and install it:

```sh
sudo dpkg -i come_1.0.0_amd64.deb
```

**From source:**

```sh
git clone https://github.com/jamesd-comelang/come-lang
cd come-lang
make
```

Either way you get one program, `come`. Check it works:

```sh
come            # prints the command list
come build --help
```

Come currently runs on **Linux (x86-64)**. You also need `gcc`
installed — the compiler uses it to turn generated C into your
program, and it will tell you plainly if gcc is missing.

## 3. Your first program

Save this as `hello.co`:

```come
module main
import std

int main() {
    std.out.printf("Hello world!\n")
    return 0
}
```

Build and run it:

```sh
come build hello.co     # produces ./hello
./hello
```

The commands you will use every day:

| Command | What it does |
|---|---|
| `come build hello.co` | Compile and link → an executable |
| `come build hello.co -o app` | Same, with the output name you choose |
| `come run hello.co -- args` | Build to a temp file and run it, passing `args` |
| `come check hello.co` | Compile only — catch errors without producing a program |
| `come fmt hello.co` | Reformat the source canonically, in place |
| `come test` | Build and run the tests in your project's `t/` folders (§15) |

Program arguments arrive through the other `main` signature:

```come
module main
import std

int main(string args[]) {
    // args[0] is the program's own name
    if (args.length() > 1) {
        std.out.printf("You said: %s\n", args[1])
    }
    return 0
}
```

## 4. The basics

### Variables and types

```come
int count = 42            // 64-bit signed integer
byte small = 7            // 8-bit, signed
long big = 9000000000L
double pi = 3.14159
bool ready = true
wchar letter = 'A'        // one Unicode character
string name = "Ada"
var inferred = 10       // var = infer the type; it locks
                          // at the first assignment
```

Numbers can use separators for readability: `10'000'000`.

**Come never narrows a number silently.** Assigning an `int` to
a `byte` variable is a compile error — write the cast when you
mean it:

```come
int x = 200
byte b = byte(x)          // explicit cast: -56
// byte bad = x           // error: no implicit narrowing
```

### Control flow

```come
if (count > 10) {
    std.out.printf("many\n")
} else if (count > 0) {
    std.out.printf("some\n")
} else {
    std.out.printf("none\n")
}

int i = 0
while (i < 3) {
    i++
}

for (int j = 0; j < 3; j++) {
    // j = 0, 1, 2
}

switch (count) {
    case 0:
        std.out.printf("zero\n")
    case 1:
        std.out.printf("one\n")
    default:
        std.out.printf("other\n")
}
```

Note: `switch` does **not** fall through — each case ends at the
next `case`. Write the keyword `fallthrough` at the end of a case
if you really want the C behavior.

### Comments and statements

`//` starts a line comment; `/* ... */` is a block comment.
Newlines end statements; use `;` to put several on one line.

## 5. Functions

```come
int add(int a, int b) {
    return a + b
}

int main() {
    int total = add(2, 3)     // 5
    return 0
}
```

**Multiple results** come back as a tuple, unpacked where the
call happens:

```come
(int, int) divmod(int a, int b) {
    return (a / b, a % b)
}

int main() {
    (q, r) = divmod(17, 5)    // q = 3, r = 2
    return q + r
}
```

**Functions as arguments** — pass behavior into a function:

```come
module main
import std

int apply(int x, int f(int v)) {
    return f(x)
}

int double_it(int v) {
    return v * 2
}

int main() {
    std.out.printf("%d\n", apply(21, double_it))   // 42
    return 0
}
```

(In v1, functions can be passed as arguments but not stored in
variables or returned.)

**Methods** are functions attached to a type, declared with the
type name; `self` is the receiver:

```come
struct Counter {
    int n
}

void Counter.bump() {
    self.n = self.n + 1
}
```

## 6. Arrays

Arrays are Come's workhorse collection. They grow; you never
allocate them by hand.

```come
module main
import std

int main() {
    int a[] = {10, 20, 30}        // a literal
    a.append(40)                  // grow by one
    a.resize(10)                  // grow/shrink to a size;
                                  // new elements are zero
    std.out.printf("%d elements\n", a.length())

    int total = 0
    for (int i = 0; i < a.length(); i++) {
        total = total + a[i]
    }

    int part[] = a.slice(0, 2)    // a copy of elements 0..1
    a.sort(asc)                   // in place, ordered by a
                                  // comparator you supply
    return 0
}

int asc(int x, int y) {           // negative = x first
    if (x < y) { return -1 }
    if (x > y) { return 1 }
    return 0
}
```

Rules worth knowing:

- **Indexing is safe.** `a[100]` on a 3-element array reads as
  `0` and sets the error object (§10). Writing out of range is
  ignored safely. No crashes, ever.
- **Fixed arrays** never change size: `int days[12]`, created
  zero-filled.
- **Assignment shares.** `int b[] = a` makes `b` a second name
  for the same elements — writes through `b` show in `a`. When
  one of them *grows*, it gets a fresh block and the other keeps
  its snapshot (§11 explains the model). Use `.dup()` for an
  independent copy.

## 7. Strings

Strings are immutable text. Every operation returns a new
string; the original never changes.

```come
module main
import std

int main() {
    string s = "Hello, World"
    std.out.printf("%d characters, %d bytes\n",
                   s.length(), s.bytesize())

    wchar c = s[0]                       // 'H' — a character,
                                         // not a byte
    string sub = s.substr(0, 5)          // "Hello"
    string shout = s.upper()             // "HELLO, WORLD"
    string joined = s + "!"              // concatenation

    string csv = "apple,banana,cherry"
    string parts[] = csv.split(",")
    string back = "-".join(parts)        // "apple-banana-cherry"

    string tidy = "  padded  ".trim()    // "padded"
    string r = "aaa".replace("a", "bb")  // "bbbbbb"
    return 0
}
```

- `.length()` counts **characters**; `.bytesize()` counts
  **bytes**. For plain ASCII they agree; for text like `"café"`
  they do not, and characters are what you usually mean.
- Compare strings with `==` against literals (`s == "done"`).
  Comparing two string *variables* with `==` asks whether they
  are the same buffer; for content comparison between
  variables, use `.cmp()` (negative / zero / positive, like C's
  `strcmp`).
- Format text with `sprintf`, which returns a string:
  `string msg = sprintf("x=%d", 42)`.

## 8. Maps

A map stores key/value pairs. Declare the types in braces:

```come
module main
import std

int main() {
    map{string, int} ages = {"Ada": 36, "Grace": 85}
    ages["Edsger"] = 72                  // insert or update

    int a = ages["Ada"]                  // 36
    int missing = ages["Nobody"]         // 0 — no exception;
                                         // ERR is set (§10)

    string keys[] = ages.keys()          // all keys, as an array
    for (int i = 0; i < keys.length(); i++) {
        std.out.printf("%s -> %d\n", keys[i], ages[keys[i]])
    }
    return 0
}
```

Today, typed maps support `map{string, int}` and
`map{string, string}`. (An older untyped `map` type also exists
for string values; new code should use the typed form.)

## 9. Structs, unions, and references

### Structs are values

```come
module main
import std

struct Vec3 {
    int x
    int y
    int z
}

int Vec3.sum() {
    return self.x + self.y + self.z
}

int main() {
    var a = struct Vec3{1, 2, 3}     // a typed literal
    var b = a                        // a COPY — b is independent
    b.x = 99
    std.out.printf("%d %d\n", a.sum(), b.sum())   // 6  104
    return 0
}
```

Assigning or passing a struct copies its fields. If a field is
an array or string, the copy shares that field's elements (the
§11 buffer rule) — field writes are shared, resizes are not.

### References: change a struct in a function

Passing a struct copies it, so a function that must *modify*
your struct takes a **reference**, spelled with `&` on both
sides — in the declaration and at the call:

```come
module main
import std

struct Vec3 {
    int x
    int y
    int z
}

void move_x(&(struct Vec3) v, int dx) {
    v.x = v.x + dx                   // writes through to yours
}

int main() {
    var p = struct Vec3{1, 2, 3}
    move_x(&p, 10)                   // the & is required
    std.out.printf("%d\n", p.x)      // 11
    return 0
}
```

- Forget the `&` at the call and the compiler stops you, with a
  message naming the fix. Same if you add one where it does not
  belong.
- Member access is always `.` — through values and references
  alike. There is no `->`.
- A reference never owns anything; it points at a value that
  someone else owns, for as long as that value lives.
- References are for structs. (Ordinary `&` keeps its C meaning
  in the few standard-library calls that use out-parameters,
  like the `scanf` family.)

### Unions

A `union` holds one of several types in the same storage —
read it back as any member to reinterpret the bits:

```come
union Bits {
    int i
    float f
}
```

## 10. Errors: the ERR object

Come has no exceptions. Fallible operations report through one
global object, `ERR`, in the C tradition — but with defined
results, so ignoring an error never corrupts your program:

- A missing map key reads as the **zero value**; `ERR` = ENOENT.
- An out-of-range array read is **0**; `ERR` = ERANGE.
- A drained channel read is an **empty** buffer (§13).

Check right after the operation, while the error is fresh:

```come
module main
import std

int main() {
    int a[] = {1, 2, 3}
    int v = a[10]                    // 0, and ERR is set
    if (ERR.set()) {
        std.out.printf("oops: %s\n", ERR.str())
        ERR.fatal("cannot continue") // print and stop, always
    }
    return 0
}
```

- `ERR.no()` — the error number (an `errno` value).
- `ERR.str()` — the message text.
- `ERR.set()` — true while an error is pending. Note it stays
  set until another error replaces it, so check promptly
  rather than at the end of a long function.
- `ERR.clear()` — reset it yourself once you have handled an
  error and want a clean slate for the next check.
- `ERR.fatal(msg)` — when continuing makes no sense. It
  halts the program in every build, debug or release, and
  its report names where the recorded error started.

**Every error is recorded with its location.** `ERR.where()`
returns `"<file>:<line>"` — the place the error was set, or
your call line for a library failure — so a log line can
point at the exact statement:

```come
if (ERR.set()) {
    std.out.printf("failed at %s: %s\n", ERR.where(), ERR.str())
}
```

Errors form a short chain: if several happen before you
check, `ERR.depth()` counts them, and `ERR.no(1)`,
`ERR.str(1)`, `ERR.where(1)` read the later ones — index 0
(the default) is always the first, the root cause. Reading
past the end of the chain returns 0 or `""` and records a
new error of its own. Each coroutine keeps its own record,
so one coroutine's failure never shows up in another's.

## 11. Memory and ownership, in practice

You will never write `malloc` or `free` in Come. Instead, one
question governs memory: **who owns this value?** The default
answer is *the scope that created it* — when the scope ends,
its values are released:

```come
int main() {
    int total = 0
    if (true) {
        int temp[] = {1, 2, 3}   // owned by this if-block
        total = temp[0]
    }                            // temp is released here
    return total
}
```

Three operations move ownership around:

**Returning hands ownership to the caller.** Building a value
locally and returning it is the normal, safe pattern — the
value is re-parented to the caller's scope automatically:

```come
int[] make_scores() {
    int s[] = {90, 85, 77}
    return s          // ownership moves out with the value
}
```

**`.chown(owner)` re-parents explicitly.** When a value must
outlive the scope that made it, hand it to a longer-lived
owner — another variable that lives longer:

```come
module main
import std

int main() {
    int conn[] = {1}              // lives for all of main
    if (true) {
        int req[] = {2, 3}
        req.chown(conn)           // req now dies with conn,
    }                             // not with this block
    std.out.printf("%d\n", conn.length())
    return 0
}
```

**`.dup()` copies.** Assignment and `.dup(0)` share the same
elements; `.dup(1)` copies the top level; `.dup()` makes a
fully independent deep copy:

```come
int a[] = {1, 2, 3}
int shared[] = a          // same elements
int copy[] = a.dup()      // independent
copy[0] = 99              // a[0] is still 1
```

### The sharing rules, on one page

1. Assigning an array/string/map copies its *handle*, not its
   contents. Both names see the same elements.
2. When a shared value **grows** (`append`, `resize`, inserting
   into a full map), the growing variable gets a fresh block
   and the other keeps a valid snapshot of what it had. Nobody
   is ever left pointing at freed memory because of growth.
3. A function that receives an array parameter sees your
   elements and can write them — but if it grows the array,
   *your* length does not change. Return the array to hand a
   grown value back.
4. Values returned from functions and sent to coroutines are
   re-parented for you. Build locally, return freely.
5. A value dies with its owner. A second name for it does not
   keep it alive — hand it to a longer-lived owner with
   `.chown()` when it must survive.

Every buffer and struct also answers `.length()` (elements /
characters / pairs / fields), `.bytesize()` (bytes of the data
block), `.owner()`, and `.type()`.

## 12. Modules and projects

Every file starts with `module <name>`. `import` brings in
another module's exported functions, constants, and types:

```come
module main
import std          // the standard library
import helpers      // your own module, found automatically
```

How imports are found, in order: beside the importing file
(`helpers.co`, or a `helpers/` folder whose files together form
one **directory module**), then under `modules/`, then under
`src/`, then the installed system modules. You do not write
paths or build files for this — the layout *is* the project
system.

(To sanity-check a module on its own, build a small program
that imports it — compiling a file into a program links it,
so a module with no `main` is verified through its importers.)

A module can run setup and teardown code:

```come
module cache

void init() {
    // runs once, before main, after the modules
    // this one imports have initialized
}

void exit() {
    // runs at program end, in reverse order
}
```

A typical small project:

```
myapp/
  main.co            module main — the entry point
  helpers.co         module helpers
  net/
    net.co           module net (a directory module:
    client.co         several files, one module)
  t/
    helpers_test.co  tests, run by `come test` (§15)
```

Struct types and constants exported from a module are usable
through it, with one current limitation: cross-module constants
and struct *values* are not yet fully namespaced (see the
spec's §3.4 note) — passing structs by value or reference
between your own modules works; constructing another module's
struct literal in your file may not yet.

**Stateful modules: instances.** The modules above are
*stateless* - pure functions you call through the namespace
(`helpers.answer()`). A module that keeps **module-level
variables** has state, and state belongs to an *instance*:

```come
module window_module

string title = "untitled"

void set_title(string t) {
    title = t
}

string get_title() {
    return title
}
```

```come
var WindowB = window_module.create()
WindowB.set_title("Window B")
std.out.printf("%s\n", WindowB.get_title())   // Window B
WindowB.destroy()
```

The rules:

- A function that uses a module variable (directly, or by
  calling another function that does) is **stateful** and
  can only be called on an instance. Calling it through
  the namespace - `window_module.set_title("x")` - is a
  compile error that shows you why. There is no hidden
  singleton behind the namespace.
- A function that touches no state can be called both
  ways: `window_module.twice(21)` and `w.twice(21)` are
  the same function.
- `create()` runs the variables' initializers and the
  module's `init()`, per instance; each instance allocates
  in its own root. `destroy()` runs `exit()` and frees
  everything the instance owns. Two instances never see
  each other's state.
- Calling anything on a destroyed instance is safe: it
  sets `ERR` ("module instance has been destroyed") and
  returns the zero value. `destroy()` itself is safe to
  call twice.
- An instance's type is the module's name, so instances
  can be passed to functions:
  `void use_it(window_module w) { ... }`.
- `create()` on a module with no variables is a compile
  error - there is nothing to instantiate.
- Your entry module (the one with `main`) is a program,
  not a library: its module variables work as plain
  globals, exactly as in §12's examples.

## 13. Coroutines and links

A **coroutine** is a function that runs alongside others,
taking turns. Coroutines talk through **links** — typed
channels that carry buffers.

```come
module main
import std

coroutine producer(link out) {
    for (int i = 1; i <= 3; i++) {
        string msg = sprintf("item %d", i)
        out.write(msg)
    }
}

int main() {
    (left, right) = colink()      // the two ends of a channel
    spawn producer(left)          // starts running alongside

    string got = right.read()     // blocks until an item arrives
    while (got.length() > 0) {
        std.out.printf("got: %s\n", got)
        got = right.read()        // empty = producer finished
    }
    return 0
}
```

Output:

```
got: item 1
got: item 2
got: item 3
```

- `spawn` starts a coroutine; only coroutines can be spawned,
  and coroutines cannot be called like functions or return a
  value — results travel over links.
- `.write(buf)` hands the buffer to the channel (ownership
  moves with it). `.read()` waits for the next buffer and
  takes ownership. When the other end closes and the channel
  is drained, reads return an **empty** buffer — that is the
  end-of-stream signal, never null.
- `wait(linkA, linkB)` waits until *any* of the links has
  something to read, for fan-in patterns.
- Coroutines cannot see the module's global variables — the
  compiler rejects it. Everything a coroutine needs arrives
  as arguments. Links close by themselves when a coroutine
  ends, and if every coroutine ends up waiting forever, the
  runtime reports a deadlock instead of hanging.

Coroutines take turns on one thread — a coroutine that calls
a blocking C function stalls the others until it returns.

## 14. The standard library at a glance

`import std` is the front door (full reference:
[`docs/Come_std.md`](docs/Come_std.md)):

- **Output**: `std.out.printf(...)`, `std.out.flush()`.
- **Files**: `std.file` objects with open/read/write/close,
  plus whole-file helpers in the `file_demo` example.
- **Processes & environment**: the `std.proc` object —
  environment variables, running commands, exit codes.
- **Random**: `std.srand(seed)`, `std.rand()`.
- **Time**: the `std.time` helpers.
- And the C library's familiar surface (math, ctype, string
  functions) through the same module, for when you need it.

`import string` adds the string toolbox shown in §7
(`split`/`join`/`trim`/`replace`, case conversion, searching).
`import array` and the other collection docs live beside the
spec in `docs/` (`Come_array.md`, `Come_map.md`).

## 15. Testing your own code

Put test programs in any folder named `t/` in your project —
each a small program whose exit code says pass (0) or fail:

```come
module helpers_test
import std
import helpers

int main() {
    if (helpers.answer() != 42) {
        std.out.printf("FAIL: answer\n")
        return 1
    }
    std.out.printf("PASS: helpers\n")
    return 0
}
```

```sh
come test            # discover and run all t/ tests
come test --bench    # ... with timings (5 runs each)
come test --cover    # ... with line coverage of your .co files
come test --race     # ... rebuilt with ThreadSanitizer
```

Each test builds in isolation and gets 30 seconds. The summary
line (`Results: 4 passed, 0 failed`) and a nonzero exit code on
failure make it CI-friendly. Run `come fmt` over your sources
to keep them canonical — it never adds or removes lines, only
spacing and indentation, and running it twice changes nothing.

## 16. Common surprises

- **"My function's resize didn't change the caller's array."**
  By design (§11, rule 3): parameters share elements, not
  lengths. Return the array, or resize before passing.
- **"After append, my two arrays disagree."** Growth forks
  (§11, rule 2): the one that grew moved to a fresh block; the
  other kept its snapshot. If two names must stay in lockstep,
  keep one owner and pass it around.
- **`a == b` on two strings said false but they look equal.**
  `==` between two string variables compares identity, not
  content. Use `a.cmp(b) == 0` for content — or compare
  against a literal, which *is* a content comparison.
- **`ERR` was set from an error three calls ago.** ERR keeps
  the most recent error until another replaces it; success
  does not clear it. Check immediately after the operation you
  care about, and `ERR.clear()` when you have handled one.
- **Integer division by zero stops the program.** Division
  and remainder by zero are a hard crash today (a known issue
  scheduled for a defined result); guard denominators that
  can be zero.
- **Huge integer literals can wrap.** Literal arithmetic is
  evaluated in C at 32-bit width today (`2000000000 + 2000000000`
  wraps); put big values in `long` variables first. Also a
  known issue.
- **Removing from a typed map is broken today.** `map.remove()`
  fails to compile for typed maps (a known codegen bug on the
  findings list). The untyped legacy `map` removes fine.
- **A missing map key is not an error you must handle.** It
  reads as the zero value. Check `ERR.set()` only when you
  need to distinguish "absent" from "present and zero".

## 17. Where to go next

- [`examples/`](examples/) — runnable demos: arrays, strings,
  maps, files, ownership, and a combined tour
  (`examples/come_demo.co`).
- [`docs/Come_spec.md`](docs/Come_spec.md) — the full language
  specification.
- [`docs/Come_types.md`](docs/Come_types.md) — every type and
  its methods, in one table.
- [`docs/Come_string.md`](docs/Come_string.md),
  [`docs/Come_array.md`](docs/Come_array.md),
  [`docs/Come_map.md`](docs/Come_map.md),
  [`docs/Come_std.md`](docs/Come_std.md) — the full reference
  for each part of the library.
- [`docs/grammar.ebnf`](docs/grammar.ebnf) — the grammar, if
  you want the exact syntax.
- [`DEVELOPER.md`](DEVELOPER.md) — if you'd like to work on
  Come itself.
- [`LLM_PROMPT.md`](LLM_PROMPT.md) — using an AI assistant?
  Paste that prompt into it first; it teaches the model to
  write correct Come instead of plausible C.
