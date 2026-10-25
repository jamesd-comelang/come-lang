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

## Step 6 trial — mask/modulo proofs + proof audit (2026-10-24)

Branch come-lang step6-proofs (from perf2 2193ff5), tag
step6-proofs, pushed. NOT merged to main.

Change (src/core/codegen.c, driver): nonneg_expr extended
to binary ops (&,|,^,+,*,/, % for nonneg operands; - and ~
excluded) and to int/long(byte[] read) (zero-extension,
finding 32, 0..255); early-exit guard -> nonneg (lower
bounds, OR disjuncts each hold; AND not); length facts
(buf -> length >= min, from early-exit length guards and
array-literal lengths, per-variable void on reassign /
remove / clear, block-scoped); mask proof (x & MASK, MASK
nonneg literal, x pure, length >= MASK+1); modulo proof
(EXPR % C, C positive literal, EXPR nonneg, length >= C);
proof audit: --debug (true debug only: no --lto, no
COME_CFLAGS) re-inserts checks for proven reads
(g_codegen_debug plumbed from driver, mirroring
g_codegen_unchecked); g_unchecked (explicit opt-out) is
NOT gated by audit.

SOUNDNESS CATCH (test gate worked): the first fixup rule
(byte if (c < 0) c += 256 -> nonneg) was UNSOUND and was
REMOVED before commit. For byte variables +256 wraps mod
256 (251 -> -5), so the fixup is a no-op and never makes
a byte nonneg; the proof test caught it (fmod returned 0
via an OOB raw read). Do NOT re-add a byte fixup rule.
The hash's actual nonneg source is zero-extension of byte
reads (int(b[pos])), not the fixup (kw_hash_code's c0/c1
are ints from zero-extended reads, already 0..255).

Gate: come-lang suite green from clean (including new
proof gate: tests/proof_project + run_proof_test.sh, normal
6 checked reads (controls only), --debug 9 (audit
re-inserts all 3 proven), ASan 6, values correct);
co_SQLite suites 9/9 green with step6 compiler
(next-token branch); bench checksums exact.

Measure (packed, paired interleaved 8 rounds, vs perf2
nt): default faithful 301 -> 297 (paired diff median
-2 ms, flat within noise), LTO faithful 209 -> 207
(-2 ms), fast flat both (176 -> 176, 138 -> 141 noise).
Best (LTO fast) unchanged ~138-141 (Go 131). As analyzed:
bench tables are parameters (length unknown), so
mask/modulo cannot fire on the bench; the ~2 ms (if real)
is from nonneg extensions improving loop proofs, not the
new rules. Step 6 is infrastructure + safety (audit
de-risks all proofs), not a bench win. The bucket read
still needs length propagation (step 8, deferred) or a
port guard (net ~0, guard cost cancels saving).
