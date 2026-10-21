# Come Roadmap — Release Plan

Built from the v2 spec (`Come_spec.md`) and the type/stdlib docs
(`Come_types.md`, `Come_std.md`, `Come_string.md`, `Come_array.md`,
`Come_map.md`). Every story names its spec reference; a story is done
when its acceptance line holds and a test covers it.

**Baseline: v0.6.0** (tagged) — lexer/parser/codegen pipeline, array /
string / map runtimes with pre-v2 semantics, `ERR` runtime, std basics,
deb packaging. Not yet present: `map{K, V}`, coroutines, several v2
semantics below.

Sizes are relative: **S** small, **M** medium, **L** large, **XL** multi-week.

---

## v0.7.0 — Spec v2 conformance (core language)

Theme: the compiler matches the spec we just wrote. Coroutines excluded
(they are v0.9.0).

| # | Story | Spec | Size | Depends | Acceptance |
|---|-------|------|------|---------|------------|
| 1 | Array literals `{1, 2, 3}`; remove `[...]` literal syntax | §6.4, Come_types | M | — | `{...}` compiles everywhere; `[...]` is a parse error with a helpful message |
| 2 | Fixed arrays stay fixed: delete all promotion paths; `resize`/growth on a fixed array is a compile error | §6.4, Come_array §1 | M | — | Old promotion test cases fail to compile; fixed arrays pass/return/store unchanged |
| 3 | Static maps: parse `map{K, V}`, `{k: v}` literals, typed access | §6.6, Come_map | L | — | `map{string, int}` programs compile; mixed-type literals are compile errors |
| 4 | Map runtime semantics + methods: missing key → zero value of `V` + `ERR`; `.has()` `.keys()` `.delete()` `.clear()` | Come_map §3, §6 | M | 3 | Missing-key read sets `ERR.set()`; method table in Come_map.md fully works |
| 5 | Struct init `{.x = 5, .y = 10}`; `packed struct` / `align(n) struct` with C-compatible default layout | §6.2 | M | — | Layout matches the equivalent C struct (verified against a C reference in tests) |
| 6 | `ERR.set()` and `ERR.fatal(msg)` in the runtime and codegen | §12, Come_std | S | — | `ERR.set()` usable in conditions; `ERR.fatal` halts in all build modes |
| 7 | Out-of-bounds semantics: read → zero value + `ERR`, write → dropped + `ERR` (arrays and strings) | Come_array §2, §6.4–6.5 | S | 6 | OOB test cases behave per doc, no crashes |
| 8 | No implicit narrowing: semantic check; explicit `T(x)` casts only | §6.8 | M | — | Narrowing assignments are compile errors; lossless widening still implicit |
| 9 | Multi-file modules: every `.co` file in one directory declaring the same module forms that module; `module_init()`/`module_exit()` across files | §3.2, §3.5 | L | — | A 2-file module compiles and runs with correct init/exit order; both-layouts-present is an error |
| 10 | Functions as arguments in declarator shape (`int cmp(byte a, byte b)`); no `method` type anywhere | §8.3 | M | — | `sort` / `qsort` / `atexit` call sites compile with inline declarators |
| 11 | String conformance: immutability enforced, `.length()` only (no `.len()`), character-first indexing | §6.5, Come_string | M | 7 | String mutation attempts are compile errors; string_demo passes with `.length()` |
| 12 | `extern "C"` restricted to system modules | §13 | S | — | `extern` in a user module is a compile error |
| 13 | Doc fixes: spec §17 "One per file" → directory-based modules; README pillars updated (drop promotion text) | §3.2, §17 | S | 9 | Docs and spec agree with the shipped behavior |
| 14 | Conformance test harness: one `.co` test per spec section, wired into `tests/run_tests.sh` | all | M | 1–13 | The suite runs green in one command and grows with every story above |

**Exit criteria:** all examples compile and run; conformance suite green;
`packaging/build_deb.sh` produces a v0.7.0 deb.

## v0.8.0 — Standard library completion

Theme: `Come_std.md` and the per-type method tables are fully implemented.

| # | Story | Spec | Size | Depends | Acceptance |
|---|-------|------|------|---------|------------|
| 1 | `std` FILE API complete per Come_std.md (open modes, `setbuf` empty = unbuffered, `gets` empty at EOF, …) | Come_std | L | v0.7.0 | Every row of the Come_std tables has a working call + test |
| 2 | `std.proc`: `exit` / `abort` / `abort_handler` / `atexit` (new signature) / `getenv` / `system` | Come_std | M | v0.7.0 #10 | atexit callbacks run in reverse order at exit |
| 3 | Array method table complete: `contains` `index` `slice` `sort` `reverse` `set` `cpy` `insert` `remove` `clear` | Come_array §4 | M | v0.7.0 | All Come_array.md methods tested |
| 4 | String method table complete (the full Come_string.md list, incl. split/join/trim/format family) | Come_string | L | v0.7.0 #11 | All Come_string.md methods tested |
| 5 | `ERR.str()` message coverage for every runtime error the stdlib can set | §12 | S | 1–4 | No `ERR` code surfaces with an empty message |
| 6 | Stdlib examples: one runnable example per std module, added to `examples/` | Come_std | S | 1–4 | Examples build in CI-style run via `tests/run_tests.sh` |

**Exit criteria:** no row in any doc's method table is unimplemented;
examples all pass.

## v0.9.0 — Coroutines

Theme: spec §9, the last big v1 feature. Nothing in the current tree
implements it (0 files reference `coroutine` / `spawn` / `colink`).

| # | Story | Spec | Size | Depends | Acceptance |
|---|-------|------|------|---------|------------|
| 1 | Runtime: stackful coroutines (stack switching) + cooperative scheduler (ready queue + wait list) | §9.3 | XL | v0.7.0 | Two coroutines interleave deterministically under `yield` |
| 2 | `coroutine` declarations + `spawn` statement (parser, AST, codegen) | §9.1 | M | 1 | A spawned coroutine runs to completion |
| 3 | Isolation checks: no module-variable access inside coroutines; buffer arguments change owner to the coroutine | §9.1, §7 | M | 2 | Violations are compile errors; ownership moves verified in tests |
| 4 | `colink()` and the `link` type: `.read()` `.write()` `.close()`, ownership-moving writes, closed-peer semantics (empty buffer / `ERR`) | §9.2, §6.7 | L | 1, 3 | Ping-pong example passes; write-after-close sets `ERR` |
| 5 | `wait(...)`: pause until any of several links is ready or closed; returns the ready link | §9.2 | M | 4 | Multi-producer example passes |
| 6 | Lifecycle: coroutine end auto-closes its links and frees its memory; `main` returning ends the program; deadlock detection | §9.3 | M | 4 | Deadlocked program reports an error instead of hanging |
| 7 | Coroutine examples + stress tests (many coroutines, link churn) | §9 | M | 2–6 | Examples pass; no leaks under the ownership tests |

**Exit criteria:** §9 examples from the spec compile and run as written;
test suite green.

## v1.0.0 — Hardening and release

Theme: v1 is the spec, fully — verified, documented, packaged.

| # | Story | Spec | Size | Depends | Acceptance |
|---|-------|------|------|---------|------------|
| 1 | Full conformance pass: every spec section has at least one test; gaps become stories or spec errata | all | L | v0.9.0 | Coverage checklist in `docs/draft/TESTING.md` is complete |
| 2 | EBNF grammar finalized from the actual parser; `docs/draft/grammar.ebnf` promoted into `docs/` | §16 | M | 1 | Grammar and parser accept the same language on the test corpus |
| 3 | Diagnostics pass: error messages name the rule and the fix (esp. narrowing, promotion-removal, isolation) | — | M | 1 | Top-20 error cases reviewed by hand |
| 4 | Packaging: deb / rpm / arch rebuilt; version → 1.0.0; release notes from this roadmap | — | S | 1–3 | Clean-machine install runs `hello.co` |
| 5 | README + site refresh to v1 facts (modules per directory, no promotion, coroutines in) | — | S | 1 | No pre-v2 claims remain in README |

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
