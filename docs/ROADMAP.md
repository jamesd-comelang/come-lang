# Come Roadmap — Release Plan

Built from the v2 spec (`Come_spec.md`) and the type/stdlib docs
(`Come_types.md`, `Come_std.md`, `Come_string.md`, `Come_array.md`,
`Come_map.md`). Every story names its spec reference; a story is done
when its acceptance line holds and a test covers it.

Stories are numbered **COIL-100** upward, globally across releases
(next free number: **COIL-137**).

Story titles state the **value delivered** — who gains what — in one
read. Implementation detail belongs in the Spec and Acceptance columns,
not in the title.

**Baseline: v0.6.0** (tagged) — lexer/parser/codegen pipeline, array /
string / map runtimes with pre-v2 semantics, `ERR` runtime, std basics,
deb packaging. Not yet present: `map{K, V}`, coroutines, several v2
semantics below.

Sizes are relative: **S** small, **M** medium, **L** large, **XL** multi-week.

Mark a story done by flipping its **Done** cell from ☐ to ☑.

---

## v0.7.0 — Spec v2 conformance (core language)

Theme: the compiler matches the spec we just wrote. Coroutines excluded
(they are v0.9.0). COIL-132 lands first, as a pure drop-in swap, so the
semantics stories build on the final memory layer.

| Story | Title | Spec | Size | Depends | Acceptance | Done |
|-------|-------|------|------|---------|------------|------|
| COIL-132 | Come programs ship free of third-party license obligations | §7 | L | — | Existing test suite passes unchanged; `src/external/talloc` removed; runtime is license-clean (Apache 2.0) | ☑ |
| COIL-100 | Programmers learn one literal syntax for every collection | §6.4, Come_types | M | — | `{...}` compiles everywhere; `[...]` is a parse error with a helpful message | ☑ |
| COIL-101 | A fixed array's size is a promise the compiler keeps | §6.4, Come_array §1 | M | COIL-132 | Old promotion test cases fail to compile; fixed arrays pass/return/store unchanged | ☑ |
| COIL-102 | Map type mistakes are compile errors, not runtime surprises | §6.6, Come_map | L | — | `map{string, int}` programs compile; mixed-type literals are compile errors | ☑ |
| COIL-103 | A missing map key gives a safe zero value and a checkable error | Come_map §3, §6 | M | COIL-102, COIL-132 | Missing-key read sets `ERR.set()`; method table in Come_map.md fully works | ☑ |
| COIL-104 | Structs match C layouts exactly, so packed formats and FFI just work | §6.2 | M | — | Layout matches the equivalent C struct (verified against a C reference in tests) | ☑ |
| COIL-105 | Errors are one obvious check away, and a fatal exit can't be compiled out | §12, Come_std | S | — | `ERR.set()` usable in conditions; `ERR.fatal` halts in all build modes | ☑ |
| COIL-106 | Out-of-bounds access reports an error instead of crashing or corrupting memory | Come_array §2, §6.4–6.5 | S | COIL-105 | OOB test cases behave per doc, no crashes | ☑ |
| COIL-107 | Numbers never lose precision silently | §6.8 | M | — | Narrowing assignments are compile errors; lossless widening still implicit | ☑ |
| COIL-108 | A module can grow across files in its directory with no import changes | §3.2, §3.5 | L | — | A 2-file module compiles and runs with correct init/exit order; both-layouts-present is an error | ☑ |
| COIL-109 | Callbacks and comparators are passed without inventing new type syntax | §8.3 | M | — | `sort` / `qsort` / `atexit` call sites compile with inline declarators | ☑ |
| COIL-110 | String length means characters, and strings never change under your feet | §6.5, Come_string | M | COIL-106, COIL-132 | String mutation attempts are compile errors; string_demo passes with `.length()` | ☑ |
| COIL-133 | A function can return a string it built | §7, §6.5 | M | COIL-132 | Returning a derived string (e.g. `return name.repeat(3)`) hands the caller the intact value instead of segfaulting (pre-existing bug, reproduced on the pre-COIL-132 build); `src/mem/t/04-string-ownership.co` extended to cover it | ☑ |
| COIL-134 | A buffer's byte count comes from a name that says bytes | §6.5, §7, Come_array, Come_string | S | — | `.size()` is gone: strings and arrays report bytes via `.bytesize()` (content bytes for strings, count × element size for arrays) and counts via `.length()`; every call site in tests, examples, and docs renamed; all suites pass | ☑ |
| COIL-111 | Raw C stays inside system modules; user code never touches it | §13 | S | — | `extern` in a user module is a compile error | ☑ |
| COIL-112 | The docs never describe a language different from the one shipped | §3.2, §17 | S | COIL-108 | Docs and spec agree with the shipped behavior | ☑ |
| COIL-113 | Every promise in the spec is backed by a test that runs in one command | all | M | COIL-100–112 | The suite runs green in one command and grows with every story above | ☑ |

**Exit criteria:** all examples compile and run; conformance suite green;
`packaging/build_deb.sh` produces a v0.7.0 deb.

## v0.8.0 — Standard library completion

Theme: `Come_std.md` and the per-type method tables are fully implemented.

| Story | Title | Spec | Size | Depends | Acceptance | Done |
|-------|-------|------|------|---------|------------|------|
| COIL-114 | File I/O behaves exactly as documented, edge cases included | Come_std | L | v0.7.0 | Every row of the Come_std tables has a working call + test | ☑ |
| COIL-115 | Programs shut down cleanly: exit callbacks run, environment is readable | Come_std | M | COIL-109 | atexit callbacks run in reverse order at exit | ☑ |
| COIL-116 | Arrays come with the full documented toolbox: search, slice, sort | Come_array §4 | M | v0.7.0 | All Come_array.md methods tested | ☑ |
| COIL-117 | Strings come with the full documented toolbox: split, join, trim, format | Come_string | L | COIL-110 | All Come_string.md methods tested | ☑ |
| COIL-118 | Every runtime error arrives with a readable message | §12 | S | COIL-114–117 | No `ERR` code surfaces with an empty message | ☑ |
| COIL-119 | Each std module can be learned from a runnable example | Come_std | S | COIL-114–117 | Examples build in CI-style run via `tests/run_tests.sh` | ☑ |

**Exit criteria:** no row in any doc's method table is unimplemented;
examples all pass.

## v0.9.0 — Coroutines

Theme: spec §9, the last big v1 feature. Nothing in the current tree
implements it (0 files reference `coroutine` / `spawn` / `colink`).

| Story | Title | Spec | Size | Depends | Acceptance | Done |
|-------|-------|------|------|---------|------------|------|
| COIL-120 | Coroutines are cheap enough to spawn per task — no threads, no locks | §9.3 | XL | v0.7.0 | Two coroutines interleave deterministically under `yield` | ☑ |
| COIL-121 | Concurrent work starts with a single spawn statement | §9.1 | M | COIL-120 | A spawned coroutine runs to completion | ☑ |
| COIL-122 | Coroutines cannot data-race — they share nothing by construction | §9.1, §7 | M | COIL-121 | Violations are compile errors; ownership moves verified in tests | ☑ |
| COIL-123 | Coroutines hand data to each other with ownership attached | §9.2, §6.7 | L | COIL-120, COIL-122 | Ping-pong example passes; write-after-close sets `ERR` | ☑ |
| COIL-124 | A single wait can listen to many channels at once | §9.2 | M | COIL-123 | Multi-producer example passes | ☑ |
| COIL-125 | Coroutines clean up after themselves; a deadlock reports instead of hanging | §9.3 | M | COIL-123 | Deadlocked program reports an error instead of hanging | ☑ |
| COIL-126 | The spec's coroutine examples run as written, even under load | §9 | M | COIL-121–125 | Examples pass; no leaks under the ownership tests | ☑ |

**Exit criteria:** §9 examples from the spec compile and run as written;
test suite green.

## v1.0.0 — Hardening and release

Theme: v1 is the spec, fully — verified, documented, packaged.

| Story | Title | Spec | Size | Depends | Acceptance | Done |
|-------|-------|------|------|---------|------------|------|
| COIL-127 | Every section of the spec is proven by a test — or corrected | all | L | v0.9.0 | Coverage checklist in `docs/draft/TESTING.md` is complete | ☑ |
| COIL-128 | The published grammar accepts exactly what the compiler accepts | §16 | M | COIL-127 | Grammar and parser accept the same language on the test corpus | ☑ |
| COIL-129 | Compiler errors name the rule and show the fix | — | M | COIL-127 | Top-20 error cases reviewed by hand | ☑ |
| COIL-130 | v1.0 installs from a package and runs on a clean machine | — | S | COIL-127–129 | Clean-machine install runs `hello.co` | ☑ |
| COIL-131 | The README describes the language as it actually is | — | S | COIL-127 | No pre-v2 claims remain in README | ☑ |

**Exit criteria:** v1.0.0 tagged; packages published; spec, grammar,
and implementation agree.

---

## v1.1.0 — One buffer design

Theme: arrays, strings and maps share one representation, and a
growing buffer can never leave an alias pointing at freed memory.

| Story | Title | Spec | Size | Depends | Acceptance | Done |
|-------|-------|------|------|---------|------------|------|
| COIL-135 | Aliases survive growth: one split-header value design for arrays, strings and maps | §7 | L | v1.0.0 | Header lives inline in the variable (data pointer, count, capacity, owner context); only the data block is heap-allocated; growth allocates a fresh data block and forks aliases into valid snapshots; C and .co tests pin alias-over-growth for arrays and maps; spec, type docs and README revised | ☑ |

## v1.1.0 — Struct references

Theme: a struct is a value; when a function must mutate a
caller's struct, the reference is explicit at the declaration
and at the call site.

| Story | Title | Spec | Size | Depends | Acceptance | Done |
|-------|-------|------|------|---------|------------|------|
| COIL-136 | Structs are values, references are explicit: &(struct T) parameters and &x call sites | §6.2, §7 | L | v1.0.0 | Struct assignment and by-value parameters copy the fields (buffer fields copy their headers, sharing data); `&(struct T)` declares a non-owning reference parameter or variable, `&x` takes a reference at the call site, `.` accesses members through values and references alike; missing or stray `&` is a compile error naming the fix; references to non-struct types, reference fields, and reference returns are rejected; C and .co tests pin value copies, buffer-handle sharing, mutation through references, and &a/&a aliasing; spec and Come_types.md revised | ☑ |
| COIL-137 | Edge-case conformance sweep: the language's edges pinned by tests, its failures written down | Seven new .co test files (20-edge-numbers through 26-edge-dup) pin verified behavior across numbers, strings, arrays, maps, structs/unions, control flow, and dup depths; every failure, crash, and spec gap found while probing is recorded in the findings list (workspace/your_files/come-edge-case-findings.md, 29 items) instead of being pinned as behavior | ☑ |
| COIL-138 | Developer commands: come test, come fmt, come check, come run | `come test [root]` builds and runs every .co test under any t/ directory below the root (default .), one isolated build per test, per-test PASS/FAIL and a summary, nonzero exit on any failure; --bench times 5 runs per test; --race rebuilds each test in a scratch tree with ThreadSanitizer (COME_CFLAGS) and fails on a sanitizer report; --cover reports .co line coverage per test file via gcov over the generated C (its #line map points back at the .co source; the prebuilt runtime is not instrumented). `come check <file>` compiles (parse, codegen, C) without linking or running. `come run <file> [args...]` builds to a temporary binary, runs it, forwards the arguments and the exit code. `come fmt <files|dir>` reformats in place: line structure preserved (the parser has line-sensitive rules), indentation and spacing canonicalized, string/char literals and comments untouched, idempotent. E2E coverage in tests/run_cli_test.sh; README documents the commands | ☑ |

## Post-v1 backlog (explicitly deferred by the spec)

- Interfaces (explicit implementation declarations only, if ever) — §17
- Function values beyond arguments: variables, struct fields, returns (`fn` types) — §8.3
- Struct keys for maps — §6.6
- `wait()` timeouts — §9.2
- `reserve(n)` capacity control for arrays — Come_array §3
- OS-thread support via system modules beyond the v1 FFI surface — §13
