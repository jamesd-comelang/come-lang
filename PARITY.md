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
  (design below; implementation next).
- Step 2: generalization -- loop guards and range
  analysis (counted loops, induction, bulk-check
  plus raw inner loop as the loop case).
- Step 3: runtime intrinsics for hot ops (length,
  small compares, byte reads as inline code) and
  whole-program TU if intrinsics alone fall short.
- Step 4: Design 1+2 remainder (loop-body locals,
  multi-assignment fixpoint, array/map promotion)
  then Design 5 frame tiers.
- HELD OUT (needs an explicit call, not covered by
  "do them"): an explicit unchecked/unsafe region.
  It is the only design that guarantees 1x, and it
  breaks Come's safety promise for that region.

## Step 1 design: guard-dominated bounds-check elimination (DESIGN 2026-10-24, not yet implemented)

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

- Step 1 implementation (narrow slice above).
- Step 2: loop guards and range analysis.
- Step 3: runtime intrinsics, whole-program TU if
  needed.
- Step 4: Design 1+2 remainder, then Design 5
  frame tiers.
- HELD: explicit unchecked region (needs his call).
