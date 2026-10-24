#!/bin/bash
# COIL-138: the developer commands - come check, come run,
# come test [--cover] [--race] [--bench], come fmt.
#
# Each check builds its own fixture project in a scratch
# dir and drives the real binary, the way a user would.
set -e
cd "$(dirname "$0")/.."
COME=./build/come
COME_ABS=$PWD/build/come
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT

fail() { echo "CLI: $1"; exit 1; }

# ---------- fixtures ----------
mkdir -p "$SCRATCH/proj/t" "$SCRATCH/proj/src/deep/t" "$SCRATCH/empty"
cat > "$SCRATCH/proj/t/01-ok.co" <<'EOF'
module ok1
import std

int main() {
    std.out.printf("PASS one\n")
    return 0
}
EOF
cat > "$SCRATCH/proj/src/deep/t/02-ok.co" <<'EOF'
module ok2
import std

int main() {
    int a[] = {1, 2, 3}
    if (a.length() != 3) { return 1 }
    std.out.printf("PASS two\n")
    return 0
}
EOF
# A project module of the tested project, with tests that
# import it - one in the module's own t/, one in the top
# t/. Both only resolve when the child builds anchor their
# src/ lookup on the tested project (COIL-150).
mkdir -p "$SCRATCH/proj/src/greeter/t"
cat > "$SCRATCH/proj/src/greeter/greeter.co" <<'EOF'
module greeter

export (hello)

string hello() {
    return "hi"
}
EOF
cat > "$SCRATCH/proj/src/greeter/t/04-mod.co" <<'EOF'
module tmod
import std
import greeter

int main() {
    if (greeter.hello() != "hi") { return 1 }
    std.out.printf("PASS four\n")
    return 0
}
EOF
cat > "$SCRATCH/proj/t/05-mod.co" <<'EOF'
module tmod2
import std
import greeter

int main() {
    if (greeter.hello() != "hi") { return 1 }
    std.out.printf("PASS five\n")
    return 0
}
EOF
cat > "$SCRATCH/runargs.co" <<'EOF'
module runargs
import std

int main(string args[]) {
    if (args.length() > 1) { std.out.printf("got:%s\n", args[1]) }
    return 7
}
EOF
cat > "$SCRATCH/bad.co" <<'EOF'
module bad
import std

int main() {
    return nosuchfn(1)
}
EOF
mkdir -p "$SCRATCH/cov/t"
cat > "$SCRATCH/cov/t/cov.co" <<'EOF'
module covt
import std

int pick(int v) {
    if (v > 10) {
        return 1
    }
    if (v < 0) {
        return -1
    }
    return 2
}

int main() {
    if (pick(20) != 1) { return 1 }
    std.out.printf("PASS cov\n")
    return 0
}
EOF
# ---------- come check ----------
$COME check "$SCRATCH/proj/t/01-ok.co" | grep -q "check ok" || fail "check did not accept a good file"
if $COME check "$SCRATCH/bad.co" > /dev/null 2>&1; then
    fail "check accepted a bad file"
fi
echo "CLI: check ok"

# ---------- come run ----------
out=$($COME run "$SCRATCH/runargs.co" -- hello) && rc=0 || rc=$?
[ "$rc" = "7" ] || fail "run did not forward the exit code (got $rc)"
echo "$out" | grep -q "got:hello" || fail "run did not forward arguments: $out"
if $COME run "$SCRATCH/bad.co" > /dev/null 2>&1; then
    fail "run accepted a bad file"
fi
echo "CLI: run ok"

# ---------- come test ----------
out=$($COME test "$SCRATCH/proj") || fail "test failed on an all-pass project"
echo "$out" | grep -q "Results: 4 passed, 0 failed" || fail "test summary wrong: $out"
echo "$out" | grep -q "PASS src/deep/t/02-ok.co" || fail "test missed the nested t/ dir: $out"
echo "$out" | grep -q "PASS src/greeter/t/04-mod.co" || fail "test did not resolve the project's own module: $out"
if $COME test "$SCRATCH/empty" > /dev/null 2>&1; then
    fail "test accepted an empty root"
fi
cat > "$SCRATCH/proj/t/03-bad.co" <<'EOF'
module bad3
import std

int main() {
    std.out.printf("failing on purpose\n")
    return 3
}
EOF
out=$($COME test "$SCRATCH/proj") && rc=0 || rc=$?
[ "$rc" = "1" ] || fail "test did not fail the run for a failing test"
echo "$out" | grep -q "FAIL t/03-bad.co (exit 3)" || fail "test failure line wrong: $out"
echo "$out" | grep -q "Results: 4 passed, 1 failed" || fail "test mixed summary wrong: $out"
rm "$SCRATCH/proj/t/03-bad.co"
cat > "$SCRATCH/proj/t/04-nobuild.co" <<'EOF'
module bad4
import std

int main() {
    return nosuchfn(1)
}
EOF
out=$($COME test "$SCRATCH/proj") && rc=0 || rc=$?
echo "$out" | grep -q "FAIL t/04-nobuild.co (build failed)" || fail "test build-failure line wrong: $out"
rm "$SCRATCH/proj/t/04-nobuild.co"
echo "CLI: test ok"

# ---------- come test --bench ----------
out=$($COME test "$SCRATCH/proj" --bench) || fail "bench run failed"
echo "$out" | grep -qE "avg [0-9]+(\.[0-9])? ms over 5 runs" || fail "bench output wrong: $out"
echo "CLI: bench ok"

# ---------- come test --cover ----------
# pick() never takes the v < 0 branch: line 9 of the
# fixture (its return) is the first uncovered line.
# (COIL-157: the branch body is the pinned shape, not a
# trailing return - a trailing return's line also
# collects the function's epilogue attribution under
# gcov, so it cannot pin an uncovered branch.)
out=$($COME test "$SCRATCH/cov" --cover) || fail "cover run failed"
echo "$out" | grep -q "coverage:" || fail "cover line missing: $out"
echo "$out" | grep -q "uncovered: 9" || fail "cover missed the uncovered line: $out"
echo "$out" | grep -q "Line coverage:" || fail "cover total missing: $out"
echo "CLI: cover ok"

# ---------- come test --race ----------
out=$($COME test "$SCRATCH/cov" --race) || fail "race run failed: $out"
echo "$out" | grep -q "Results: 1 passed, 0 failed" || fail "race summary wrong: $out"
echo "CLI: race ok"

# ---------- come fmt ----------
cat > "$SCRATCH/messy.co" <<'EOF'
module main
import std
int add(int a,int b){return a+b}
int main(){
int r=add(1,2)
if(r!=3){return 1}
string s="spaces  stay"
std.out.printf("%s %d\n",s,r)   // note
return 0
}
EOF
$COME fmt "$SCRATCH/messy.co" > /dev/null || fail "fmt failed"
cat > "$SCRATCH/expected.co" <<'EOF'
module main
import std
int add(int a, int b) { return a + b }
int main() {
    int r = add(1, 2)
    if (r != 3) { return 1 }
    string s = "spaces  stay"
    std.out.printf("%s %d\n", s, r)  // note
    return 0
}
EOF
diff "$SCRATCH/expected.co" "$SCRATCH/messy.co" > /dev/null || fail "fmt output differs from the canonical form:
$(diff "$SCRATCH/expected.co" "$SCRATCH/messy.co")"
out=$($COME fmt "$SCRATCH/messy.co") || fail "fmt second run failed"
echo "$out" | grep -q "already formatted" || fail "fmt is not idempotent"
$COME run "$SCRATCH/messy.co" > /dev/null || fail "formatted file does not build and run"
echo "CLI: fmt ok"

# ---------- genc with a bare -o name (dirname fix) ----------
mkdir -p "$SCRATCH/genc"
(cd "$SCRATCH/genc" && "$COME_ABS" genc "$SCRATCH/proj/t/01-ok.co" -o out.c > /dev/null) || fail "genc bare -o failed"
[ -f "$SCRATCH/genc/out.c" ] || fail "genc bare -o did not produce a file"
echo "CLI: genc ok"

echo "CLI suite: check, run, test, bench, cover, race, fmt all behave."
