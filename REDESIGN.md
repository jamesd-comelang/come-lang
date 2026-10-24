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
transient or reject escaping ones. UPDATE 2026-10-24:
Design 1 is done (see below); general views are now
actionable. UPDATE 2026-10-24 (later): views are
done too (see the Design 3 views section below). The ASCII fast path was assessed and
parked: the tokenizer is byte-based, so char-level
O(1) indexing buys it nothing measurable; it may
still help char-heavy code later.

## Remaining (in order)

- Design 5 no-frame tier (DONE 2026-10-24)

Stub trial first: plain push/pop as no-ops (with
pop_to_marker still really popping the function
marker, or markers strand) gave 0.90 s -> 0.60 s,
checksums identical — block/loop frames were a
third of the bench. So the no-frame tier was built
for real, without escape analysis: a block's frame
is skipped only when the frame would be empty.

- Predicate `block_is_clean` (codegen.c): mirrors
  frame_walk_clean at codegen time against live
  local tables (the analysis tracker is stale by
  then). Clean = scalar-only locals, plain `=`
  only, `+` only on provably scalar operands,
  calls only to frameless functions, methods only
  length/bytesize/owner or same-module frameless
  calls. String/aggregate literals, break/continue,
  spawn/yield, switch, non-frameless calls all
  disqualify.
- Why it is sound: lexical depths are unchanged
  (scope_depth_push/pop still run), so break
  targets and variable scopes resolve exactly as
  before; pop_to_depth pops "everything above an
  index", which stays correct when the missing
  frames are empty. Buffer variables referenced
  inside the block are declared outside it (a
  buffer declaration anywhere in the subtree
  disqualifies), so their adoption targets, at
  depths shallower than the block, are unaffected.
  Returned buffers adopt to caller_ctx, a
  marker-based lookup, never to a skipped block.
- Emission: AST_BLOCK, WHILE, FOR, DO_WHILE
  bodies skip push/pop when clean; SPAWN bodies
  left alone (conservative). The C braces stay.
- Test: tests/run_cleanblock_test.sh +
  tests/cleanblock_project/ (output + ASan +
  generated-C push count == 3: dirty loop,
  break loop, break if; clean loop, clean if,
  and outer-buffer-assign if all skip).

Results (same bench, checksums identical):

- Faithful + A4 + lookup: 0.90 s -> 0.66 s.
- Faithful + A4 + lookup + clean blocks +
  generated-only LTO: ~0.52 s. Gap to C
  (0.033 s) now ~16x, from ~115x at the start
  of perf work.
- Suites green from clean, 0 warnings: Come
  67 passed, 0 failed (plus the new cleanblock
  e2e); co_SQLite 7 passed, 0 failed.

The checkpoint and full-frame tiers of Design 5
still need escape analysis (a checkpoint is an
arena pointer save/restore), so they wait on
Design 1+2.

## Remaining (in order)

- Design 1: escape analysis (DONE 2026-10-24)

May-escape fixpoint over functions and locals, in
codegen.c, run in analyze_module alongside
frameless/mayfail. A buffer parameter escapes if
it is returned (buffer return type), stored in a
module variable, an instance field, or a container,
passed to a function whose corresponding parameter
escapes, captured by a coroutine (spawn/yield),
re-parented by chown, or flows through a local
that escapes (assignment and dup(0) edges, backward
propagation). Unknown and cross-module callees
escape their identifier arguments; container
mutators (put/append/insert/set/cpy) escape theirs.
Pure methods (length/bytesize/owner) and other
runtime methods borrow. Scalars are never dumped.
New driver command `come escapes <file.co|dir>`
prints one line per buffer parameter:
"module.fn(param): escapes|transient".

- Test: tests/run_escapes_test.sh +
  tests/escapes_project/ (15 classifications,
  including the edge cases: keep/transient via a
  non-escaping local, pass_through/escapes via the
  interprocedural fixpoint, dup_copy/transient via
  the deep-copy distinction, reparent(t)/transient
  via receiver-only chown). The fixture also
  passes `come check`, so it is compilable Come.
- Application: `come escapes` on the co_SQLite
  tokenizer (directory module) gives 21 transient
  and 1 escapes: open(sql) — the cursor retaining
  its input, which is its job. tk_scan's b/sql/kw,
  tk_keyword_in's m/word, and get_token's sql are
  all transient (borrowed, not retained).
- Suites green from clean, 0 warnings: Come
  67 passed, 0 failed (plus the new escapes e2e);
  co_SQLite 7 passed, 0 failed. Bench unchanged
  (~0.61 s vs 0.66 s, noise): no codegen change
  this turn, by design — the analysis is the
  prerequisite, not a speedup by itself.

What this does NOT do: arenas (Design 2) are still
a runtime change (bump allocator, promotion on
adopt/chown/dup, checkpoints) — the analysis it
needs is done, but the runtime is not. Spawn/yield
rules are implemented (over-approximate, walk the
subtree for identifiers) but not covered by the
fixture (no bare spawn/yield example to copy);
they can only add escapes, never remove, and the
walk is null-checked.

## Remaining (in order)

- Design 3: general string views + byte view
  (DONE 2026-10-24, except the NUL-scanner audit
  below)

New methods `.view(start, end)` (character indices,
like substr) and `.byte_view(start, end)` (byte
indices): zero-allocation headers over the parent's
block (count = bytes, cap = 0, ctx = parent's ctx).
Soundness is by promotion, not by tracking: a
compiler flag (`g_view_promote`, reset at every
statement boundary) is set at every storing
position - declaration initializers, assignments
(including container keys), returns, container and
field stores, call arguments, spawn arguments, and
printf arguments (%s reads to NUL, which a view
lacks) - so a view there compiles to a copy
(substr / byte_range) instead. No view variable
ever exists; a view that survives to codegen is a
temporary consumed within its statement, where its
parent is in scope and therefore alive. Views
compose (a view of a view is a subrange of the
original block), and the view walk is bounded by
the input's byte count (not by NUL, which a view
need not contain at its end).

- Runtime: come_string_view (count-bounded char
  walk, substr's clamping), come_string_byte_view,
  come_string_byte_range (promotion fallback).
- Codegen: view/byte_view added to the string
  method list, the .length() plain-string list,
  expr_come_type (view -> string, byte_view ->
  byte[]), both printf string lists, the mayfail
  pure list, and the clean/frameless walker
  allowlists (views allocate nothing).
- Companion fixes the testing forced (not assumed):
  a struct temporary passed to fprintf %s crashes
  (variadic ABI), so printf arguments promote
  views to copies; come_string_len/cmp/casecmp
  walked by NUL and gave parent-suffix answers for
  non-suffix views - now count-bounded, which is
  identical for owned strings (NUL-terminated, no
  embedded NUL) and correct for views.

- Test: tests/run_view_test.sh +
  tests/view_project/ (17 checks: temporary views,
  promotions (stored, returned, map/array stored,
  call argument, printf), UTF-8 views, nested
  views, byte views). Generated C is pinned to
  contain BOTH forms (view/byte_view for
  temporaries, substr/byte_range for promotions),
  and the run is ASan-clean (a missed promotion
  would read freed parent data).
- Suites green from clean, 0 warnings: Come
  67 passed, 0 failed (plus the new view e2e);
  co_SQLite 7 passed, 0 failed. Tokenizer bench
  unchanged (~0.62 s, checksums identical): the
  tokenizer does not use views, so this step moves
  no bench number by design. Micro-benchmark
  (2M-iteration compare loop, same compiler):
  view 0.12 s vs substr 0.29 s (~2.4x), both
  correct.

Known limitation, stated plainly: other
NUL-scanning string operations (find, split, chr,
regex, numeric parse, and friends) still read to
the parent's NUL for non-suffix views, so they
return parent-suffix results, not view results.
Views are correct for ==, !=, length, byte
indexing, nesting, and every promotion path. The
count-bounded audit of the remaining string
functions is the follow-up (story-sized, not done
here). Second precision loss: call arguments
always promote, even transient ones (a per-arg
escape-registry lookup would recover transient-arg
views).

## Design 2, phase 1 (DONE 2026-10-24)

- Arena scopes for temporary string concats;
  general transient-value arenas (below) still
  pending.

Chunked bump allocator in the runtime (memory.c):
4 KB blocks, never realloc'd (a realloc would
stale pointers held by earlier temporaries in the
same statement), so a new block links when the
current one fills; oversize requests get an
exact-size block. The arena lives in the scope
stack, so each coroutine has its own. Each frame
records its (block, offset) at push; popping
rewinds the checkpoint block and frees blocks
after it - bulk free for the frame's temporaries,
no per-value bookkeeping. The stack-empty pop
frees the whole arena.

Codegen (codegen.c): a string `+` in a temporary
position (the storing-position flag clear) emits
come_string_concat_arena; in a storing position
(flag set) it keeps the talloc concat. The flag
is the one from Design 3 views, now the general
"storing position" flag for both features.
expr_come_type gained a BINARY_OP "+" case
(stringish operands -> "string"), so .length()
and friends on a concat temporary resolve to the
string forms - and so concat == variable now
compiles at all (it follows the spec's hybrid
==: buffer-buffer is identity, so it is false
for two distinct blocks even with equal content;
content comparison is the buffer-vs-literal path).

Safety nets, so a missed flag degrades to a copy
instead of corruption: adoption sites for strings
(emit_adopt_value's string case, map-put keys and
string values) check come_arena_contains first
and promote to a talloc copy instead of
re-parenting an arena pointer (which is not a
talloc chunk). come_string_free on an arena
string is a no-op (the arena reset frees it).

Test: tests/run_arena_test.sh +
tests/arena_project/ (12 checks: temporary
concats, nested concats, stored/returned/call-arg
/map/array concats, a >4 KB nested concat that
forces new arena blocks within one statement
(checked by content via views against literals,
not by == against a buffer, which is identity),
loop temporaries). Generated C is pinned to
contain both concat forms plus the adoption net,
and the run is ASan-clean.
Suites green from clean, 0 warnings, twice in a
row: Come 67 passed, 0 failed (plus the new
arena e2e); co_SQLite 7 passed, 0 failed.
Tokenizer bench unchanged (~0.66 s, checksums
identical): the tokenizer does not concat.

Micro-benchmarks (same program, new compiler vs
the pre-arena af25c54 compiler): a 1M-iteration
single-concat compare loop ties (0.08 s both -
loop overhead dominates, and glibc's small-block
cache is already fast); a 5-concat chain wins
1.7x for the arena (0.13 s vs 0.22 s). The
large-string case is unmeasured (the old
compiler cannot compile .length() on a concat;
the expr_come_type fix enables it going
forward).

Bug the benchmarking caught: an empty-checkpoint
pop freed every block, so a loop whose iterations
each start from an empty arena re-malloc'd a
block per iteration - slower than talloc (0.12 s
vs 0.08 s). Fixed by keeping the first standard
block, rewound, for reuse (oversize first blocks
and later blocks are still freed; the stack-empty
pop frees the kept block at the end).

Drive-by fix, exposed by this test's flakiness:
the incremental .ccache check used strict > on
1-second mtimes, so two different files sharing
a basename (every e2e fixture is main.co) built
in the same second collided - the second build
reused the first's generated C. Changed to >=
(safe: an unchanged source is always strictly
older than the cache generated from it). This
also fixes the known same-second staleness when
the compiler itself is rebuilt.

Known limits: arenas cover temporary concats
only. Variables, arrays, maps, and general
transient values still use talloc; those need
value-level escape analysis plus dup(0) aliasing
handling (below). Bare literals holding arena or
view elements: the map-put net covers map puts;
array-literal element adoption is untested.

## Design 3 follow-up (DONE 2026-10-24)

Count-bounded audit of the NUL-scanning string
functions, so operations on non-suffix views stay
within the view instead of reading into the
parent (which gave parent-suffix answers: find
found substrings past the view, rchr returned
parent offsets, count counted parent occurrences,
substr ran past the view, regex matched to the
parent's end, tol parsed parent digits).

Fixed in src/string/string.c (receiver side):
chr (memchr), rchr (backward scan), find and
count (shared bounded memcmp search), isascii
(count loop), substr / at / wchar_at (char walks
bounded by the byte count), replace and split
(bounded search, bounded segments; the empty-
pattern no-op copies are bounded too), and the
regex family plus tol/tod (copy-then-scan: a
NUL-terminated copy of exactly the receiver's
bytes, owned by the current scope so loop
iterations don't accumulate it, then the existing
NUL-based logic runs on the copy - match offsets
relative to it match offsets within the view).
Already bounded, no change: memchr, rfind,
isdigit/isalpha/isalnum/isspace, upper/lower,
repeat, join, dup, trim/ltrim/rtrim (receiver
side), byte_array, utf8.

Test: tests/run_view_ops_test.sh +
tests/view_ops_project/ (17 checks, all on view
temporaries - storing a view would promote it to
a copy and miss the bug - plus owned-string
regression controls), and a C-level pin in
tests/test_string.c for view indexing (Come []
needs a named variable, so a view temporary
cannot be indexed in Come at all). ASan-clean;
suites green from clean, 0 warnings; co_SQLite
7/7, bench unchanged (~0.68 s).

Known limit, stated plainly: pattern arguments
(sub/sep/old/new/cutset/pattern) are bare C
strings (->data), so a VIEW passed as a pattern
over-reads into its parent (memory-safe, wrong
pattern). Fixing that needs bounded (data, len)
pattern arguments (a codegen + runtime signature
change) - a follow-up story, not done here. The
common case (view receivers, literal or owned
patterns) is now correct.

## Design 6 (DONE 2026-10-24, verdict: correct but no tokenizer win)

`come build --single-tu` concatenates every user
.c file into one translation unit (build/
single_tu.c, in link order so global init order
matches a separate build), compiles it once, and
links it with the runtime (which stays separate).
Per-file compiles are skipped in this mode (their
objects would be unused). Two constructs cannot
simply repeat in one TU, and are handled in the
driver's build_single_tu (src/core/come_compiler.c):
come_strerror (a static helper codegen emits in
every file - later identical copies skipped), and
struct definitions (the exported surface repeats
them per importer - identical bodies, compared
whitespace-insensitively and excluding the typedef
name, are skipped; a named typedef-struct still
emits a forward typedef so its typedef name stays
defined). A DIFFERENT definition of the same tag
is refused loudly (die, naming the struct) -
struct tags are plain C names, not
module-mangled, so two modules defining e.g.
struct Point with different layouts cannot share
a TU. (Cosmetic limit: the refusal names the
struct but not the modules, since both surfaces
live in the importer's file.) Single-line
definitions are handled too (tuple returns are
"typedef struct { ... } Name;" on one line);
functions returning structs and variables of
struct type are excluded by requiring the tag to
be followed by '{'.

Measured on the tokenizer (checksums identical):
separate -O2 0.54 s, single-TU -O2 0.57-0.59 s,
separate -O2 -flto (gen-only) 0.56-0.59 s. Verdict:
user-only single-TU does NOT beat separate -O2,
and neither does gen-only LTO - the remaining
bottleneck is runtime calls, not user-to-user
inlining. The next lever, if inlining is to help
at all, is runtime inlining (full LTO with the
runtime built -flto, or single-TU including
runtime sources) - a follow-up measurement, not
assumed. Correctness: types + consts projects
(structs, tuples, consts, stateful vault,
cross-module calls) produce identical output
under --single-tu; the Point-collision case
refuses loudly; full suites green from clean
with the flag off (normal builds untouched).

## String dummy-src over-read fix (DONE 2026-10-24)

ASan (under the -flto global layout) caught a
pre-existing over-read: five allocate-then-fill
sites passed "" as the source to
come_string_new_len with a real length
(repeat, replace, join, regex_replace,
vsprintf), and new_len memcpys len bytes from
its source whenever it is non-NULL - so e.g.
replace("l","L") on "hello" read 5 bytes from a
1-byte "" literal. It passed ASan before only
because the old global layout did not place a
poisoned redzone there. Fix: pass NULL instead
of "" at all five sites (new_len's NULL branch
zero-fills, which is what these callers want -
they overwrite every byte anyway). No remaining
dummy-"" new_len calls in string.c. Pre-existing
before the NUL-scanner audit, which preserved
these allocation lines untouched.

## Design 6 follow-up (DONE 2026-10-24, verdict: full LTO wins ~33%)

Runtime objects are now built with -flto, so a
user LTO link can inline runtime calls into user
code (which gen-only LTO and user-only single-TU
cannot do, since they lack runtime LTO info).
-flto is added only in the runtime modules own
Makefiles (mem/map/array/coro append it, incl.
link.o via coro; std/string include it in their
CFLAGS= lines); Makefile.inc itself is unchanged,
so the compiler core, which also includes it,
builds without -flto. (An earlier attempt edited
Makefile.inc directly and leaked -flto into the
core build; reverted and scoped per-module.)
Measured on the tokenizer (checksums identical):
control (-O2, -flto runtime, no LTO link)
0.55-0.60 s vs full LTO (COME_CFLAGS="-O2 -flto",
runtime + user code + link all -flto) 0.37-0.39 s
- about a third faster, distributions
non-overlapping (one 0.51 s cold first run
excluded). Honest scope: runtime -flto alone
does not speed up default builds (the control
matches the old 0.54 s baseline within noise);
the win needs the user to opt into LTO via
COME_CFLAGS="-O2 -flto". So this change ENABLES
an opt-in ~33% win at no measured default cost.
Suites green from clean with the -flto runtime
(Come 67/67, co_SQLite 7/7).

## Design 2 remainder - general transient-value arenas (DESIGN 2026-10-24, not implemented)

Phase 1 covers temporary string concats only (the
storing-position flag routes temporaries to the
bump arena, storing positions to talloc). Named
locals, arrays/maps, and non-concat values still
use talloc. This is the design the implementation
turn starts from; it is deliberately not code yet,
because the soundness questions below decide
whether a value may be arena at all, and a wrong
answer there is a dangling pointer, not a slowdown.

A buffer-typed local is transient when it never
escapes its scope: not returned, not stored in a
module variable, instance field, or container,
not passed to an escaping callee parameter, not
captured by spawn/yield, not flowed (via plain =
or dup(0)) into an escaping local. Same escape
rules as Design 1 params, extended to locals,
OR-ed over all uses: reassignment means if ANY
use escapes, the local escapes. Unknown callees
and unanalyzed constructs default to escaping.

dup(0) aliasing (COIL-135: dup(0) shares the
block, dup() deep-copies, assignment shares and
forks on growth): an alias edge runs both ways.
If any alias of a value escapes, the value must
be owning (talloc), not arena - freeing an arena
block while an alias still lives dangles the
alias. V1 rule: a transient local with an
escaping dup(0) alias is treated as escaping
(stays talloc). Promotion-at-escape for dup(0)
aliases is v2, not v1.

Loop accumulation, the sharp edge: arena values
live until their scope frame pops. A transient
local declared in a loop body is freed per
iteration (frames pop per iteration - good). A
transient local declared OUTSIDE a loop but
assigned inside accumulates one arena value per
iteration until the outer scope pops -
catastrophic for hot loops at tokenizer scale.
V1 rule: outer locals assigned inside a loop
stay talloc (conservative exclusion). This
costs little for the tokenizer (its hot temps
are loop-body locals or phase-1 temporaries),
but that must be measured, not assumed.

V1 scope is string locals only, extending phase
1s string handling: transient string locals
allocate via the bump arena, and the existing
adoption net (arena -> talloc promotion on
adopt/store/return) extends to locals, so a
missed classification degrades to a copy, never
to corruption. Array/map buffer promotion is v2:
a container that escapes must promote its header
AND any arena elements, or the elements dangle,
and that header/element split needs its own
design.

Soundness invariants (non-negotiable for the
implementation): (1) no arena value is ever
adopted/re-parented - adoption promotes to
talloc; (2) no arena value outlives its scope
frame - transient = never escapes + frame-pop
frees arena; (3) dup(0): any escaping alias
makes the value owning; (4) the loop rule
above; (5) any analysis uncertainty defaults to
escaping (talloc), never to arena.

Verification plan for the implementation turn:
fixture with transient-local cases (straight-
line, loop-body, outer-loop exclusion pinned via
generated C as no arena alloc for that local,
dup(0) non-escape vs escape, return-escape
promotion, assignment chains), ASan, full suites,
bench. Expect the tokenizer to move little (its
temps are already covered) - measure with a
named-transient-local micro-benchmark, not just
the tokenizer. Design 5 checkpoint/full tiers
still wait on this (they need arena values to
skip); the frame-checkpoint mechanism already
exists from phase 1.

## Remaining (in order)

## Design 1+2, straight-line slice: transient string locals (DONE 2026-10-24)

A string local whose concat initializer never escapes
its scope now stays in the bump arena instead of
talloc. Analysis (src/core/transient.c, new file,
wired into the driver's pre-scan alongside
frameless/mayfail/escapes): a local is transient only
when it is declared `string` (or `var` with a
string-concat init), its init is a concat, the decl is
NOT inside a loop, the name is not a parameter and is
not re-declared, the init does not reference the name,
the name is never assigned again, and every other use
is a whitelisted borrow (concat operand, == / !=
operand, or receiver of length / bytesize / owner /
substr / view / byte_view). Anything else -- return,
call argument, store, map/array/field value, dup/chown
receiver, spawn/yield capture, printf argument --
disqualifies it, and it keeps the talloc path. The
default is "not transient": an unrecognized construct
can only miss an optimization, never dangle.
Codegen change is one condition: at VAR_DECL, a
transient local does NOT set the storing-position
flag, so its init emits concat_arena; declaration
inits get no adoption (COIL-157), so no adoption
surgery was needed, and ASSIGN is untouched (a
transient local is never re-assigned by
construction). Module-level decls always promote (a
module variable outlives any frame). Soundness: the
arena concat is created in the same block as the
declaration, and the local is never stored, returned,
or captured, so no reference can outlive the block's
frame pop. The stringish check is only a registry
filter, not a soundness gate (codegen selects the
string-concat branch by its own types; the flag is
ignored for a numeric "+").
Tests: tests/transient_project/ (main.co combined,
pos.co positives-only, ctl.co controls-only) +
tests/run_transient_test.sh, wired into run_tests.sh.
Pins are per-group, not just file-level: pos.co's
generated C has arena concats and NO talloc concat
(all 5 positives registered); ctl.co's has talloc
concats and NO arena concat (all 8 controls --
reassign, return, call arg, loop decl, map store,
printf arg, dup(0), array append -- correctly
disqualified). ASan clean. Full suites green from
clean (Come 67/67, co_SQLite 7/7, 0 warnings).
Measured: tokenizer bench neutral (pre-transient
0.66-0.68 s vs transient 0.62-0.66 s, checksums
identical) -- the tokenizer's temps were already
covered, as predicted. Named-temp micro-benchmark
(helper with 3 chained transient temps, 2M calls):
0.32-0.34 s -> 0.21-0.22 s (~1.5x). The stale
bench_come binary in co_SQLite/tools/bench/ (~52 s)
is a pre-COIL-158 artifact, not a control -- do not
cite it against transient.
Remaining for Design 1+2: loop-body locals (decl +
assign inside the same loop body, per-iteration
safe), multi-assignment fixpoint (value flow through
reassignment and t -> u chains), first-assignment
locals, printf-argument borrows, and array/map
promotion. Design 5 checkpoint/full tiers still wait
on general arenas.

- Design 2 (remainder): loop-body transient locals,
  multi-assignment fixpoint, first-assignment locals,
  printf borrows, and promotion for arrays/maps.
  (Straight-line string-local slice is done, above.)
- Design 3 follow-up (remainder): bounded pattern
  arguments for string methods (see above).
- Design 5 (remainder): checkpoint / full-frame
  tiers driven by escape analysis. Needs general
  arenas (the frame-checkpoint mechanism now
  exists, but the tiers need arena values to skip).
