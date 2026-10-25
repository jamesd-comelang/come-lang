# perf2 — second performance round (from parity 99f6ba0)

Branch: `perf2` (come-lang), from `parity` 99f6ba0.
co_SQLite work (keyword port, bytes view) goes on a co_SQLite
`perf2` branch from `redesign` 74bce7c when it starts.
One tag per finished step, descriptive names (not step numbers).
Rule: full come-lang suite + co_SQLite 7/7 green + ASan clean
before a tag; bench checksums must stay exact
(2,530,480 tokens, typesum 285119860, lensum 7962360).

## Baselines (perf2 = parity, measured 2026-10-24)

3 runs each, shell `date`, from co_SQLite tools/bench/.

| Build | Faithful | Fast |
|---|---|---|
| Default (-O0) | 594/598/590 (~594) | 459/472/460 (~460) |
| -O2 only (manual cc -O2 of generated C) | 491/474/471 (~474) | 455/439/469 (~455) |
| --lto (-O2 -flto) | 407/375/400 (~400) | 357/385/354 (~357) |

New finding (review 2 §C, confirmed): -O2 alone captures most
of the LTO gain for the faithful scanner (594 -> 474, -120 ms)
but almost none for the fast scanner (460 -> 455); fast's win
is in -flto (455 -> 357). So a default -O2 helps faithful far
more than fast. All checksums exact.

## Step A: lazy scope markers (IN PROGRESS)

Design (runtime-only, src/mem/memory.c, no codegen change):
push/push_marker record only the slot (ctx NULL + arena
checkpoint); the talloc chunk + link is allocated on first
use via co_scope_materialize(), called by
come_scope_current() (generated C uses COME_CTX per
allocation, so this covers every allocation site),
come_scope_caller_ctx() (return adoption must not steal a
lazy caller frame to root) and come_scope_ctx_for_depth()
(store adoption into an outer scope). Materialization
recurses to ancestors so the chunk links under the correct
parent. come_scope_adopt() skips NULL-ctx frames (a lazy
frame owns nothing). Pop already tolerates NULL ctx
(co_frame_free_ctx is a no-op), the checkpoint-frame path.
Fail-safe: a checkpoint block that talloc-allocates (a
block_is_checkpoint bug) now materializes instead of
allocating in root.

Expectation (calibrated, not the review's ~100 ms): the
saving is the freelist + link/unlink portion of the ~10.5 ns
scope pair, for markers that never call current() (next(),
fast_scan() — wrappers/accessors that create no values).
Typ()/len() are likely already frameless (0 cost). Clean
blocks are already skipped (Design 5). Realistic: ~25–50 ms,
not ~100 ms. Measure, don't assume.

Status: DONE, tagged `perf2-lazy-markers` (pushed).
Gate: come-lang suite all green (67 passed, 0 failed);
co_SQLite 7/7 green; ASan clean (user code AND an
ASan-instrumented memory.c build), checksums exact.

Measured 2026-10-24 (3 runs each, same bench binaries
rebuilt with the new runtime):

| Build | Before (parity) | After (lazy) | Delta |
|---|---|---|---|
| Default faithful | ~594 | 471/464/462 (~464) | -130 (-22%) |
| Default fast | ~460 | 349/372/322 (~349) | -111 (-24%) |
| --lto faithful | ~400 | 256/252/250 (~253) | -147 (-37%) |
| --lto fast | ~357 | 258/259/256 (~258) | -99 (-28%) |

Bigger than the calibrated ~25–50 ms expectation, close to
the review's ~100–125 ms. LTO faithful is now ~253 ms
(was ~400); gap to Go (131 ms) is now ~122 ms, to Rust
(80 ms) ~173 ms. Still behind both, but the gap nearly
halved. No correctness change: checksums exact, suites
green, ASan clean.

## Step B: fat LTO runtime objects + -fno-lto on non-LTO links (DONE, no perf win — kept for robustness)

Finding: the runtime .o files were already SLIM LTO objects
(`plugin needed to handle lto object` with `-fno-lto`).
Default links therefore ran auto-LTO at -O0 (the plugin
claims LTO sections even without -flto), so the runtime ran
unoptimized in default builds; --lto links already inlined
runtime code via bytecode. B is fat objects (machine code +
bytecode) in all 6 runtime Makefiles (mem, map, array, coro,
string, std) PLUS `-fno-lto` on non-LTO driver links
(come_compiler.c, respecting a user -flto in COME_CFLAGS),
so default links use -O2 machine code and --lto links still
use bytecode. Neither half alone works: fat alone still
auto-LTOs at -O0; -fno-lto alone fails to link slim objects.

Fix required along the way: string.o/std.o are built by
`ld -r` combining, which drops fatness. Changed both to
`gcc -flto -ffat-lto-objects -r` (plain `gcc -r` also drops
it — LTO -r defers codegen). This also fixed the examples
build, which broke on -fno-lto + slim string.o/std.o.

Measured 2026-10-24 (vs step A; checksums exact, suites
green: come-lang 67/0, co_SQLite 7/7):

| Build | After A | After B | Delta |
|---|---|---|---|
| Default faithful | ~464 | ~476-481 | +3% (noise) |
| Default fast | ~349 | ~346 | flat |
| --lto faithful | ~253 | ~254 | flat |
| --lto fast | ~258 | ~253 | flat |

Verdict: NO measurable perf win. The default-vs-LTO gap is
dominated by user code at -O0, not by the runtime's -O level
(post-A the runtime hot path is already minimal). Kept
anyway (tag `perf2-fat-lto`, pushed): fat objects link with
any linker (not just gcc-with-plugin) and default links are
no longer dependent on auto-LTO behavior, which varies by
gcc version/distro. Merge decision is the founder's: A is
the perf win, B is robustness. If B is ever deemed unwanted
complexity, it reverts cleanly (separate commit).

## Step C: default -O2 with --debug escape (DONE)

Driver change (come_compiler.c): non-LTO, no-COME_CFLAGS
builds now compile with -O2 (was -O0: gcc with no -O flag).
COME_CFLAGS set: the user's flags as-is (add -O2 yourself
if you want it). --lto unchanged (-O2 -flto). New flag
`come build --debug` gives -O0 -g, but only when COME_CFLAGS
is unset and without --lto (with either, --debug has no
effect -- documented in --help). The review's static/hidden
visibility half of C is NOT done: come links .o files
statically, not a .so, so visibility does not enable any
inlining here (LTO already covers it).

Object names suffix by ACTUAL flags (the codebase's own
principle, extended): default -O2 builds use .o2.o (not
plain .o), so pre-C -O0 objects are not silently reused
after upgrade; --debug builds use .debug.o (and
.unchecked.debug.o / .unchecked.o2.o for unchecked).
COME_CFLAGS builds keep .o/.unchecked.o (their
pre-existing flag-change staleness is out of scope).
Single-TU keeps its fixed single_tu.o for all flags
(pre-existing; --debug + --single-tu users should clean
between flag changes, as --lto + --single-tu already
requires).

Measured 2026-10-24 (vs keyword-hash step; checksums
exact, suites green: come-lang 67/0, co_SQLite 8/8):

| Build | Before C | After C | Delta |
|---|---|---|---|
| Default faithful | ~432 | ~329 | -103 (-24%) |
| Default fast | ~276 | ~232 | -44 (-16%) |
| --debug faithful | -- | ~432 | = old -O0 exactly |
| --lto faithful | ~221 | ~215 | flat |
| --lto fast | ~157 | ~159 | flat |

--debug reproducing the old -O0 value exactly (432) is
the control: the win is the -O2 itself, nothing else.
LTO flat as expected (it already had -O2). The value of
C is the default: users who do not pass --lto now get
232 ms (fast) / 329 ms (faithful) instead of 276/432.

## Remaining queue — CLOSED (see co_SQLite PERF2.md for F)

- D (non-erroring map probe): MOOT for the bench — the
  keyword port removed the tokenizer's hot ERR-miss path,
  which was D's motivation. Not done for perf.
- G (mask/modulus index proofs): NOT a tokenizer win —
  the hot path already has zero checked reads. Not done.
- F (bytes view): tried, no win (promotion by design;
  see co_SQLite PERF2.md). Not done.
- co_SQLite-side keyword port is logged in
  co_SQLite/PERF2.md (tag perf2-keyword-hash).

Final perf2 numbers (tokenizer bench, checksums exact):
default faithful ~329, default fast ~232,
--lto faithful ~215, --lto fast ~157.
Best (LTO fast) vs Go 131 ms: ~26 ms behind;
vs Rust 80 ms: ~77 ms behind; vs C 33 ms: ~124 ms behind.
Nothing in perf2 is merged to main; all steps are tagged
on branch perf2 (come-lang) / perf2 (co_SQLite) for the
founder's merge decisions.

- B: inlinable runtime (static inline hot helpers + fat LTO
  objects for the prebuilt runtime; same cc builds both here,
  so version coupling is not an issue).
- Keyword port: literal sqlite3KeywordCode in co_SQLite
  (O(1), no map, no ERR); add mixed-case keywords to the
  differential corpus first.
- C: default -O2 (with -O0/--debug escape); note the baseline
  asymmetry above (faithful -120 ms, fast ~0).
- D probe (non-erroring map probe, general API); __thread ERR
  only if errno access then measures hot.
- F: zero-copy bytes view in bench/cursor path, after
  checking open()'s ownership (byte_array() copies: talloc
  alloc + memcpy, ~8 ms total — confirmed in code).
- G: mask/modulus proofs (general codegen, not tokenizer).
- Hold: header packing / context IDs (redesign, not a
  packing); E tuple half (check tuple lowering first).

## Step 7a trial — runtime-helper header inlining (2026-10-25)

Branch come-lang step7a-inline (from perf2 2193ff5), tag
step7a-inline-trial, pushed. NOT merged to main.

Change: generated C prologue now defines the 7 hot runtime
helpers as C99 `static inline` (come_err_arm, come_scope_
push / push_checkpoint / push_marker / pop / pop_to_marker
/ pop_to_depth), with scope + arena struct definitions and
extern decls for the 5 helpers they call (grow, marker_base,
arena_reset, arena_free_blocks, frame_free_ctx) and the 2
globals they read (co_scope_active, g_err_active). memory.c:
those 7 symbols changed static -> extern (defined in
memory.o; no public header declares them, so hand-written
code cannot use them -- exposure is to generated code only).
Header guards (backward-compatible, default unchanged):
mem/talloc.h scope decls wrapped in COME_SCOPE_NO_PROTO,
come_err.h err_arm decl wrapped in COME_ERR_NO_ARM_PROTO;
prologue defines both macros at the very top (before any
include) and skips codegen's err_arm extern. Checkpoint
test grep narrowed to call syntax (definitions no longer
count as pushes; output diff already passed, so this is a
counting fix, not a behavior change).

Linkage lessons (all caught by the build, not by review):
plain `inline` emits standalone copies -> multiple definition
with memory.o; `static inline` conflicts with extern decls
in the same TU, so externs must be suppressed (guards +
skipped codegen extern), not coexisted with; come_coro.h
transitively includes mem/talloc.h before the prologue's
talloc.h include, so guards must be defined before ANY
include, not just before talloc.h (preprocess proved the
double inclusion; the first, pre-guard inclusion leaked
the decls and talloc.h's own include guard made the second
a no-op). An early LTO-wrapper segfault building std.o was
a downstream symptom of the duplicate definitions, not a
separate LTO bug: it vanished once linkage was fixed.

Gate: come-lang suite green from clean (with checkpoint
grep fix); co_SQLite suites 9/9 green with step7a compiler
(next-token branch); bench checksums exact.

Measure (packed, paired interleaved 8 rounds, vs perf2 nt):
default faithful 299 -> 294 (-7 ms paired median), default
fast 179 -> 165 (-14 ms, all rounds negative), LTO faithful
211 -> 210 (flat), LTO fast 144 -> 133 (-10 ms, all rounds
negative). Best (LTO fast) now ~133 ms (Go 131, ~2 ms behind
-- essentially at Go parity for the trial fast scanner;
faithful primary still ~210 ms, so product parity is NOT
achieved). Asymmetry is the finding: 7a recovers ~38% of
fast's default-vs-LTO gap but only ~8% of faithful's --
faithful's gap is mostly user-code inlining (deferred 7b),
not runtime helpers. The LTO fast gain was unexpected
(LTO was assumed to already inline these); it suggests LTO
does not fully inline runtime helpers for fast's shape, a
finding, not a proven mechanism.

## Measurements + tests (2026-10-25, branch tests-lazy-header)

Authorized scope: callgrind substitutes (ptrace, gcov, objdump) +
lazy-marker and header-order tests. No merges, no 7b, step 10
still unanswered. Tests commit 77aa450 on this branch; this
section is the measurement log commit on top of it.

### Ptrace instruction counts (default perf2 packed benches)
Binaries: co_SQLite bench_nt / bench_fast_nt (perf2, default -O2,
packed methods). Subset: first 200 lines of workload.sql, 20
passes, 128,720 tokens, checksums identical both scanners
(typesum=14512780, lensum=407360). Tracer:
~/workspace/ptrace_tools/tracer2 (workspace, NOT /tmp: /tmp/tracer2
vanished 00:53-01:01 on 2026-10-25, killing the first subset run;
recreated in workspace and rerun properly backgrounded).
Empty baseline (0 lines, 0 tokens): 2,676,242 steps for BOTH
faithful and fast, identical to the step -- startup is a clean
common control, subtracted below. Single run each: step counts
are deterministic, not timing, so paired rounds are not needed
for noise (empty determinism + identical checksums support this).

- Faithful total 115,220,074 steps (2077 s at ~55k steps/s)
  net 112,543,832, 874.4 instr/token.
- Fast total 77,667,224 steps (1392 s)
  net 74,990,982, 582.6 instr/token.
- Net ratio faithful/fast = 1.501. Raw ratio = 1.484
  (startup compresses it slightly toward 1; net is the right
  figure, raw is shown only because it lands almost exactly
  on the brief's 1.48).
- Time ratio, same binaries, default packed (next-token
  baseline): ~305/187 ms = 1.631. Other trial runs gave
  1.67-1.76 (VM phase variation), so the CPI penalty below
  is ~9-14%, not a single precise number.

Verdict: predominantly instruction-bound, with a smaller
stall/layout penalty in default builds. Time ratio =
instruction ratio x IPC ratio: 1.631 / 1.501 = 1.087, so
faithful also runs ~9% worse IPC than fast in default builds.
Cutting instructions is the main lever (faithful must shed
~292 instr/token to reach fast -- structural, not a few
pushes, consistent with the earlier 55 ms unexplained gap);
stall/layout work alone cannot close a 1.5x instruction gap.
Limitations: default binaries only -- LTO instruction ratio
was NOT measured (another ~1 h via ptrace, not done); net
includes the common bench-loop body per token (~15-20 instr,
est.), so the scanner-only ratio is slightly higher than
1.501 (roughly 1.52 if loop overhead is removed), which does
not change the verdict. Full bench via ptrace would be ~20x
longer (~19 h), hence the subset.

### Gcov (counts only, never timing)
COME_CFLAGS="--coverage -O2 -g" builds, full bench, checksums
exact. Faithful: tk_scan 2,530,480 calls, kw_hash_code 528,980
(0.209/token). Fast: fast_scan 2,530,480, kw_hash_code 528,980
(identical). All per-class counts identical. Gcov does NOT
give instruction counts -- it replicates step 1 counters with
a per-line breakdown. Files: ~/workspace/gcov_results/.

### Objdump (LTO fast hot function fast_next.isra.0)
perf2: 24 call sites, 13 scope/err (1 push_marker, 3 pop,
9 err_arm), 11 oob. 7a: 21 call sites, 0 direct scope/err,
1 co_scope_grow (rare), 3+3 inlined-pop helpers, 11 oob
(unchanged). Yes, 7a removed the direct scope/err calls from
the hot function. Main-function call counts (115 vs 121) are
not the hot loop (main includes setup/line-loading).

### Tests on this branch (see 77aa450)
Lazy-marker: nested-callee first allocation, chown across a
lazy middle frame, escaping returns, unwinding through lazy
frames + early return -- output + ASan, first run green.
Header-order (7a-specific): guards before first include,
talloc/coro after guards, static inline defs present, no
conflicting externs -- output + ASan, first run green.
Full suite green, co_SQLite 9/9 green. Neither test is merged
to main; header-order fails correctly on pre-7a compilers.
