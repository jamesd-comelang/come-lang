# Come — Developer Guide

This guide is for people working **on** Come: building the compiler,
changing the language, fixing bugs, or reviewing changes. For the
language itself, the normative documents are
[`docs/Come_spec.md`](docs/Come_spec.md) (the specification),
[`docs/grammar.ebnf`](docs/grammar.ebnf) (the grammar), and the
per-type references ([`docs/Come_types.md`](docs/Come_types.md) is the
summary table and the authority when documents disagree). This guide
explains how the project is built, how the implementation is
organized, and how changes are made.

---

## 1. Project overview

### What Come is

Come is a small, compiled programming language that transpiles to C.
It is built around three ideas:

1. **Memory without a garbage collector and without manual
   `malloc`/`free`.** Every allocation lives in a hierarchical
   *context* (an allocation tree in the style of talloc). Buffers
   die with their owner; `resize()` is the only allocation
   primitive; an owner can be a scope, a module, or another
   variable.
2. **Value semantics where they are cheap, explicit sharing where
   they are not.** Structs are values. Arrays, strings, and maps
   are headers over shared data blocks, with snapshot semantics on
   growth. Mutation through a reference is spelled `&` at both
   ends.
3. **Errors as values the C way, minus the pain.** A single global
   `ERR` object backed by `errno`: fallible operations set it,
   nothing throws, and a missing map key or an out-of-range index
   is a defined result (zero value + `ERR` set), never a crash or
   an exception.

Coroutines are built in: stackful, cooperative, with typed
channels (`link`s), isolation from module state enforced by the
compiler.

### Design principles

- **Costs are visible.** No hidden allocations behind innocent
  syntax, no implicit numeric narrowing, no deep copies behind
  plain assignment (`.dup()` is the copy, and it says so).
- **The compiler tells you the rule and the fix.** Diagnostics are
  written in Come terms ("a reference parameter needs `&variable`"),
  never leaked C errors, wherever the compiler knows the rule.
- **One way, documented once.** The spec holds syntax and core
  semantics plus a pointer; each type has exactly one full
  reference document; `Come_types.md` is the summary table.
- **Deliberate decisions stay decided.** Several choices are
  unusual on purpose (byte is signed, string `==` is identity,
  `m[key]` does not throw). They are recorded in §8 of this guide
  and are not re-litigated by drive-by changes.

### Maturity

- **v1.0.0** was released on 2026-10-22 (tag `v1.0.0`, Debian
  package attached to the GitHub release).
- **v1.1.0 is in development** on `main` (split-header buffers,
  struct references, the edge-case sweep, the developer
  commands). It is not tagged or released yet.
- The language is usable and fully self-tested: 57 Come-language
  conformance tests, nine C unit suites, and a set of shell
  end-to-end suites (spec, grammar, error messages, install,
  CLI) all run under one command (`make test`).
- It is still a one-maintainer language. Known limitations are
  listed honestly in §8; the canonical open-issues list is the
  COIL-137 findings list referenced there.

### Supported platforms

- **Linux, x86-64** is the supported platform: the compiler is
  built and tested there, and the Debian package targets
  Ubuntu/Debian amd64. The driver locates itself through
  `/proc/self/exe`, so other Unixes are not currently supported,
  even though the generated C is portable C11.

---

## 2. Getting started

### Prerequisites

- `gcc` and `make` (the compiler checks for gcc and stops with a
  clear message if it is missing).
- `git` and, for pushing, the `gh` CLI authenticated to the
  repository.
- Optional, only for `come test --cover`: `gcov`. Only for
  `come test --race`: ThreadSanitizer support in gcc and,
  ideally, `setarch` (see §6).

### Building the compiler

```sh
make            # builds build/come, the runtime, and the examples
make build/come # just the compiler binary
make clean      # removes build artifacts and the transpile cache
```

The build has two stages: `src/` builds the compiler and the
runtime objects into `build/`, then the freshly built compiler
compiles the standard library modules (`src/std`, `src/string`)
and the examples. That means the compiler is always exercised by
its own build — a compiler change that breaks codegen usually
breaks the build immediately, which is intentional.

### Running a program

```sh
come build hello.co        # -> ./hello (or -o <path>)
come run hello.co -- args  # build to a temp binary and run it
come check hello.co        # compile only: no link, no run
```

Every C compile and link the driver runs honors a `COME_CFLAGS`
environment variable with extra flags (for example
`COME_CFLAGS="-g" come build x.co`).

### The development loop

Most compiler work follows this loop:

1. Reproduce or specify the behavior with a small `.co` probe
   file **outside** the tree (for example under `/tmp`).
2. Change the compiler (usually `src/core/codegen.c` or
   `src/core/parser.c`).
3. `touch` the file you edited, then `make build/come` (see the
   stale-binary warning below).
4. Run the probe, then the focused test suites, then the full
   `make test` before committing.

> **Stale-binary warning.** If a source file is edited and
> rebuilt within the same second, `make` can consider the
> compiler up to date and keep the old binary; a stale compiler
> then produces confusing results. `touch` edited sources (or
> `make clean`) before trusting a rebuild. Relatedly, the driver
> caches transpiled C under `./.ccache` keyed by source path;
> after a *failed* build the cache can hold partial output —
> `rm -rf .ccache` when generated C looks wrong. The compiler's
> own mtime is part of the cache key, so a genuinely rebuilt
> compiler retranspiles everything it touches.

### Development environment notes

- Editor: anything; run `come fmt` on `.co` files you touch
  (§6) rather than hand-formatting.
- The repository is the compiler's own test fixture: standard
  library sources live under `src/`, so a compiler built from
  this tree treats them as *system modules* (which is what
  allows them to use `extern "C"`; see §3).

---

## 3. Language semantics

This section is a working summary. The spec is normative;
section numbers below refer to `docs/Come_spec.md`.

### Syntax in brief (§2, §10, §11, §14–§16)

- Files begin with `module <name>`. Newlines terminate
  statements; `;` separates statements on one line.
- Literals: array literals use braces (`{1, 2, 3}`, never
  `[...]`), map literals likewise (`{"a": 1}`), numbers may
  carry digit separators (`10'000'000`). Float literals have
  no exponent form.
- `var` declares with type inference; the inferred type locks
  at the first assignment and later mismatches are errors.
- Control flow: `if`/`else`, `while`, C-style `for`, and
  `switch` — which does **not** fall through unless the case
  ends with the `fallthrough` keyword. `break`/`continue`
  bind to the innermost loop.
- Two parser rules are line-sensitive: postfix `++`/`--` and
  trailing closures attach to the line they end, and a line
  beginning with `&(struct T)` is always a reference
  declaration, never a bitwise-`&` continuation. Keep both
  in mind before changing expression parsing.
- `alias` defines type aliases (and grouped/macro forms);
  `alias` is a keyword.

### Types (§6)

| Kind | Types | Notes |
|---|---|---|
| Integers | `byte` (signed!), `short`, `int`, `long` | `int` is 64-bit |
| Floats | `float`, `double` | casts truncate toward zero |
| Other scalars | `bool`, `wchar` | `wchar` is a Unicode codepoint |
| Buffers | arrays `T[]`, `string`, maps `map{K, V}` | §4 of this guide |
| Aggregates | `struct`, `union` | value semantics, below |
| Concurrency | `link` | §3, coroutines |
| Functions | first-class only as arguments | `fn` types are post-v1 |

- **No implicit narrowing** (§6.8): assigning a wider integer
  to a narrower type is a compile error; write the cast
  (`byte(200)`). Literal constants that do not fit the target
  are rejected the same way.
- **Arrays**: dynamic (`int a[]`) or fixed (`int f[3]`,
  zero-filled, never grow). Literals are expressions.
- **Strings** are immutable and character-first: `.length()`
  counts characters, `.bytesize()` counts bytes, `s[i]`
  yields the `wchar` codepoint at character index *i* (a
  linear scan, deliberately). Embedded NULs survive inside
  Come; the C boundary is NUL-terminated.
- **Maps**: in the current implementation, typed maps support
  `map{string, int}` and `map{string, string}`; see §8 for
  the gap between that and §6.6. A missing key reads as the
  value type's zero with `ERR` set to `ENOENT` — it never
  throws. The untyped legacy `map` is string-valued.
- **Every buffer and struct** answers the universal methods
  (§7.4): `.length()`, `.bytesize()`, `.owner()`, `.chown()`,
  `.dup()`, `.type()` — with two current exceptions recorded
  in §8 (maps have no `.bytesize()`; struct `.dup()`).

### Functions (§8)

- Signatures are ordinary (`int add(int a, int b)`); multiple
  results come back as tuples, destructured at the statement
  level (`(a, b) = colink()`).
- Functions can be passed as arguments (the `fn:` declarator
  form); they cannot be stored, returned, or put in fields
  in v1.
- The four entry points are `int main()`,
  `int main(string args[])`, `void main()`,
  `void main(string args[])`; `args[0]` is the program name.
- Methods are functions named `Type.name()` with an implicit
  `self`; a method call mutates through `self` explicitly.
- `extern "C"` declarations are allowed **only in system
  modules** (sources under the compiler tree's `src/` or the
  installed `lib/modules/`), never in user code (§13).

### Structs and references (§6.2)

- **Structs are values.** Assignment and pass-by-value copy
  the fields. A buffer field copies its *header*, not its
  contents — element writes are shared, resizes are not
  (this is the §7.2 buffer rule composed with copying).
- A typed literal is a value anywhere:
  `var a = struct Vec3{1, 2, 3}`, including as a method
  receiver.
- **References are explicit and non-owning:**
  `&(struct T)` declares a reference parameter or local;
  `&x` takes a reference at the call site (`x` must be a
  variable, field, or element — never a temporary). Member
  access is `.` through values and references alike; there
  is no `->`. A reference variable passes onward without a
  second `&`; assigning a value to it assigns through,
  assigning `&x` rebinds it; a function that declares the
  struct by value may `return` a reference and returns the
  referent's value.
- Misuse is a compile error naming the fix: missing `&`,
  stray `&`, wrong struct type, `&` of a temporary, `&` of a
  reference, an uninitialized reference variable.
- Scope limits: references are to structs only. No reference
  fields, no reference return types, no reference parameters
  on coroutines, no arrays of references. A bare `&` away
  from typed call sites keeps its C-interop meaning (the
  standard library's `scanf`-family out-parameters predate
  the feature).
- References do not own and do not extend the referent's
  lifetime; the compiler does not track dangling references.

### Modules (§3)

- A file is a module; all `.co` files in one directory with
  the same module name form a **directory module**. Its
  alphabetically first file is primary and defines the
  module's state and context.
- Resolution order for `import X` (§3.2): beside the
  importing file (`X.co`, then the `X/` directory), then
  under `modules/`, then under the project `src/` tree, then
  the system depot next to the compiler. `import X as ...`
  forces depot resolution.
- Modules may define `init()`/`exit()` hooks (also spelled
  `module_init`/`module_exit`). Initialization runs at most
  once, dependencies first; exits run in exact reverse
  order, via a registry in the memory runtime.
- Export visibility is enforced for function calls across
  modules (§3.4): calling a non-exported function is an
  error. Constants and types are **not** yet namespaced in
  generated C — `geom.PI` / `geom.Point` do not resolve;
  that gap is tracked in §8.

### Errors (§12)

- One global object: `ERR.no()` (the errno value),
  `ERR.str()` (the message), `ERR.set()` — a boolean
  *predicate*, true when an error is pending — and
  `ERR.fatal(msg)`, which halts in every build.
- Defined failure results instead of exceptions: a missing
  map key is the zero value with `ENOENT`; an out-of-range
  read is 0 with `ERANGE`; reading a drained, closed link is
  an empty buffer with `EPIPE`-family semantics on write.
- Since COIL-139 every error is also recorded — code,
  message, and the source location where it was set — in a
  fixed per-coroutine chain: `ERR.no(i)` / `ERR.str(i)` /
  `ERR.where(i)` read entry *i* (default 0, the root
  cause), `ERR.depth()` counts entries, an out-of-range
  index returns the zero value and records a new error,
  and `ERR.fatal` prints the message with the root
  location. A successful operation still does not clear a
  previously set error.

### Concurrency (§9)

- `coroutine name(args) { }` declares; `spawn name(args)`
  starts one. Coroutines cannot be called directly, cannot
  return a value, and only coroutines may be spawned.
- **Isolation is compiler-enforced**: module variables are
  not visible inside a coroutine. Buffers passed to `spawn`
  change owner to the coroutine; values are copied.
- `colink()` creates a channel pair of `link` ends.
  `.write(buf)` moves the buffer into the channel;
  `.read()` blocks until data arrives and takes ownership;
  after the peer closes and the channel drains, reads
  return an empty buffer, never null. `.close()` closes an
  end; a coroutine's links close automatically when it
  ends. `wait(...)` blocks until any of the given links is
  readable.
- Scheduling is cooperative on one OS thread (a stackful
  `ucontext` engine, 256 KiB stacks): a coroutine yields on
  an empty read or an explicit `yield`. Blocking C calls
  stall the whole scheduler — that is by design and worth
  remembering before calling one inside a coroutine.
- Deadlock (every coroutine blocked) is detected and ends
  the program with an error on stderr.

---

## 4. Memory model

### Allocation trees

Every allocation belongs to a **context**; contexts form a
tree (the design follows talloc's model, implemented in
`src/mem/memory.c` since COIL-132 — there is no external
allocator dependency). Freeing a context frees its whole
subtree. There is no `alloc()`/`free()` in the language:

- `resize(n)` is the only allocation primitive. `resize(0)`
  yields a valid empty buffer, never null.
- Scopes are the default roots of the tree: a buffer created
  in a scope dies when the scope exits. Modules own a
  context that lives for the program's duration.
- **An owner can be another variable.** `.chown(owner)`
  re-parents a buffer (and its subtree) to any longer-lived
  owner — a scope, a module, or another buffer variable —
  for example re-parenting a request-scoped buffer to the
  connection that outlives it: `req_buf.chown(conn)`.
- When a buffer is returned from a function or passed to
  `spawn`, ownership transfers (the runtime *steals* the
  data block into the receiving context). The exact spec
  wording for this is the open §7.2 question in §8; the
  behavior is pinned by tests.

### Buffers: one design (arrays, strings, maps)

All three buffer kinds share one layout (COIL-135): a
24-byte **header kept inline in the variable** —
`{ data pointer, count, capacity, owner context }` — and a
single heap data block owned by the context. Consequences:

- **Assignment copies the header and shares the data
  block.** Writes through either variable are visible in
  both, while they share the block.
- **Growth forks.** `resize()`, `append()`, or a map insert
  that outgrows capacity allocates a *fresh* data block for
  the growing variable; the old block is never moved or
  freed early, so an alias keeps a valid snapshot of the
  old contents and length. An alias is never left dangling
  by growth. (For maps, the fork happens at the first
  rehash after the alias is made.)
- **Parameters are header copies.** A callee that grows a
  buffer parameter changes only its own header; the
  caller's length does not move. Element writes within
  capacity are shared. Return the buffer to hand a grown
  value back.
- **Fixed arrays** are headers over an exactly-sized block;
  the compiler rejects growth operations on them.
- **Out-of-range access is defined**: reads (any index
  outside `0..length`) yield the zero value and set `ERR`
  to `ERANGE`, served from a shared zeroed slot; writes to
  out-of-range indices are discarded. Indices are never a
  crash (integer division by zero currently is — §8).

### Copying: `.dup()` depths

`dup` takes an optional depth, and the number is literal:

- `.dup(0)` — a reference copy: new header, same data
  block, same owner as the origin. Identical to assignment.
- `.dup(1)` — a fresh top-level block; nested children
  (elements that are themselves buffers) are shared.
- `.dup()` — a full deep copy: fresh blocks at every level.

Struct assignment copies field values; for a deep copy of a
struct with buffer fields, `dup` semantics are the open
item in §8 — today `.dup()` on a struct does not work (see
the limitations).

### Strings

Strings share the header design but are **immutable**:
every transformation allocates a fresh block in the
source's context, so string aliasing questions mostly
reduce to identity. `==` between strings tests buffer
identity when both sides are variables and content against
a literal (see §8 — this hybrid is deliberate); use `.cmp()`
for content comparison between two variables.

### Lifetime rules, summarized

1. A buffer dies when its owner context dies — scope exit,
   module teardown, or the death of the variable that owns
   it, whichever the current owner is.
2. Aliases do not extend lifetime. An alias that outlives
   its owner's death dangles; the compiler does not catch
   this today (§8). Growth can never dangle an alias, but
   owner death can.
3. Return values and `spawn` arguments are re-parented, so
   the idiomatic "build it locally, return it" is safe.
4. References (`&(struct T)`) never own and never extend
   lifetime — see §3.
5. Allocation failure is fail-stop: the runtime aborts.
   There is no out-of-memory path through `ERR`.

---

## 5. Compiler architecture

### Source layout

```
come-lang/
  src/
    core/       the compiler: lexer.c, parser.c, codegen.c,
                come_compiler.c (the driver/CLI), utils.c
    include/    shared headers (come_array.h, come_string.h,
                come_map.h, mem/talloc.h, ...)
    mem/        the context allocator (memory.c)
    array/      array runtime          map/    map runtime
    string/     string runtime         coro/   coroutine + link runtime
    std/        the std module, written in Come
    string/     (module) string toolbox, written in Come
  docs/         spec, grammar, per-type references, ROADMAP
  examples/     runnable examples (built and run by the suite)
  tests/        C unit suites and the shell end-to-end suites
  packaging/    the Debian package builder
  build/        all build output (compiler, objects, dist)
  .ccache/      transpiled C cache, per working directory
```

The compiler is about 10,000 lines of C, half of it the code
generator. The driver, parser, and codegen are separate
translation units linked into one binary, `build/come`.

### Compilation pipeline

1. **Parse** (`parser.c`): source → AST. The parser also
   registers module exports and directory-module symbols so
   codegen can enforce visibility and name rules.
2. **Generate C** (`codegen.c`): one `.co` file → one
   `.co.c` file under `.ccache/`, with `#line` directives
   mapping every generated region back to its `.co` line
   (this is what lets C diagnostics and `come test --cover`
   speak in `.co` lines). Functions are name-mangled as
   `come_<module>__<name>`; module state lives in a
   per-module context.
3. **Compile C** (the driver shells out to `gcc -c`), with
   the include paths resolved relative to the executable,
   so the same driver works from the build tree and from an
   installed layout.
4. **Link** against `libcome.a` (the runtime: memory,
   array, map, string, coroutine/link, and the compiled
   std/string modules) plus `-ldl`.

Incremental builds key on source mtime, cached-C mtime, and
the compiler binary's own mtime — a rebuilt compiler
retranspiles everything it previously generated. Object
files are keyed by basename under `build/`; modules are
compiled by recursion over imports (the driver walks the
import graph from the entry file; a directory module's
files are compiled as one group with the primary file
defining the singletons).

### The runtime

- **Memory** (`src/mem/memory.c`): the hierarchical context
  allocator behind everything — contexts, steal, realloc
  within a context, and the module init/exit registry.
  `come_ERR` support also lives here: `errno` is the code
  store, and a message slot holds the specific text that
  `ERR.str()` prefers when its code matches.
- **Buffers** (`src/array`, `src/map`, `src/string` plus the
  headers in `src/include`): the split-header operations —
  growth always allocates a fresh block; indexing goes
  through macros that route out-of-range access to the
  shared zero slot and set `ERR`.
- **Coroutines** (`src/coro`): a stackful `ucontext`
  scheduler (256 KiB stacks, round-robin, per-coroutine
  context slot) and the link implementation — buffers cross
  links as boxed header cells whose data is stolen into the
  receiving context; end-of-data yields an empty header
  over a real zeroed block, so receivers never see null.

### Build system

- The top-level `Makefile` orchestrates: `src/Makefile`
  builds the compiler objects (an explicit object list —
  adding a compiler source file means adding it there),
  module Makefiles build the runtime pieces, and the
  compiler then builds the Come modules and examples.
- `make dist-local` assembles the installed layout
  (`bin/come`, `lib/libcome.a`, `lib/modules/*.co`,
  `include/`); the compiler is *exe-relative*: it finds its
  headers, library, and module depot from its own path,
  which the clean-room install test exercises with a
  scrubbed environment.
- `packaging/build_deb.sh` produces the `.deb`.

---

## 6. Testing and debugging

### The one command

```sh
make test
```

From a clean tree this builds everything and runs, in
order: the C unit suites (`tests/run_tests.sh`: lexer,
parser, codegen, string, mem, out-of-range, std, array,
coroutine), the shell end-to-end suites (directory-module
project, extern, proc, coroutines, spec conformance,
grammar pinning, the top-20 error messages, the CLI
suites, the clean-room install test, the README facts
check), the **`.co` sweep** — every `src/**/t/*.co` file,
auto-discovered — and the examples as smoke tests. Anything
failing stops the run. Current size: 57 `.co` tests.

### Testing a project (and this repo) with `come test`

```sh
come test [root]            # every .co under any t/ dir
come test --bench           # mean of 5 runs per test
come test --race            # ThreadSanitizer build
come test --cover           # per-file .co line coverage
```

Each test builds in an isolated scratch directory (its own
transpile cache — no basename collisions, no pollution of
the project's `build/`), with a 30-second timeout. `--race`
instruments the generated code via `COME_CFLAGS`; the
prebuilt runtime is not instrumented. On machines where
ThreadSanitizer refuses to start under the kernel's ASLR
layout, test children are run under `setarch -R` when it is
available. `--cover` runs gcov over the generated objects
and attributes lines to the `.co` sources via `#line`.

### Adding a test

- **Language behavior** → a `.co` file in the matching
  `src/<area>/t/` directory (core language: `src/core/t/`).
  Style: print `FAIL: ...` and `return 1` on failure, print
  a `PASS: <name>` line and `return 0` at the end; the
  harness checks the exit code. Name files
  `NN-<topic>.co` following the existing numbering.
- **Compiler internals** (lexer/parser/codegen output
  shapes) → the C suites under `tests/`, in the existing
  `gen_ok`/`gen_fail` and text-pinning style.
- **Diagnostics** → `tests/run_errors_test.sh`: a case is a
  small broken program plus the message fragment it must
  produce. The bar: the build fails *and* the first
  diagnostic names the rule and the fix in Come terms.
- **End-to-end behavior** → a shell suite in `tests/`,
  wired into `tests/run_tests.sh`.

House rule: a story's acceptance is only met when it has
**both** a C-side test and a `.co` test where both apply —
and the claim "tests pass" is verified with a clean
`make test`, never asserted from a partial run.

### Debugging compiler changes

- **Read the generated C.** `come genc file.co -o out.c`
  (any `-o` path works, including a bare filename). The
  `#line` directives keep it navigable; most codegen bugs
  are visible as the wrong C shape for one construct.
- **Verbose driver**: `come build file.co -v` prints every
  subcommand the driver runs.
- **Beware the two caches** (§2): a same-second edit/rebuild
  can leave a stale binary — `touch` the source — and a
  failed build can poison `.ccache` — `rm -rf .ccache` when
  output looks stale. When generated C "ignores" your
  codegen change, it is almost always one of these two.
- **Bisect against history**: `git worktree add` at an older
  commit builds fine side by side with the main tree; the
  project has repeatedly used this to prove a bug
  pre-existing rather than a regression.
- A probe that segfaults the compiler is a finding, not an
  inconvenience: shrink it, and pin the fixed behavior (or
  the named error) in the suites above.

### Formatting

```sh
come fmt <file.co|dir> [...]
```

`come fmt` never adds or removes a line (the parser has
line-sensitive rules), canonicalizes indentation and token
spacing within a line, and passes literals and comments
through byte-for-byte. It is idempotent; formatting the
whole tree and diffing the *generated C* is a sound way to
prove a formatter change is semantics-preserving.

---

## 7. Contribution workflow

### How work is organized

- Work happens in **stories**, numbered `COIL-<n>` in
  `docs/ROADMAP.md`, grouped under the release they belong
  to. Story titles are value-centered: they name what the
  user of the language gains ("Structs are values,
  references are explicit"), not the mechanism.
- The roadmap row is the contract: problem, spec sections
  touched, acceptance criteria. A story is done when its
  acceptance is met *and verified*, its row is checked off,
  and the suite is green.
- Small documentation/admin changes ride under `COIL-04`.

### Making a change

1. Branch or work on `main` per the maintainer's current
   practice (history to date is linear commits on `main`).
2. Write the failing test first where the change has
   observable behavior — a `.co` test for semantics, a C
   test for compiler internals, an errors-test case for
   diagnostics.
3. Implement. Prefer the smallest change that satisfies the
   acceptance; do not fold unrelated fixes into a story
   (file them instead).
4. Run the focused suites, then a clean `make test`
   (0 warnings is the bar — the build has been warning-free
   since COIL-135 and stays that way).
5. Update the docs the change touches (below) in the same
   commit.
6. Commit with the story prefix: `[COIL-<n>] <title>`, with
   a body that states what changed, what was verified, and
   any deliberate non-changes. Push once green.

### Coding conventions

- C code follows the existing style of the file it lives in
  (the compiler sources are the reference); keep functions
  small, name things in Come terms in user-visible strings.
- Diagnostics: state the rule and the fix, in the user's
  vocabulary, e.g. "a reference parameter (&(struct Vec3))
  needs a reference: pass &variable". Never let a raw C
  compiler or linker error be the first thing a user sees
  for a mistake the compiler understands.
- No new external dependencies for the compiler or runtime
  without a roadmap discussion; the allocator, coroutine
  engine, and libraries are all in-tree on purpose.
- `.co` sources (standard library, tests, examples) are
  formatted with `come fmt`.

### Documentation updates

The docs are layered, and a change updates exactly its
layer:

- **Syntax / core semantics** → `docs/Come_spec.md`, kept
  brief, with a pointer to the full reference.
- **A type's full behavior** → its dedicated doc
  (`Come_array.md`, `Come_map.md`, `Come_string.md`,
  `Come_std.md`); the spec links to it.
- **Summary tables** → `docs/Come_types.md`, which wins
  when documents disagree.
- **Grammar** → `docs/grammar.ebnf`, pinned to the parser
  by `tests/run_grammar_test.sh`; a syntax change that
  does not update the grammar fails the suite.
- **README** states current-version facts only; a test pins
  a set of load-bearing terms and forbids superseded ones.

### Review expectations

A change is reviewable when: the story row describes it,
the tests demonstrate it (both kinds, per §6), the full
suite is green from clean with 0 warnings, the docs are
updated in the same commit, and the commit message says
what was deliberately *not* done. Design-changing work
(anything touching §8's accepted decisions or the open
questions) needs the maintainer's explicit decision first —
discussion is cheap, and several areas are one decision
away from a ready implementation.

---

## 8. Design decisions and roadmap

### Accepted decisions (v2, do not re-litigate)

| Decision | Where |
|---|---|
| `ERR` is a single errno-backed object; `ERR.set()` is a predicate | spec §12 |
| Memory is context trees; `resize()` is the only primitive; owners may be variables | spec §7 |
| Split-header buffers; growth forks aliases into snapshots | COIL-135 |
| Buffer parameters are header copies (length changes do not propagate) | spec §7.2 |
| Structs are values; references explicit, non-owning, structs-only | COIL-136 |
| No implicit numeric narrowing; `byte` is **signed** | spec §6.8 |
| String `==` is identity between variables, content against a literal | spec §14 |
| Map miss is a zero value + `ENOENT`, never a throw | spec §6.6 |
| `alias` includes the grouped/macro forms | COIL-127 |
| Coroutines are isolated from module state by the compiler | COIL-122 |
| `extern "C"` only in system modules | spec §13 |
| No interfaces, no `fn` types beyond arguments, no struct map keys in v1 | spec §17 |

### Unresolved questions (the maintainer's calls)

1. **Spec §7.2 "Promotion" wording.** The spec text
   describes compiler-managed buffer promotion; the
   implementation re-parents buffers on return/spawn (the
   behavior is test-pinned). Either (A) the text is made
   precise about the context model, or (B) the promotion
   passage is rewritten as reject-only. Every other part of
   §7.2 has been written around this paragraph.
2. **§3.4 namespacing.** Module constants and types are not
   namespaced in generated C, so cross-module
   `geom.Point`/`geom.PI` do not resolve, struct values
   cannot be materialized by importers, and cross-module
   reference calls are unchecked. This is now on the
   critical path for two features (below and module
   instances).
3. **Module instances.** Discussed 2026-10-22: namespace-only
   `import` plus `.create()` for instances, with
   statefulness computed transitively over the call graph
   and namespace-export violations reported as authoring
   errors. Undecided: default instance or none, exit as a
   lifetime hook or a method, language feature vs the
   already-working pattern (a state struct plus functions
   taking `&(struct State)`), and calls on an exited
   instance.
4. **v1.1.0 release.** The v1.1.0 stories (COIL-135–138)
   are complete on `main`; tagging and releasing are the
   maintainer's go-ahead.

### Known limitations

The canonical list is the **COIL-137 findings list**
(29 items, kept with the maintainer's working files;
`come-edge-case-findings.md`). The load-bearing ones:

- **Typed-map `.remove()` is broken in codegen** (emits a
  call to an unrelated runtime function; cannot build).
- **Integer `/` and `%` by zero crash** with SIGFPE;
  float→int out-of-range casts and signed overflow are C
  undefined behavior; there is no `-fwrapv` yet.
- **Integer literal expressions compute at C 32-bit
  width** (`2000000000 + 2000000000` wraps) while `int`
  variables are 64-bit.
- **Structs are second-class payload types**: arrays of
  structs do not compile; nested-array element access
  fails in generated C; typed maps reject struct values;
  structs cannot cross `spawn`/`link` boundaries; fixed
  array fields in structs silently lose writes; struct
  `.dup()` generates a call to a method that does not
  exist; universal methods fail on unions.
- **Typed maps** support only `map{string, int}` and
  `map{string, string}` (spec §6.6 promises more key
  types; buffer values are impossible in any map).
- **`ERR` never clears on success** (records are
  per-coroutine since COIL-139); **use-after-move** (using a buffer after
  `link.write`/`spawn` took it) is not caught by the
  compiler; a plain alias can dangle if its owner dies
  first (no lifetime analysis).
- **Float literals have no exponent form**; the `for`
  update clause accepts only `++`/`--`-style updates;
  `\xNN` escapes followed by a hex-digit character are
  corrupted by C's greedy escape rule; `var` inference
  mistypes a string `+` result as a string list and does
  not know `substr()`'s return type; some type errors
  still surface as raw C diagnostics; coroutine stack
  overflow is a crash, not a report; `grammar.ebnf`
  predates the COIL-136 reference productions.

### Planned features (post-v1 backlog, from the roadmap)

- Interfaces (explicit implementation declarations only,
  if ever) — spec §17.
- Function values beyond arguments: variables, struct
  fields, returns (`fn` types) — §8.3.
- Struct keys for maps — §6.6.
- `wait()` timeouts — §9.2.
- `reserve(n)` capacity control for arrays —
  `Come_array.md` §3.
- OS-thread support via system modules beyond the v1 FFI
  surface — §13.
- The natural next language stories, from the findings
  list: structs as payload types (one story closing the
  array/map/spawn/link gaps together), struct `.dup()`,
  and the hardening batch (defined divide-by-zero and
  cast behavior, per-coroutine `ERR` with defined
  clearing, a coroutine stack guard page).
