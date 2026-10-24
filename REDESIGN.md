# Foundation redesign — branch log

Branch `redesign` (from `perf-experiments` @ 1d8ac57). Goal: verify
the six foundation redesign proposals from the 2026-10-24
discussion by implementing them on this branch, one at a time,
each gated by the full Come suite + co_SQLite suite + bench.
Nothing here merges to main without a story decision.

## A4 — zero-cost errors (DONE 2026-10-24)

Per-statement ERR arms are gone. Emission now arms only before
statements that may set an error, decided by a may-fail
fixpoint over the call graph (same machinery style as A5
framelessness) plus a statement predicate:

- may-fail primitives: typed map reads (come_map_get_e sets
  ENOENT on miss), string index (come_string_wchar_at_e sets
  ERANGE), failing runtime methods (regex family, at, tol,
  tod; array insert/remove; FILE ops; getenv), instance
  method calls (destroyed-instance guard), division/modulo,
  spawn/yield.
- NOT may-fail: array index reads (OOB slot macro records
  its own location via #line), plain map-index writes
  (puts never set ERR; key/value expressions still walked),
  pure methods (length/bytesize/owner/type/dup/chown, lower,
  upper, substr, find, split, byte_array, append, clear,
  resize, free, slice, put, str), ERR methods (fatal
  carries explicit site args; set/no/str/where/depth read).
- Calls resolve through the per-module may-fail registry
  (same-module, cross-module if registered, unknown module
  arms on doubt). Over-approximation is safe (extra arm);
  under-approximation would record a stale location, so
  every rule arms on doubt. The #line directive stays
  unconditional (OOB slots read their location from it).
- Semantics: the recorded location becomes the innermost
  armed statement (the failing statement itself when the
  predicate covers it), degrading gracefully to the nearest
  enclosing may-fail call site on any predicate gap. The
  old call-site semantics is the fallback, not the rule.
  This picks innermost for the branch trial; call-site
  remains an option for main.

Results (bench: 4,000 SQL lines, 20 passes, 2,530,480
tokens, checksums typesum=285119860 lensum=7962360):

- Arm sites in bench generated C: 1,243 -> 39 (97% fewer).
- Faithful scanner, default flags: 2.73 s -> 2.01 s (~26%).
- Faithful + generated-only LTO: ~1.71 s. (Old combo
  1.40 s was full LTO + stubbed arms; full LTO with real
  A4 still to measure.)
- Suites green from clean, 0 warnings: Come 67 passed,
  0 failed; co_SQLite 7 passed, 0 failed.

Cover-test pin moved 9 -> 8 (tests/run_cli_test.sh):
with arms gone, gcov no longer counts line 8 (the
unexecuted second if) as hit through a partial-block
artifact, and the closing braces are no longer
executable lines. The new report (8-11 uncovered) is
the lines that genuinely never execute on the pick(20)
path; the old pin was the artifact. Not a weakening:
the test still pins the uncovered region, now accurately.

## Remaining (in order)

- Design 3 — keyword-path verification (DONE 2026-10-24)

Views alone would not have fixed the keyword path: `lower()`
must allocate by nature (it changes bytes), and the branch
also paid a char-index walk plus a substr alloc. So the
verification uses the sound primitive the path actually
needs, instead of general views:

- New runtime `come_map_get_lower_e(map, bytes, pos, len)`
  (src/map/map.c): DJB2 over the range lowercased
  (ASCII) on the fly — the same hash as the stored
  lowercase keys — then linear-probe with a lowercased
  compare. No allocation. Miss sets ERR (ENOENT) like
  get_e; out of range sets ERR (ERANGE). Contract:
  matches keys stored in lowercase; a stored uppercase
  byte can never match (its hash differs).
- New typed-map method `m.lookup_lower(bytes, pos, len)`
  (codegen dispatch + may-fail list, so A4 arms its
  statements): returns the value with ERR set on a miss,
  mirroring a typed map read, for int and string values.
- co_SQLite `redesign` branch: tk_scan's keyword branch
  now does `kw.lookup_lower(b, pos, j - pos)` with the
  ERR dance, replacing the tk_char_index walk + substr
  + lower + map read. `sql` is now unused in tk_scan
  (signature kept so callers don't change; dropping it
  is follow-up cleanup if the path is adopted).

Results (same bench, checksums identical):

- Faithful + A4: 2.01 s -> 0.90 s with the lookup.
- Faithful + A4 + generated-only LTO: ~0.75 s.
  Gap to C (0.033 s) now ~23x, from ~115x at the start
  of perf work and ~42x at A5+combo.
- Suites green from clean, 0 warnings: Come 67 passed,
  0 failed; co_SQLite 7 passed, 0 failed (differential
  vs C, upstream tokenize.test, fuzz all cover the new
  path). fastscan (A3 trial) untouched; it can adopt
  lookup_lower too.

What Design 3 did NOT do, and why: general substr views
and byte views are unsound without escape analysis — a
view dangles when its parent's scope pops first, and
return adoption would corrupt the parent by re-parenting
its data chunk. So general views ride on Design 1
(escape analysis), which will either prove views
transient or reject escaping ones. The ASCII fast path
was assessed and parked: the tokenizer is byte-based,
so char-level O(1) indexing buys it nothing measurable;
it may still help char-heavy code later.

## Remaining (in order)

- Design 1+2: escape analysis + arena scopes for
  non-escaping values. A4's fixpoint is the reusable
  machinery. Unlocks sound general views (above).
- Design 5: frame tiers (no frame / checkpoint / full
  frame) driven by escape analysis. Needs arenas first.
- Design 6: cross-module inlining / single-TU codegen.
  LTO already covers most of this; lowest priority.
