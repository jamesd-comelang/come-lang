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
| COIL-139 | A failure reports where it started: ERR records a chain of errors with source locations | §12, Come_std §3/§16.1 | M | COIL-105, COIL-118 | The runtime records module, function, file and line when it sets an error (a fixed static store, per coroutine, no heap). ERR.no(i)/ERR.str(i)/ERR.where(i) take an optional index (no argument = 0, the root cause); ERR.depth() returns the entry count (0 with no error, 1 with one); ERR.fatal prints the message and the location; ERR.clear() resets the record. An out-of-range index returns 0 or an empty string and sets a new error. Existing ERR tests pass unchanged; a failed file open's ERR.where() names the user's file and line; errors in one coroutine are not visible in another. Spec §12 and Come_std §3/§16.1 list the new signatures, the index order and the out-of-range rule | ☑ |
| COIL-147 | Big files compile whole: no silent ceiling | §3 | M | — | The token list was a fixed 4096 array whose overflow wrote into the count field and neighbouring globals, so past the ceiling a file failed with wandering errors at innocent lines (the misdiagnosed gap #8) or - worse - compiled with constructs silently dropped (fn0(3) printing -1). The token list now grows with the file. The same audit found the AST children array (1024 per construct) overflowing into heap corruption and compiler segfaults: every parser append goes through AST_ADD, and a construct past 1024 elements is a loud error naming the split fix. Verified against the full filed matrix (25x15 through 100x5, 1x400, 2x400, 40x10 - all print 3). Test: tests/run_capacity_test.sh (generated shapes) | ☑ |
| COIL-146 | Tuple and struct returns cross module boundaries | §3.4, §8.2 | L | COIL-140 | An importer's translation unit carried none of an imported plain module's types: calls were implicitly declared (string returns corrupted, struct returns exploded in C with no Come diagnostic), tuple typedefs were never emitted for importers (a stateful module's tuple functions broke consumers' builds even when unused and unexported), and (a, b) = mod.f() was rejected because the tuple table only knew the current file. The driver now registers every module's struct definitions; each importer (and each sibling file of a directory module) emits the exported surface - struct definitions, tuple typedefs, exact prototypes, export-filtered - before its own code; destructures and tuple returns resolve callees through the registry (this file, sibling file, namespace, or instance) and forward field by field. Also fixed, found on the way: the parser read a following tuple return type as an uninitialized module variable's parameter list, silently dropping the variable and reparsing the function as void. Tests: tests/types_project + run_types_test.sh | ☑ |
| COIL-145 | An array literal is a value: pass it, assign it, don't just declare it | §6 | M | COIL-135 | In expression positions an array literal was pasted into the generated C as raw braces, which are not a value ("expected expression before '{'") - every ad-hoc array needed a named declaration first, which killed the test-helper shape expect(sql, {types...}, {starts...}). A literal in an expression now materializes its header through a statement expression performing the declaration's own construction (bulk copy for scalar elements, owned appends for strings), with the element type worked out from the elements (numeric promotion included). An empty or mixed literal whose type cannot be worked out is a clear error naming the typed-declaration fix. The declaration paths that need the raw braces (their own C arrays, struct initializers) opt out explicitly. Tests: src/array/t/08-literal-expr.co plus two negative cases in run_errors_test.sh | ☑ |
| COIL-144 | Long lines survive: no 256-byte line wall, no 130-char literal corruption | §6.5 | S | COIL-143 | Two stacked fixed buffers made long string content unusable: the lexer read source lines into a 256-byte buffer (a longer line was truncated mid-literal and reported unterminated), and a token's text was 128 bytes (a longer literal overran it and the corruption surfaced as broken generated C). Source lines are now read whole into a growable buffer; token text is 4096, matching the AST node text, and the literal scanners copy by span with a loud too-long error past the limit - never a silent truncation. The parser's adjacent-literal concatenation is bounded the same way. Regression test src/core/t/29-long-lines.co (a generated 3000-char literal verified char by char, plus a >256-char code line) | ☑ |
| COIL-143 | A quote can be written inside its own literal: '\'' and \" lex as escapes | §6.5 | S | — | The lexer's char and string scanners copied bytes one at a time with no escape awareness, so an escaped terminator was read as the closing quote and the rest of the line became an unterminated literal - the one character a char literal could not hold, and the everyday SQL quoted-identifier case in strings. Both scanners now consume a backslash pair as a unit; codegen's char decode already mapped \' to 39. The tokenizer's CP_SQUOTE numeric workaround is obsolete. Regression test src/core/t/28-quote-escapes.co covers both quotes, the scan-and-compare shape, and the neighbouring escapes | ☑ |
| COIL-142 | A lookup table is built once: maps live at module level | §3.5, §6 | M | COIL-102, COIL-140 | `map{K, V}` declarations parse at top level (the top-level path never consumed the type's braces, so a module-level map died on '{'). A header-typed module variable (string, array, map) in a singleton module emits the bare global - a zeroed header is a live empty value - and its index reads/writes, methods and printf conversions resolve its type through the module registry, so a table declared bare and filled once in init() is read by every function, on both the singleton surface and instance fields. An initializer on such a variable in a singleton module is a clear compile error naming the fix (assign in init()/main()) instead of a runtime call emitted as a C file-scope initializer. Along the way: files under a system module's `t/` directory are test programs, not system-module sources - the driver's system classification no longer skips their module analysis. Tests: src/map/t/04-module-table.co and tests/table_project (two instances, one fill each) with the initializer negative | ☑ |
| COIL-141 | A string hands over its bytes: s.byte_array() reaches the runtime it names | Come_string | S | — | The codegen constructs `come_string_<method>` for string methods, but byte_array's runtime function was named `come_string_to_byte_array`; the call was implicitly declared, returned int, and initializing a byte[] from it failed in the generated C ("invalid initializer"). The runtime function takes the constructed name. An audit of every other string method confirms byte_array was the only mismatch. Regression test exercises the assignment form, the empty string and signed UTF-8 bytes | ☑ |
| COIL-140 | Two ways to use a module: namespace for the stateless, instances for the stateful | §3 | XL | COIL-108, COIL-122 | A module with module-level variables gains instances: `var w = mod.create()` allocates the state bundle (the module's variables as fields, a fresh allocation-tree root, the init hook run bound to the instance); `w.f()` calls bind that state implicitly; `w.destroy()` runs the exit hook and reclaims the instance's owned memory. Statefulness is a static fixpoint over the module call graph (direct variable use, propagated through calls, with a witness chain); namespace calls to stateful functions are compile errors naming the chain, and namespace use never implies a singleton. Stateless functions keep today's plain form and are callable both ways (instance calls reach them through a generated wrapper). Using a destroyed instance is safe: the call sets ERR and returns the zero value; destroy is idempotent. Module variables of an instance module are declared in one file (its state home). Spec §3 documents the two surfaces, the analysis, and the lifetime rules | ☑ |

## Post-v1 backlog (explicitly deferred by the spec)

- Interfaces (explicit implementation declarations only, if ever) — §17
- Function values beyond arguments: variables, struct fields, returns (`fn` types) — §8.3
- Struct keys for maps — §6.6
- `wait()` timeouts — §9.2
- `reserve(n)` capacity control for arrays — Come_array §3
- OS-thread support via system modules beyond the v1 FFI surface — §13
