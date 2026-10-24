# Performance trials — branch `perf-experiments`

Goal: Come on par with pure C. Reference workload: the co_SQLite
tokenizer bench (4,000 SQL lines, 20 passes, 2,530,480 tokens) vs C
`sqlite3GetToken` at ~0.033 s. Every trial below prints identical
checksums (typesum=285119860, lensum=7962360); a trial that changed
the checksums would be invalid, not faster.

Baseline on this branch (main @ 69fdf49, COIL-160): ~3.76 s (~115x).

## Results (3 runs each, same machine, same day)

| Trial | Method | Time | vs base |
|---|---|---|---|
| base | `come build` plain | 3.74 / 3.73 / 3.80 s | — |
| A1 `-O2` | `COME_CFLAGS="-O2"` | 3.77 / 4.12 / 3.97 s | none |
| A1 `-O3` | `COME_CFLAGS="-O3"` | 3.82 / 3.78 / 3.78 s | none |
| A2 `-flto` (generated only) | `COME_CFLAGS="-O2 -flto"` | 3.44 / 3.41 / 3.28 s | ~10% |
| A2 full LTO | one `gcc -O2 -flto` over bench + runtime sources | 3.30 / 3.17 / 3.15 s | ~15% |
| A4 upper bound (arm = no-op) | stub `come_err_arm` body, link manually, `-O2` | 3.38 / 3.45 / 3.38 s | ~10% |
| A5 leaf (frames stripped, arms kept) | strip scope push/pop from 6 leaf predicates, no `-O` | 2.72 / 2.74 / 2.72 s | ~28% |
| **Combo: leaf no-frame + no-arm + full LTO** | all three together | 1.29 / 1.28 / 1.32 s | **~66%, 2.9x** |

## What each trial actually was

- **A1**: pure flag trials, each from a clean co_SQLite `.ccache`
  (flags are not a cache input, so a stale cache would have
  invalidated the comparison). `-O2`/`-O3` alone do nothing: the
  hot path is opaque runtime calls and call structure, not local
  code the C compiler can improve. `-O2` stays valuable only as
  the base that lets LTO/inlining work.
- **A2 generated-only**: `come build` already passes COME_CFLAGS to
  every compile and link, so `-flto` flows through. It inlines
  across generated translation units but NOT into the runtime
  (prebuilt `build/*.o` lack LTO bytecode).
- **A2 full LTO**: one gcc invocation over the 5 bench `.c` files
  (from `.ccache`, listed by `come build -v`) plus all runtime
  sources (`memory.c`, `map.c`, `array.c`, `coro.c`, `link.c`,
  `std.c` + `build/std.co.c`, `string.c` + `build/string.co.c`).
  The extra ~5% over generated-only LTO is the runtime inlining.
- **A4 upper bound**: `come_err_arm` is 4 stores + a flag
  (`src/mem/memory.c`). A copy with an empty body was compiled to
  a trial object and linked in place of `build/memory.o`. This
  measures the most A4 can ever buy, not A4 itself: real A4 must
  still stamp locations on error paths. Result (~10%) is well
  below the old 0.72 s model (326M arms x 2.2 ns) — the micro
  price does not transfer to the interleaved bench.
- **A5 leaf**: in generated `chars.co.c`, all scope push/pop
  lines were deleted from exactly six scalar-only leaf functions
  (`tk_byte`, `tk_is_space`, `tk_is_digit`, `tk_is_xdigit`,
  `tk_is_idchar`, `tk_class`; 90 lines). Sound because these
  functions allocate nothing and return scalars — their frames
  own no values. `tk_class` alone accounts for 64 of the lines:
  it pushes a block frame per branch, not one per call, which is
  why this beats the one-pair-per-call model (0.83 s). The
  `__inst` wrappers delegate to the plain functions and carry no
  frames of their own.
- **Combo**: leaf no-frame + no-arm + full LTO together. The gain
  is super-additive (2.9x vs ~1.5x for the parts): removing the
  calls unblocks inlining, which removes more calls. This is the
  key interaction the ranked plan missed — the three costs are
  not independent.

## A3 trial (co_SQLite branch `perf-experiments`, commit 08d5c97)

A single-function scanner, `fast_scan()` (new file
`src/tokenizer/fastscan.co`, same module so it reuses the
constants, keyword table, and cursor state), mirroring `tk_scan`
branch-for-branch with byte reads, class dispatch, and
predicates inlined. `fast_next()` on the shared cursor state,
`tools/bench/bench_fast.co` driver, differential test
`src/tokenizer/t/07_fastscan_test.co` (556 cases: all 04+06
corpus inputs plus edge cases incl. BOM, VT, high bytes, blobs,
malformed numbers, unterminated quotes/comments; a mutation
check proves the test bites). All 7 suites green.

| Trial | Time | vs base |
|---|---|---|
| A3a fast scanner, current compiler | 2.67–2.89 s | ~26% |
| A3b fast_scan frames stripped in generated C | 2.60–2.69 s | ~30% |
| Fast scanner + no-arm + full LTO | 1.42–1.45 s | ~62% |

Readings:

- A3a's win (~1.0 s) is exactly the removed per-byte helper
  calls — the same magnitude as the A5 leaf trial, because it
  is the same cost removed a different way.
- A3b barely moves over A3a: the fast scanner's remaining
  frames are per-token and per loop iteration, ~0.1 s total.
  (Loop iterations are ~10M here, not the 79M pairs of the old
  profile — that figure was dominated by per-call frames.)
- Fast-combo is slightly SLOWER than old-combo (1.44 s vs
  1.29 s). The C compiler, given inlinable frameless leaves,
  reconstructs the tight loop itself and beats the hand
  structure (giant if-chain dispatch, redundant bound checks).
- **Verdict: do NOT rewrite the faithful port.** A3 is
  subsumed by the compiler-side fixes. The fast scanner stays
  on the co_SQLite branch as the trial record only.

## A5 real implementation (come-lang branch `perf-experiments`, commit cffcc67)

The leaf trial is now a compiler feature, on the branch.
The driver computes each module's frameless set as a
fixpoint over the call graph in `analyze_module` (before
any file is generated) and registers the names; emission
skips the marker push at entry and the pop-to-marker on
every exit path for those functions. Qualification is
conservative: no buffer-typed locals, no string or
aggregate literals, no calls except to other frameless
functions of the same module (plus `.length()` /
`.bytesize()` / `.owner()`), no break/continue (they pop
to a marker-relative depth), and string `+` only on
provably scalar operands. Main, coroutines,
instance-form functions, and create/destroy never
qualify. Loop and block frames inside a frameless
function are still emitted; only the marker is skipped.

| Trial | Time | vs base |
|---|---|---|
| A5 real, faithful scanner | 2.68–2.79 s | ~28% |
| A5 real + no-arm + full LTO | 1.39–1.41 s | ~63% |

Readings:

- Real A5 matches the leaf trial (2.73 s) and the fast
  scanner (2.76 s): all three remove the same dominant
  cost, the leaves' per-call frames. Marker-only removal
  captures nearly the whole leaf win because the leaves'
  block frames fire only on the taken branch.
- Tokenizer leaves that went frameless: tk_byte, tk_class,
  is_space/digit/xdigit/idchar, tk_char_index, tk_utf8_len,
  tk_cp, tk_id_len. tk_scan stays framed via tk_keyword_in
  (`.lower()` + ERR); the number scanners stay framed via
  `break`. `next()` stays framed (instance-form).
- A5 + no-arm + LTO (1.40 s) lands just above the
  hand-stripped ceiling (1.29 s); the 0.11 s gap is the
  leaves' remaining block frames, the v2 item.
- The frameless e2e (`tests/run_frameless_test.sh`:
  output + generated-C marker checks + ASan run) passes;
  full suite green from clean, 0 warnings.
- Side finding, pre-existing, NOT caused by this change:
  passing a fresh string concatenation straight to
  `printf` as an argument segfaults on the baseline
  compiler too (assign-first works). The fixture avoids
  that shape. It deserves its own investigation.
- Branch-only: merging to main is a story decision (next
  free number COIL-161). A4 (error-path-only stamping)
  is still gated on the founder's call.

## What is still open

- ~1.3 s remains after the combo (~39x from C). What is left:
  per-token `tk_scan` frames, per-line `byte_array` conversions,
  keyword map lookups, bench-loop frames. That is A3 territory
  (single-loop scanner restructure, co_SQLite side), not more
  compiler flags.
- Real A4 and A5 implementations (compiler analyses, not stubs
  and generated-C edits) are still to be built. A4 is gated on
  the founder's call: does error semantics need the innermost
  failing statement inside a callee (location threading), or is
  call-site stamping enough?
- A5's full form (no-alloc analysis + `may_allocate` summaries +
  stack checkpoints for non-escaping scopes) goes beyond the
  leaf case proven here; the leaf case is the safe first slice
  (scalar-only functions).
- Trial binaries (`tools/bench/bench_trial_*`, `bench_full_lto`,
  `bench_noarm`, `bench_noframe`, `bench_combo`) are scratch in
  the co_SQLite checkout, not committed anywhere. Trial sources
  were generated under `/tmp/probe157/`; the commands above
  reproduce them.

## How to reproduce

From the co_SQLite checkout, with this branch's compiler built:

```
# list the bench's generated C files
come build -v tools/bench/bench.co -o tools/bench/bench_verbose  # [CMD] link line
# flag trials: rm -rf .ccache first, then COME_CFLAGS="..." come build ...
# manual links: gcc <flags> -I<come>/src/include -I<come>/src/core/include \
#   -D__STDC_WANT_LIB_EXT1__=1 <bench .c files> <runtime .o or .c files> -ldl
```
