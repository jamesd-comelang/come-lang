# Come

**Come (C Object and Module Extensions)** is a C-inspired systems
language that transpiles to C. It keeps the C mental model — static
types, explicit control flow, no hidden machinery — and removes the
parts that bite: no raw pointers in user code, no implicit narrowing,
no fallthrough by default, and memory that is owned, hierarchical,
and freed with its owner.

**Status: v1.0.0.** The specification, the published grammar, and the
compiler agree, and one command proves it: `make test` (see
[Testing](#testing)).

## Quickstart

Install the Debian package (Ubuntu/Debian, amd64):

```sh
sudo dpkg -i come_1.0.0_amd64.deb
```

or build from source (needs gcc and make):

```sh
make            # builds build/come and the examples
```

Write `hello.co`:

```text
module main
import std

main() {
    std.out.printf("Hello world!\n")
}
```

Build and run it:

```sh
come build hello.co
./hello
```

## The language in brief

- **Modules are the only compilation unit.** Each file declares one
  `module`; a module is one file or a directory of files. Modules
  have private memory contexts and optional `module_init` /
  `module_exit` hooks that run in dependency order.
- **Visibility is explicit.** A module with an `export (...)` list
  exposes exactly those names; calling anything else from outside
  is a compile error. A module with no export list is open.
- **No raw pointers in user code.** Arrays, strings, and maps are
  headered buffer objects:

  ```text
  [ capacity | count | data... ]
  ```

  Every buffer answers `.length()`, `.bytesize()`, `.owner()`,
  `.chown()`, `.dup()`, and `.type()`.
- **Ownership is hierarchical.** Buffers live in a memory context
  and die with their owner; `.chown()` re-parents a buffer,
  `.dup()` deep-copies, `.dup(0)` makes a reference copy.
  `resize(n)` is the only memory primitive — there is no
  `alloc`/`free` in user code. Fixed arrays (`int a[5]`) stay
  fixed; dynamic arrays (`int a[]`) can resize.
- **Strings are immutable and character-first.** `s[i]` is the
  i-th character (a `wchar`), `.length()` counts characters,
  `+` concatenates.
- **Static typing with local inference.** `var` infers from the
  first assignment and locks. There is **no implicit narrowing**:
  storing a `double` in an `int` is a compile error unless you
  write the cast, `int(x)`.
- **Errors are values, not exceptions.** A global `ERR` object
  reports failure: `ERR.no()`, `ERR.str()`, `ERR.set()` is a
  boolean "did the last call fail" predicate, and `ERR.fatal(msg)`
  stops the program.
- **Multiple returns via tuples**, destructured on assignment:
  `(sum, cmp) = add_n_compare(5, 3)`.
- **Coroutines and links** (spec §9). `coroutine` declares a
  cooperative task, `spawn` starts one, and `colink()` channels
  pass buffers — ownership included — between them. A coroutine
  sees only its own locals and parameters.
- **Control flow is C-shaped, with the traps removed.** `switch`
  does not fall through unless a case ends in `fallthrough`.
  A newline ends a statement; `;` separates statements on one
  line.

System modules (the compiler's own `std`, `string`, and friends)
are the one place `extern "C"` may appear; they are implemented
in C and declare their Come surface in `.co` files.

## Documentation

- [Language specification](docs/Come_spec.md) — syntax and core
  semantics (§1–§17)
- [Grammar (EBNF)](docs/grammar.ebnf) — the published grammar;
  a conformance test checks it against the compiler
- [Types summary](docs/Come_types.md) — the type table (the
  authority when documents disagree)
- [Arrays](docs/Come_array.md), [Strings](docs/Come_string.md),
  [Maps](docs/Come_map.md), [Standard library](docs/Come_std.md)
- [Project layout](docs/Come_project_layout.md)
- [Roadmap](docs/ROADMAP.md) — how v1.0.0 was built, story by story

## Testing

One command runs the whole conformance suite:

```sh
make test
```

It builds the compiler and examples, runs the C unit suites and
the end-to-end project tests (`tests/run_tests.sh` — including
the spec, grammar, top-20 error-message, and clean-room install
checks), compiles and runs every Come-language test under
`src/**/t/` (auto-discovered — a new test joins the suite just by
being added there), and runs the examples as smoke tests.
Anything failing stops the run.

## License

Apache 2.0 — see [LICENSE](LICENSE).
