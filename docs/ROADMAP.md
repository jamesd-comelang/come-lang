# Come Roadmap — Release Plan

Built from the v2 spec (`Come_spec.md`) and the type/stdlib docs
(`Come_types.md`, `Come_std.md`, `Come_string.md`, `Come_array.md`,
`Come_map.md`). Every story names its spec reference; a story is done
when its acceptance line holds and a test covers it.

Stories are numbered **COIL-100** upward, globally across releases
(next free number: **COIL-133**).

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
| COIL-132 | Replace vendored talloc (LGPLv3) with Come's own memory framework: drop-in swap behind the `src/mem` API (context tree, `steal`, `realloc`); OOM → `ERR`/`ERR.fatal`, never NULL | §7 | L | — | Existing test suite passes unchanged; `src/external/talloc` removed; runtime is license-clean (Apache 2.0) | ☐ |
| COIL-100 | Array literals `{1, 2, 3}`; remove `[...]` literal syntax | §6.4, Come_types | M | — | `{...}` compiles everywhere; `[...]` is a parse error with a helpful message | ☐ |
| COIL-101 | Fixed arrays stay fixed: delete all promotion paths; `resize`/growth on a fixed array is a compile error | §6.4, Come_array §1 | M | COIL-132 | Old promotion test cases fail to compile; fixed arrays pass/return/store unchanged | ☐ |
| COIL-102 | Static maps: parse `map{K, V}`, `{k: v}` literals, typed access | §6.6, Come_map | L | — | `map{string, int}` programs compile; mixed-type literals are compile errors | ☐ |
| COIL-103 | Map runtime semantics + methods: missing key → zero value of `V` + `ERR`; `.has()` `.keys()` `.delete()` `.clear()` | Come_map §3, §6 | M | COIL-102, COIL-132 | Missing-key read sets `ERR.set()`; method table in Come_map.md fully works | ☐ |
| COIL-104 | Struct init `{.x = 5, .y = 10}`; `packed struct` / `align(n) struct` with C-compatible default layout | §6.2 | M | — | Layout matches the equivalent C struct (verified against a C reference in tests) | ☐ |
| COIL-105 | `ERR.set()` and `ERR.fatal(msg)` in the runtime and codegen | §12, Come_std | S | — | `ERR.set()` usable in conditions; `ERR.fatal` halts in all build modes | ☐ |
| COIL-106 | Out-of-bounds semantics: read → zero value + `ERR`, write → dropped + `ERR` (arrays and strings) | Come_array §2, §6.4–6.5 | S | COIL-105 | OOB test cases behave per doc, no crashes | ☐ |
| COIL-107 | No implicit narrowing: semantic check; explicit `T(x)` casts only | §6.8 | M | — | Narrowing assignments are compile errors; lossless widening still implicit | ☐ |
| COIL-108 | Multi-file modules: every `.co` file in one directory declaring the same module forms that module; `module_init()`/`module_exit()` across files | §3.2, §3.5 | L | — | A 2-file module compiles and runs with correct init/exit order; both-layouts-present is an error | ☐ |
| COIL-109 | Functions as arguments in declarator shape (`int cmp(byte a, byte b)`); no `method` type anywhere | §8.3 | M | — | `sort` / `qsort` / `atexit` call sites compile with inline declarators | ☐ |
| COIL-110 | String conformance: immutability enforced, `.length()` only (no `.len()`), character-first indexing | §6.5, Come_string | M | COIL-106, COIL-132 | String mutation attempts are compile errors; string_demo passes with `.length()` | ☐ |
| COIL-111 | `extern "C"` restricted to system modules | §13 | S | — | `extern` in a user module is a compile error | ☐ |
| COIL-112 | Doc fixes: spec §17 "One per file" → directory-based modules; README pillars updated (drop promotion text) | §3.2, §17 | S | COIL-108 | Docs and spec agree with the shipped behavior | ☐ |
| COIL-113 | Conformance test harness: one `.co` test per spec section, wired into `tests/run_tests.sh` | all | M | COIL-100–112 | The suite runs green in one command and grows with every story above | ☐ |

**Exit criteria:** all examples compile and run; conformance suite green;
`packaging/build_deb.sh` produces a v0.7.0 deb.

## v0.8.0 — Standard library completion

Theme: `Come_std.md` and the per-type method tables are fully implemented.

| Story | Title | Spec | Size | Depends | Acceptance | Done |
|-------|-------|------|------|---------|------------|------|
| COIL-114 | `std` FILE API complete per Come_std.md (open modes, `setbuf` empty = unbuffered, `gets` empty at EOF, …) | Come_std | L | v0.7.0 | Every row of the Come_std tables has a working call + test | ☐ |
| COIL-115 | `std.proc`: `exit` / `abort` / `abort_handler` / `atexit` (new signature) / `getenv` / `system` | Come_std | M | COIL-109 | atexit callbacks run in reverse order at exit | ☐ |
| COIL-116 | Array method table complete: `contains` `index` `slice` `sort` `reverse` `set` `cpy` `insert` `remove` `clear` | Come_array §4 | M | v0.7.0 | All Come_array.md methods tested | ☐ |
| COIL-117 | String method table complete (the full Come_string.md list, incl. split/join/trim/format family) | Come_string | L | COIL-110 | All Come_string.md methods tested | ☐ |
| COIL-118 | `ERR.str()` message coverage for every runtime error the stdlib can set | §12 | S | COIL-114–117 | No `ERR` code surfaces with an empty message | ☐ |
| COIL-119 | Stdlib examples: one runnable example per std module, added to `examples/` | Come_std | S | COIL-114–117 | Examples build in CI-style run via `tests/run_tests.sh` | ☐ |

**Exit criteria:** no row in any doc's method table is unimplemented;
examples all pass.

## v0.9.0 — Coroutines

Theme: spec §9, the last big v1 feature. Nothing in the current tree
implements it (0 files reference `coroutine` / `spawn` / `colink`).

| Story | Title | Spec | Size | Depends | Acceptance | Done |
|-------|-------|------|------|---------|------------|------|
| COIL-120 | Runtime: stackful coroutines (stack switching) + cooperative scheduler (ready queue + wait list) | §9.3 | XL | v0.7.0 | Two coroutines interleave deterministically under `yield` | ☐ |
| COIL-121 | `coroutine` declarations + `spawn` statement (parser, AST, codegen) | §9.1 | M | COIL-120 | A spawned coroutine runs to completion | ☐ |
| COIL-122 | Isolation checks: no module-variable access inside coroutines; buffer arguments change owner to the coroutine | §9.1, §7 | M | COIL-121 | Violations are compile errors; ownership moves verified in tests | ☐ |
| COIL-123 | `colink()` and the `link` type: `.read()` `.write()` `.close()`, ownership-moving writes, closed-peer semantics (empty buffer / `ERR`) | §9.2, §6.7 | L | COIL-120, COIL-122 | Ping-pong example passes; write-after-close sets `ERR` | ☐ |
| COIL-124 | `wait(...)`: pause until any of several links is ready or closed; returns the ready link | §9.2 | M | COIL-123 | Multi-producer example passes | ☐ |
| COIL-125 | Lifecycle: coroutine end auto-closes its links and frees its memory; `main` returning ends the program; deadlock detection | §9.3 | M | COIL-123 | Deadlocked program reports an error instead of hanging | ☐ |
| COIL-126 | Coroutine examples + stress tests (many coroutines, link churn) | §9 | M | COIL-121–125 | Examples pass; no leaks under the ownership tests | ☐ |

**Exit criteria:** §9 examples from the spec compile and run as written;
test suite green.

## v1.0.0 — Hardening and release

Theme: v1 is the spec, fully — verified, documented, packaged.

| Story | Title | Spec | Size | Depends | Acceptance | Done |
|-------|-------|------|------|---------|------------|------|
| COIL-127 | Full conformance pass: every spec section has at least one test; gaps become stories or spec errata | all | L | v0.9.0 | Coverage checklist in `docs/draft/TESTING.md` is complete | ☐ |
| COIL-128 | EBNF grammar finalized from the actual parser; `docs/draft/grammar.ebnf` promoted into `docs/` | §16 | M | COIL-127 | Grammar and parser accept the same language on the test corpus | ☐ |
| COIL-129 | Diagnostics pass: error messages name the rule and the fix (esp. narrowing, promotion-removal, isolation) | — | M | COIL-127 | Top-20 error cases reviewed by hand | ☐ |
| COIL-130 | Packaging: deb / rpm / arch rebuilt; version → 1.0.0; release notes from this roadmap | — | S | COIL-127–129 | Clean-machine install runs `hello.co` | ☐ |
| COIL-131 | README + site refresh to v1 facts (modules per directory, no promotion, coroutines in) | — | S | COIL-127 | No pre-v2 claims remain in README | ☐ |

**Exit criteria:** v1.0.0 tagged; packages published; spec, grammar,
and implementation agree.

---

## Post-v1 backlog (explicitly deferred by the spec)

- Interfaces (explicit implementation declarations only, if ever) — §17
- Function values beyond arguments: variables, struct fields, returns (`fn` types) — §8.3
- Struct keys for maps — §6.6
- `wait()` timeouts — §9.2
- `reserve(n)` capacity control for arrays — Come_array §3
- OS-thread support via system modules beyond the v1 FFI surface — §13
