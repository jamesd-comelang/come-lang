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
- Step 4: Design 1+2 remainder, then Design 5
  frame tiers. Sliced like step 3: 4a (loop-body
  locals + printf borrows, DONE), 4b
  (multi-assignment init, DONE), first-assignment
  locals (DONE), 4c-arrays (DONE), 4c-maps
  (DONE, see below) first, then 4d (frame tiers).
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

## First-assignment DONE (2026-10-24): transient first-assignment locals (step 4b remainder)

A local whose declaration has NO concat init, but
whose FIRST assignment is a string concat, with all
uses borrows and every assignment sharing the
declaration's loop depth, gets its concat
assignments as direct arena assignments. Analysis
in transient.c (a separate first-assignment
registry; trans_collect restored loop-depth
tracking for declaration and assignment depths,
and records first-assignment concat-ness per name;
shadowed names and params excluded; the rule is
disjoint from init-transient, so a concat-init
local keeps talloc reassignments). Codegen in
codegen.c: the bypass sits in BOTH assignment
paths, because statement assignments (generate_node
AST_ASSIGN) and nested assignments
(generate_expression) have separate _come_v +
adopt emission -- the first run caught only the
expression path, and the statement test failed
until the statement bypass was added. The bypass
clears g_view_promote for the RHS (so the concat
is arena) and emits a plain `s = <arena concat>`
with no temp and no adopt, like a declaration
init; later escaping uses are still caught by the
net at those sites. Test:
`tests/transient_first_project/` +
`tests/run_transient_first_test.sh` (positives:
no-init-literal and literal-init first assignments
plus a multi-assignment case, all direct arena,
NO talloc concat in positives; controls: a
non-concat first assignment and an outer local
assigned in a loop, both talloc via _come_v, NO
direct arena; ASan). Suites green from clean
(Come exit 0, "all green"; co_SQLite 7/7).
Tokenizer bench: ~0.61 s, essentially unchanged
(no first-assignment string locals in its hot path).

## Step 4b DONE (2026-10-24): transient multi-assignment init (reassignment targets allowed)

trans_use_bad no longer disqualifies a reassignment
TARGET (ASSIGN child 0): overwriting the local
orphans its arena init string, which the frame pop
frees anyway, so only escaping uses (return, call
arg other than std printf, store, dup/chown,
spawn/yield) disqualify. A reassignment whose RHS
uses the name badly still disqualifies via that RHS
use. Reassignments themselves stay talloc (they go
through a _come_v temp + adoption); only the
declaration's init is arena. Test:
`tests/transient_multi_project/` +
`tests/run_transient_multi_test.sh` (multi and
multi-self positives: declaration line arena,
reassignment temp talloc; bad-RHS and store
controls: no arena at all; ASan). The old
straight-line ctl lost its reassigned control (c1)
to this step -- it moved to transient_multi as a
positive, and the old ctl + expected output were
updated. Suites green from clean (Come exit 0,
"all green"; co_SQLite 7/7). Tokenizer bench:
~0.60 s, essentially unchanged (no multi-assigned
string locals in its hot path). First-assignment
locals (a local whose FIRST assignment, not its
init, is a concat) are NOT in this cutoff: they
need codegen ASSIGN-time transient handling, not
just an analysis relaxation, so they are the next
slice, not folded in here.

## Step 4a DONE (2026-10-24): transient loop-body locals + std printf borrows

transient.c no longer excludes loop declarations:
a local DECLARED inside a loop body is a candidate
(its frame pops per iteration, freeing its arena
string, and each iteration re-declares it fresh).
An OUTER local assigned inside a loop stays talloc,
but that falls out of the never-reassigned rule
(the assignment is a disqualifying use), not out of
a loop exclusion -- so the old loop gate was pure
conservatism, and removing it is sound (plus the
adoption net promotes any arena value that does
escape, so a miss degrades to a copy, never to
corruption). Std printf arguments (out/err/std.
printf, any argument position) are whitelisted
borrows: printf only reads its arguments. The
receiver check is text-only, like codegen's own
printf branch -- std.out parses as a field node,
not an identifier, so a type check missed it (found
by the first failing run, fixed same turn). A
user-defined printf on another receiver is NOT
whitelisted (still a disqualifying call arg).
Test: `tests/transient_loop_project/` +
`tests/run_transient_loop_test.sh` (while/for loop
locals, printf, loop+printf positives with arena
pins; outer-reassign and general-call-arg controls
with talloc pins; ASan). The old straight-line
ctl lost two controls to this step (its loop-body
c4 and printf c6 are now transient by design);
they moved to transient_loop as positives, and the
old ctl + expected output were updated accordingly
(including c6's printf(r6) "hello world" line).
Suites green from clean (Come exit 0, "all green";
co_SQLite 7/7). Micro (1M loop iterations, one
string local per iteration, == borrow): old
compiler (talloc per iteration) ~139 ms, step 4a
(arena per iteration) ~114 ms, 1.22x -- modest,
because the loop body is not alloc-dominated.
Tokenizer bench: ~0.62 s, essentially unchanged
(as predicted -- its hot path is byte reads, not
loop string locals).

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

## Step 4c-arrays DONE (2026-10-24): transient array containers (no eager element promotion)

A separate container registry in transient.c
(codegen_transient_container): an array local
with an explicit array type is transient iff
every use is a built-in method receiver, an
array-access base, or a rebinding target, and
every element-adding store (append/insert/set
method call, index-store assignment) shares the
declaration's loop depth (so an outer container
appended to in a loop is excluded -- its arena
elements would dangle at iteration end). Params
and shadowed names excluded; `var` arrays
excluded (no inferred array type in the
analysis). Codegen suppresses eager element
promotion for transient receivers: the
adopt_elem condition in the method-call emission
(append/insert/set) and the _come_v adopt in
both index-store blocks (fixed + dynamic) in
the statement ASSIGN case. Element escapes via
reads (arr[i] then stored/returned) are still
promoted by the net at those sites. Test:
`tests/transient_cont_project/` +
`tests/run_transient_cont_test.sh` (positives:
append, index-store, loop-body container -- NO
promote and NO scope-adopt anywhere in
positives; controls: return, call arg,
outer-loop store -- promote present; ASan).
Suites green from clean (Come exit 0, "all
green"; co_SQLite 7/7). Tokenizer bench: ~0.60
s, essentially unchanged (no containers in its
hot path, as predicted). Container micro (1M
loop-body array appends of an arena concat):
old compiler ~298 ms vs new ~218 ms = 1.37x.
Maps are NOT in this cutoff (emit_map_put
suppression needs the named-local-only guard
for literal construction) -- 4c-maps next.

## Step 4c-maps DONE (2026-10-24): transient map containers (no eager key/value promotion)

The container registry (transient.c) now records
typed maps ("map{...}"; legacy plain "map"
excluded) alongside arrays, sharing the
never-escapes rule and the scope-depth store
check. Codegen suppresses eager promotion in
emit_map_put for named-local index stores only
(base != NULL): int-value puts drop the key
promote/adopt, string-value puts drop the value
promote/adopt and the key adopt. Literal
construction (base == NULL) always promotes --
it builds a temporary that is then adopted, so
suppressing there would be useless and unsafe.
Test: `tests/transient_map_project/` +
`tests/run_transient_map_test.sh` (positives:
string-map, int-map, loop-body map -- NO
promote and NO scope-adopt anywhere; controls:
return, call arg, outer-loop store -- promote
present; literal case pinned function-level to
still promote; ASan). Two soundness fixes found
by this work, both in this cutoff: (1) map
adoption now also adopts string value CELLS, not
just their bytes -- returning a map previously
freed its value cells on frame pop
(use-after-free, caught by the fixture under
ASan); (2) scope depth now counts enclosing
BLOCKs, not just loops, for first-assignment
strings AND containers -- a value built in an
inner block and stored into an outer container
dies at block end, and the old loop-only depth
called it safe (caught by the map scope test,
which stores an inner-block string into an outer
map; the same hole is closed for arrays and
first-assignment strings). Suites green from
clean (Come exit 0, "all green"; co_SQLite 7/7).
Tokenizer bench: ~0.60 s, essentially unchanged
(no maps in its hot path, as predicted). Step 4
remaining: 4d frame tiers only.

## Step 4d DONE (2026-10-24): checkpoint frames (Design 5 checkpoint tier)

Blocks whose allocations are all arena now get
a checkpoint frame (bump rewind only, no talloc
frame to allocate or free) instead of a full
frame, at all four block sites (bare BLOCK,
WHILE, FOR, DO_WHILE bodies; IF branches ride
the bare BLOCK case). Runtime:
`come_scope_push_checkpoint()` (src/mem/memory.c,
declared in src/include/mem/talloc.h) records
only the arena checkpoint in a stack slot with a
NULL ctx; the ordinary pop rewinds the arena
and no-ops the frame free. Codegen:
`block_is_checkpoint()` / `checkpoint_walk()`
(conservative, mirroring clean_walk): scalars
and transient-string concats of variables are
allowed (chained concats via a flag); anything
that could allocate talloc in the block
(string/aggregate literals, non-transient
concats, container stores, non-pure methods,
non-frameless calls, printf/spawn/yield/switch,
break/continue) disqualifies, and scalar `+`
operands are allowed via a scalar-operand
check. Test: `tests/checkpoint_project/` +
`tests/run_checkpoint_test.sh` (2 checkpoint
pushes for a bare block and a loop body with
variable-only transient concats, 2 full pushes
for a literal-operand concat and a scope-unsafe
outer store; output + ASan). Suites green from
clean (Come exit 0, "all green"; co_SQLite
7/7). Tokenizer bench ~0.59 s, essentially
unchanged (its blocks were already clean, as
predicted). A 1M-iteration variable-concat loop
micro: pre-4d ~43 ms steady vs 4d ~36-44 ms --
a small win at best, within noise for this
shape (concat + length walk dominate the frame
saving). Step 4 is COMPLETE. Remaining is the
exhaust work (see below).

## Interval analysis DONE (2026-10-24, exhaust safe slice): single-bound while proofs

Profiling first: the tokenizer's hot path
already has ZERO checked array reads (tk_byte's
read is raw via step 1, the rest is calls/maps),
so no bounds work can move it -- this slice is
a general-Come win, not a tokenizer win. A
while with a single-bound condition (`i <
b.length()`) and i provably nonneg at entry now
proves (b, i) at body top (step 2 only covered
positive-form `0 <= i && i < len` and counted
for). Nonneg is threaded through block children
(saved/restored per block, cleared at function
entry): gained by int i = <nonneg constant /
length / nonneg identifier>, preserved by i++
and i = i + <nonneg>, lost by any other
assignment and by i--. Reads before any
modification to i in the body are raw; reads
after one are checked, because the void rule
now also voids on i++/i-- (a soundness fix:
step 2's bodies had the same hole for i++ inside
the body). The bounds_loop cond-only control
moved to positives (it is now proven); its
other controls are unchanged. Test:
`tests/interval_project/` +
`tests/run_interval_test.sh` (3 positives raw,
2 controls checked, output + ASan). Suites
green from clean (Come exit 0, co_SQLite 7/7).
Tokenizer bench ~0.59-0.64 s, essentially
unchanged, as profiled. Remaining exhaust work
below is now the main lever: baselines,
unchecked, port restructuring.

## Rust/Go baselines DONE (2026-10-24, measured, not guessed)

Faithful Rust + Go ports of sqlite3GetToken
(Come tokenizer logic, TK_* values and the 147
keyword codes copied from the Come sources, no
hand transcription) in ~/workspace/baselines/
(rust/main.rs + consts.rs, go/main.go +
consts.go; workload.sql copied from
co_SQLite/tools/bench/). Both reproduce the
bench exactly on identical work (4,000 lines,
20 passes): 2,530,480 tokens, typesum
285119860, lensum 7962360. Timings (shell
date, 3 runs, same VM):
- C (cc -O2, reference): ~33 ms.
- Rust (rustc -C opt-level=3, std HashMap):
  ~80 ms (81/79/79).
- Go (go build, default): ~131 ms
  (128/134/131).
- Come (default, no --lto): ~622 ms
  (633/599/635).
- Come (--lto): ~380 ms (earlier).
So the real targets: beat Go needs < 131 ms
(4.7x from Come default, 2.9x from --lto);
beat Rust needs < 80 ms (7.8x / 4.8x). The
guesses (Rust 40-80, Go 60-150) are retired --
Rust landed at the top of its guess, Go inside
it. Toolchains installed user-local (no sudo):
~/sdk/go (1.23.0), ~/.cargo (rustc 1.99.1).
The gap is dominated by per-byte/per-token
function calls and map lookups, not bounds
checks (already zero in the hot path) -- which
is why unchecked + port restructuring are the
remaining levers, in that order.

## Step 4c design (2026-10-24, not yet implemented)

Verified first, because it changes the shape of
the step: container adoption ALREADY recurses
into elements. emit_adopt_value for a dynamic
array adopts the data block and then each buffer
element (codegen.c, array branch), and map puts
promote arena keys/values eagerly (emit_map_put).
Array append/insert/set adopt elements eagerly
(codegen.c, adopt_elem), and array index stores
adopt via _come_v + emit_adopt_value. So the
REDESIGN.md worry -- "a container that escapes
must promote its header AND any arena elements,
or the elements dangle" -- is already closed by
the net: an escaping container's elements are
promoted with it, and a stored element is
promoted at store time. 4c is therefore NOT a
safety fix; it is the performance side:
transient containers.

Rule (strict never-escapes, mirroring the string
rule but stricter, because a callee could retain
a container): a container local is transient iff
every use is a built-in method receiver
(append/insert/set/index/length/contains -- the
built-ins do not retain the receiver) or an
index read (arr[i]; the ELEMENT's own net still
catches element escapes at their use sites), and
NO use is a return, a store (to outer/module/
field/map/array), a call argument (any function
could retain it), an assignment RHS (aliasing),
a dup(0)/dup, or a spawn/yield capture.
Assignment TARGETS are allowed (rebinding
orphans the old container, like string
reassignment; the container still never escapes
regardless of whether its current value is arena
or talloc). Loop rule: like strings, an outer
container assigned in a loop is excluded (the
loop-accumulation argument applies to its arena
elements too); a loop-body container is allowed
(per-iteration frame).

Codegen: suppress eager element promotion at
the store sites when the receiver is transient:
(1) array append/insert/set (adopt_elem in the
method-call emission), (2) array index stores
(the _come_v + adopt blocks for fixed and
dynamic arrays in the statement ASSIGN case),
(3) map puts (emit_map_put key/value promote)
-- but only when the map is a named transient
local, NOT for map-literal construction (whose
pairs go into a temporary that is then adopted).
The container's own header stays as-is (it is
already a local header; no arena container
allocation is needed for this slice -- the win
is skipping per-element talloc copies, not
moving the header). Element escapes via reads
(arr[i] then stored/returned) are still promoted
by the net at those sites, so a missed container
classification degrades to a copy at the element
level, never to a dangle -- except the suppressed
eager promotion itself, which is why the
never-escapes rule must be complete (no call
args at all, unlike strings, where printf args
are whitelisted).

Split: 4c-arrays first (append/insert/set +
index stores, string[] and T[]), then 4c-maps
(emit_map_put suppression). Tokenizer impact is
expected to be nil (it does not build string
containers in its hot path) -- measure with a
local-scratch-container micro, not the tokenizer.

## Remaining (in order, under the 2026-10-24
  "exhaust everything" authorization: unchecked
  region + port restructuring both included)

- Unchecked region, opt-in only, design +
  implementation.
- Port restructuring of the co_SQLite tokenizer
  (table-driven, bulk reads, raw iteration).
- HELD: explicit unchecked region (needs his call).
- Future (not in the current step list): general
  interval analysis -- arbitrary index arithmetic,
  nested-loop fixpoints, i >= 0 induction for
  while conds of `i < len` alone. Step 2 covers
  loop guards only, not this.
