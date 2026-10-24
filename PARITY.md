# Parity branch log

Language-only path to C parity. No port changes (the
tokenizer stays a faithful port; only the language,
compiler, and runtime move). Each step is a cutoff:
implemented, tested, benched, committed, and tagged
(`parity-stepN`), so any step can be picked up,
merged, or reverted independently later. Branch:
`parity`, cut from redesign 216e152.

## Steps (in order)

- Step 1: guard-dominated bounds-check elimination
  (DONE, see below).
- Step 2: loop guards -- while positive-form and
  counted for (DONE, see below).
- Step 3: runtime intrinsics + whole-program TU
  (DONE, see below).
- Step 4: Design 1+2 remainder (loop-body locals,
  multi-assignment fixpoint, array/map promotion)
  then Design 5 frame tiers.
- HELD OUT (needs an explicit call, not covered by
  "do them"): an explicit unchecked/unsafe region.
  It is the only design that guarantees 1x, and it
  breaks Come's safety promise for that region.

## Step 1 DONE (2026-10-24): guard-dominated bounds-check elimination, narrow slice

Implemented in codegen.c (proof set threaded through
`generate_block_children`: early-exit guard adds its
pair after the guard statement, positive guard adds
its pair inside the then-branch only, any statement
with an assignment or call voids all pairs, block
save/restore scopes proofs, function entry clears).
Array reads with a proven (identifier buffer,
identifier index) emit raw `((T*)(h.data))[idx]`
instead of `COME_ARR_GET_T` (also an lvalue, and it
evaluates each operand once, fixing the macro's
double evaluation for these operands). String
indexing (`s[i]` -> wchar walk) is NOT covered --
its check is intertwined with the UTF-8 walk, not a
plain ternary. Test: `tests/bounds_project/` +
`tests/run_bounds_test.sh` (combined + pos/ctl pins:
pos has raw and no COME_ARR_GET_T, ctl the reverse;
ASan; boundary i == len - 1 valid, i == len and
i == -1 caught by the guard). Suites green from
clean (Come 79 passed lines, exit 0, "all green";
co_SQLite 7/7). Real tokenizer `tk_byte` verified
raw in generated C (`(uint8_t*)(b.data))[i]`).
Bench (checksums identical): ~0.66 s, UNCHANGED
from the 0.62-0.68 s pre-step-1 range -- honest
result, not a win on this workload: the per-byte
cost is the `tk_byte` call itself (43.7M calls, not
inlined at default flags) plus the guard's
`length()` call, not the redundant check. That is
what step 3 (intrinsics) targets; step 1 still pays
where checks dominate (and it removes a check per
byte that no longer needs proving).

## Step 3 DONE (TU part, 2026-10-24): opt-in `come build --lto` (user-code-only LTO)

`come build --lto` adds `-O2 -flto` to the user-code
compile and link steps (via the driver's single
`cc_extra_flags()` point, so it composes with
COME_CFLAGS). It is user-code-only: the runtime .o
files are prebuilt and untouched, which the
measurement says is sufficient (user-only LTO
matches full LTO). --single-tu was measured first
and does NOT capture the win (~0.62 s): GCC at -O2
does not inline the global tk_byte even in one TU.
--lto needed -O2, not just -flto: the driver's
default compile has no -O flag (-O0), and LTO at
-O0 does not inline (first --lto attempt, -flto
only, gave ~0.62 s, no win; adding -O2 gave
~0.38 s). The flag is opt-in; making it the
default is a separate call (not made here).
Test: `tests/lto_project/` + `tests/run_lto_test.sh`
(cross-module add/mul: identical output with and
without --lto; verbose build shows -flto and -O2 in
the cc commands; nm pin -- the helper symbols are
global without --lto and inlined away with it).
Suites green from clean (Come exit 0, "all green";
co_SQLite 7/7). Bench (checksums identical):
default ~0.64 s, `come build --lto` ~0.38 s (1.7x).
Gap to C (0.033 s) is now ~11x, down from ~20x.
Remaining gap is runtime calls + ownership, which
is step 4's territory (escape/arenas) and beyond.

## Step 3 DONE (intrinsic part, 2026-10-24): array/list .length() as a header-count intrinsic

Non-string `.length()` (byte[], string[], T[] --
the "shared" length path) no longer calls
`come_string_list_len`; it emits `(receiver).count`
directly. The function only returned list.count, so
the intrinsic is exactly equivalent (same value,
same uint32_t type, same single evaluation of the
receiver). Plain strings are excluded: their length
counts characters, not bytes, so they keep the
walking `come_string_len` call. Maps are excluded:
they keep `come_map_len`. The legacy `len` method
name is left as a call (not hot). The intrinsic
preserves the unsigned comparison behavior
(finding 38) -- it is not a fix for that finding.
Test: `tests/intrinsic_project/` +
`tests/run_intrinsic_test.sh` (byte/list positives
with `.count` present and no list_len call; string
(UTF-8 "hello" with an accent, 5 chars not 6 bytes)
and map controls pinned to their calls by
function-level generated-C checks; ASan). Suites
green from clean (Come exit 0, "all green";
co_SQLite 7/7). Real tokenizer verified: tk_byte's
guard is now `(i >= (b.count))`, zero list_len calls
in the tokenizer modules. Bench (checksums
identical): ~0.64 s, essentially unchanged from
step 2's ~0.65 s -- the length call was not the
bottleneck; the tk_byte CALL itself is.
MEASURED (step 3 continuation): user-code-only LTO
(COME_CFLAGS="-O2 -flto", runtime NOT rebuilt with
-flto) moves the bench 0.64 s -> ~0.37 s (1.7x),
matching the redesign's full-LTO figure -- so the
win is from inlining USER code (tk_byte across
modules), and runtime -flto adds little on top.
Whole-program TU / an LTO build mode is therefore
the step-3 continuation that actually moves the
bench; it is NOT folded into this cutoff (it is a
build-system change, to be done as its own tagged
piece, opt-in first, default only on his call).

## Step 2 DONE (2026-10-24): loop guards (while positive-form, counted for)

Loop bodies now thread proofs like blocks: a shared
per-statement helper (`generate_guarded_stmt`: emit,
then void on assign/call, add on early-exit guard)
is used by block children AND by while/for/do-while
body children. Loop bodies start with a CLEARED proof
set (save/clear before, restore after), so pre-loop
proofs cannot leak into a body that runs many times.
While with a positive-form condition (`0 <= i &&
i < len`, either order) and counted for (`int i =
<nonneg>` init, `i < len` cond, `i++` update) prove
their pair at the top of every iteration (the body
sees it; after the loop it does not persist, since
the loop may exit with i >= len). Do-while bodies
start cleared with no proof (the cond runs after the
body, so it cannot guard the first iteration).
FIXED HERE (errata on step 1): step 1 shipped a leak
-- loop bodies bypassed `generate_block_children`,
so a pre-loop guard's proof leaked into loop bodies
(a `while (true)` body could go raw from a stale
proof). Tag parity-step1 has this leak; step 2 closes
it. The parser rejects assignment in for init/update
positions, so the matcher only handles VAR_DECL init
and POST_INC update (assign branches removed as dead
code). A while cond of `i < len` alone is refused
(it does not prove i >= 0); a for with a negative
init or a decrement is refused. Test:
`tests/bounds_loop_project/` + `tests/run_bounds_loop_test.sh`
(3 positives, 5 controls incl. the pre-leak pin and
a for_neg pin whose b[-1] is checked, so ASan would
fail the run if it were wrongly raw). Suites green
from clean (Come exit 0, "all green"; co_SQLite 7/7).
Bench (checksums identical): ~0.65 s, essentially
unchanged from step 1's ~0.66 s -- the tokenizer's
hot path is tk_byte calls, not loop-guarded direct
accesses, so loop guards do not move it either.
FOUND DURING STEP 2 (pre-existing, not a step-2
bug, filed as finding 38): Come's `i < length()`
compiles to a C unsigned comparison (`length()`
returns uint32_t), so a negative `i` makes the cond
false when Come semantics say it is true. The
for_neg control therefore cannot assert its runtime
sum (its loop never runs); its function is kept
uncalled in the fixtures and pinned checked by
generated C only. The fix needs a design (casting
lengths to int at comparisons, or an explicit idx
>= 0 check in the macro if count ever becomes
signed) and is not folded into step 2.

## Step 1 design (superseded by the DONE section above; kept for the soundness record)

Target: an array/string access `b[i]` whose bounds
check is redundant because a dominating guard already
proved `0 <= i < b.length()`. The tokenizer's
`tk_byte` is the canonical case: it guards with
`if (i < 0 || i >= b.length()) { return 0 }` and
then reads `b[i]`, so the access emits a second,
redundant check (a ternary falling back to
`come_oob_slot_*_at`, which records ERANGE). One
check per byte across 43.7M `tk_byte` calls is a
large share of the 19x gap.

Soundness rule (narrow slice): an access may omit
its check iff, on every path to the access, a
dominating guard proves the bound for the SAME
buffer expression and the SAME index expression,
with neither reassigned, grown, or adopted between
guard and access, and with no intervening call that
could reassign them (conservative: any intervening
call to an unknown function voids the proof, unless
the callee is proven pure for these variables).
Accepted guard forms (narrow slice, straight-line
only, same block):
- Early-exit guard: `if (i < 0 || i >= b.length())
  { return / break / continue }` followed by the
  access (tk_byte's shape).
- Positive guard: `if (0 <= i && i < b.length())
  { ... access ... }` with the access in the
  then-branch (and the symmetric `||` early-exit
  form with the access in the else-branch).
NOT in the narrow slice (step 2): loop conditions
(`while (i < b.length()) { ... b[i] ... }`), which
need induction (i changes in the loop); `&&`/`||`
combinations beyond the two forms above; guards in
an outer block with the access in a nested block
(sound in principle, but needs dominance tracking
across blocks -- step 2); any guard over a
different-but-equal expression (e.g. a copy of i).

Fallback (non-negotiable): if the proof fails for
any reason, emit the checked access exactly as now.
A failed proof can only miss an optimization, never
remove a load-bearing check. In particular, the
check is load-bearing (not redundant) whenever the
guard does not dominate, the buffer or index may
have changed, or the access is a lookahead past a
NUL terminator (the tokenizer's `pos + 1` / `pos + 2`
reads rely on the check to return 0 past the end --
those are NOT guard-dominated and must keep their
checks, unless a padded-allocation design lands
first and makes small overshoot safe by
construction, which is a separate step, not this
one).

Verification plan for the implementation turn:
fixture (guarded access emits raw `data[i]` with no
ternary and no OOB slot; unguarded access still
emits both; boundary cases i == len - 1 valid and
i == len caught by the guard, not by a removed
check), ASan, full suites, bench (expect a
measurable tokenizer move from removing one check
per byte; report it, do not assume it).

## Remaining (in order)

- Step 4: Design 1+2 remainder, then Design 5
  frame tiers.
- HELD: explicit unchecked region (needs his call).
- Future (not in the current step list): general
  interval analysis -- arbitrary index arithmetic,
  nested-loop fixpoints, i >= 0 induction for
  while conds of `i < len` alone. Step 2 covers
  loop guards only, not this.
