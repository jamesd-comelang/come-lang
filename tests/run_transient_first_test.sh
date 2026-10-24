#!/bin/bash
# Parity step 4b remainder: first-assignment
# transient locals. A local whose declaration has
# NO concat init, but whose first assignment is a
# string concat (with all uses borrows and every
# assignment loop-safe), gets its concat assignments
# as direct arena assignments (bypassing adoption,
# like a declaration init). main.co runs every case
# for output and ASan; pos.co is positives only
# (direct `s = come_string_concat_arena` lines, NO
# talloc concat at all); ctl.co is controls only
# (a non-concat first assignment, or an outer local
# assigned in a loop, disqualifies -- talloc concat
# via the _come_v temp, NO direct arena assignment).
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
cp tests/transient_first_project/main.co "$WORK/main.co"
if ! "$COME" check "$WORK/main.co" > "$WORK/check.log" 2>&1; then
    echo "FAIL: transient-first fixture did not pass come check"
    cat "$WORK/check.log" | head -30
    exit 1
fi
if ! "$COME" build "$WORK/main.co" -o "$WORK/app" > "$WORK/build.log" 2>&1; then
    echo "FAIL: transient-first fixture did not build"
    cat "$WORK/build.log" | head -30
    exit 1
fi
if ! "$WORK/app" > "$WORK/out.txt" 2> "$WORK/err.txt"; then
    echo "FAIL: transient-first fixture failed to run"
    cat "$WORK/err.txt" | head -30
    exit 1
fi
if ! diff -u tests/transient_first_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: transient-first output mismatch (see diff above)"
    exit 1
fi
GENC="$REPO/.ccache/main.co.c"
if ! grep -F -q "s = come_string_concat_arena(" "$GENC"; then
    echo "FAIL: no direct arena assignment in combined generated C"
    exit 1
fi
if ! grep -q "come_string_concat(COME_CTX" "$GENC"; then
    echo "FAIL: no talloc concat in combined generated C (a control lost its talloc path)"
    exit 1
fi
if ! ( cd "$WORK" && COME_CFLAGS="-fsanitize=address -g" "$COME" build main.co -o app_asan > build_asan.log 2>&1 ); then
    echo "FAIL: transient-first project did not build under ASan"
    cat "$WORK/build_asan.log" | head -30
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/asan_err.txt"; then
    echo "FAIL: transient-first project failed under ASan"
    cat "$WORK/asan_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/transient_first_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: transient-first ASan output mismatch"
    exit 1
fi
cp tests/transient_first_project/pos.co "$WORK/pos.co"
if ! "$COME" build "$WORK/pos.co" -o "$WORK/pos_app" > "$WORK/pos_build.log" 2>&1; then
    echo "FAIL: transient-first positives fixture did not build"
    cat "$WORK/pos_build.log" | head -30
    exit 1
fi
if ! "$WORK/pos_app" > "$WORK/pos_out.txt" 2> "$WORK/pos_err.txt"; then
    echo "FAIL: transient-first positives fixture failed to run"
    cat "$WORK/pos_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/transient_first_project/expected_pos.txt "$WORK/pos_out.txt"; then
    echo "FAIL: transient-first positives output mismatch"
    exit 1
fi
GENPOS="$REPO/.ccache/pos.co.c"
if ! grep -F -q "s = come_string_concat_arena(" "$GENPOS"; then
    echo "FAIL: no direct arena assignment in positives generated C"
    grep -n "s =" "$GENPOS" | head -5
    exit 1
fi
if grep -q "come_string_concat(COME_CTX" "$GENPOS"; then
    echo "FAIL: talloc concat in positives generated C (a first assignment did not go arena)"
    grep -n "come_string_concat(COME_CTX" "$GENPOS" | head -5
    exit 1
fi
cp tests/transient_first_project/ctl.co "$WORK/ctl.co"
if ! "$COME" build "$WORK/ctl.co" -o "$WORK/ctl_app" > "$WORK/ctl_build.log" 2>&1; then
    echo "FAIL: transient-first controls fixture did not build"
    cat "$WORK/ctl_build.log" | head -30
    exit 1
fi
if ! "$WORK/ctl_app" > "$WORK/ctl_out.txt" 2> "$WORK/ctl_err.txt"; then
    echo "FAIL: transient-first controls fixture failed to run"
    cat "$WORK/ctl_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/transient_first_project/expected_ctl.txt "$WORK/ctl_out.txt"; then
    echo "FAIL: transient-first controls output mismatch"
    exit 1
fi
GENCTL="$REPO/.ccache/ctl.co.c"
if ! grep -q "come_string_concat(COME_CTX" "$GENCTL"; then
    echo "FAIL: no talloc concat in controls generated C"
    exit 1
fi
if grep -F -q "s = come_string_concat_arena(" "$GENCTL"; then
    echo "FAIL: direct arena assignment in controls generated C (a control wrongly went transient)"
    grep -n "s = come_string_concat_arena(" "$GENCTL" | head -5
    exit 1
fi
echo "transient-first: OK"
