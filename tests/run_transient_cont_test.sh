#!/bin/bash
# Parity step 4c-arrays: transient containers. An
# array local that never escapes (only built-in
# method use and index reads/stores, no return,
# call arg, store, alias, or outer-loop store)
# holds arena elements without eager promotion on
# append/insert/set/index-store. main.co runs every
# case for output and ASan; pos.co is positives
# only (NO promote and NO scope-adopt anywhere --
# every store suppressed); ctl.co is controls only
# (a return, a call arg, and an outer-loop store
# each disqualify -- eager promote present).
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
cp tests/transient_cont_project/main.co "$WORK/main.co"
if ! "$COME" check "$WORK/main.co" > "$WORK/check.log" 2>&1; then
    echo "FAIL: transient-cont fixture did not pass come check"
    cat "$WORK/check.log" | head -30
    exit 1
fi
if ! "$COME" build "$WORK/main.co" -o "$WORK/app" > "$WORK/build.log" 2>&1; then
    echo "FAIL: transient-cont fixture did not build"
    cat "$WORK/build.log" | head -30
    exit 1
fi
if ! "$WORK/app" > "$WORK/out.txt" 2> "$WORK/err.txt"; then
    echo "FAIL: transient-cont fixture failed to run"
    cat "$WORK/err.txt" | head -30
    exit 1
fi
if ! diff -u tests/transient_cont_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: transient-cont output mismatch (see diff above)"
    exit 1
fi
GENC="$REPO/.ccache/main.co.c"
if ! grep -q "come_string_promote" "$GENC"; then
    echo "FAIL: no promote in combined generated C (a control lost its eager adopt)"
    exit 1
fi
if ! ( cd "$WORK" && COME_CFLAGS="-fsanitize=address -g" "$COME" build main.co -o app_asan > build_asan.log 2>&1 ); then
    echo "FAIL: transient-cont project did not build under ASan"
    cat "$WORK/build_asan.log" | head -30
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/asan_err.txt"; then
    echo "FAIL: transient-cont project failed under ASan"
    cat "$WORK/asan_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/transient_cont_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: transient-cont ASan output mismatch"
    exit 1
fi
cp tests/transient_cont_project/pos.co "$WORK/pos.co"
if ! "$COME" build "$WORK/pos.co" -o "$WORK/pos_app" > "$WORK/pos_build.log" 2>&1; then
    echo "FAIL: transient-cont positives fixture did not build"
    cat "$WORK/pos_build.log" | head -30
    exit 1
fi
if ! "$WORK/pos_app" > "$WORK/pos_out.txt" 2> "$WORK/pos_err.txt"; then
    echo "FAIL: transient-cont positives fixture failed to run"
    cat "$WORK/pos_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/transient_cont_project/expected_pos.txt "$WORK/pos_out.txt"; then
    echo "FAIL: transient-cont positives output mismatch"
    exit 1
fi
GENPOS="$REPO/.ccache/pos.co.c"
if grep -q "come_string_promote" "$GENPOS"; then
    echo "FAIL: promote in positives generated C (a transient store did not suppress adopt)"
    grep -n "come_string_promote" "$GENPOS" | head -5
    exit 1
fi
if grep -q "come_scope_adopt" "$GENPOS"; then
    echo "FAIL: scope-adopt in positives generated C (a transient store did not suppress adopt)"
    grep -n "come_scope_adopt" "$GENPOS" | head -5
    exit 1
fi
cp tests/transient_cont_project/ctl.co "$WORK/ctl.co"
if ! "$COME" build "$WORK/ctl.co" -o "$WORK/ctl_app" > "$WORK/ctl_build.log" 2>&1; then
    echo "FAIL: transient-cont controls fixture did not build"
    cat "$WORK/ctl_build.log" | head -30
    exit 1
fi
if ! "$WORK/ctl_app" > "$WORK/ctl_out.txt" 2> "$WORK/ctl_err.txt"; then
    echo "FAIL: transient-cont controls fixture failed to run"
    cat "$WORK/ctl_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/transient_cont_project/expected_ctl.txt "$WORK/ctl_out.txt"; then
    echo "FAIL: transient-cont controls output mismatch"
    exit 1
fi
GENCTL="$REPO/.ccache/ctl.co.c"
if ! grep -q "come_string_promote" "$GENCTL"; then
    echo "FAIL: no promote in controls generated C (a control wrongly went transient)"
    exit 1
fi
echo "transient-cont: OK"
