#!/bin/bash
# Parity step 1: guard-dominated bounds-check elimination.
# main.co runs every case (guarded + positive + controls)
# for output and ASan; pos.co is guarded/positive only
# (generated C must have raw `.data))[` accesses and NO
# COME_ARR_GET_T); ctl.co is controls only (generated C
# must have COME_ARR_GET_T and NO raw access). A guarded
# access that keeps its check, or a control that loses
# its check, breaks its pin.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
cp tests/bounds_project/main.co "$WORK/main.co"
if ! "$COME" check "$WORK/main.co" > "$WORK/check.log" 2>&1; then
    echo "FAIL: bounds fixture did not pass come check"
    cat "$WORK/check.log" | head -30
    exit 1
fi
if ! "$COME" build "$WORK/main.co" -o "$WORK/app" > "$WORK/build.log" 2>&1; then
    echo "FAIL: bounds fixture did not build"
    cat "$WORK/build.log" | head -30
    exit 1
fi
if ! "$WORK/app" > "$WORK/out.txt" 2> "$WORK/err.txt"; then
    echo "FAIL: bounds fixture failed to run"
    cat "$WORK/err.txt" | head -30
    exit 1
fi
if ! diff -u tests/bounds_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: bounds output mismatch (see diff above)"
    exit 1
fi
GENC="$REPO/.ccache/main.co.c"
if ! grep -F -q ".data))[" "$GENC"; then
    echo "FAIL: no raw access in combined generated C"
    exit 1
fi
if ! grep -q "COME_ARR_GET_T" "$GENC"; then
    echo "FAIL: no checked access in combined generated C"
    exit 1
fi
if ! ( cd "$WORK" && COME_CFLAGS="-fsanitize=address -g" "$COME" build main.co -o app_asan > build_asan.log 2>&1 ); then
    echo "FAIL: bounds project did not build under ASan"
    cat "$WORK/build_asan.log" | head -30
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/asan_err.txt"; then
    echo "FAIL: bounds project failed under ASan"
    cat "$WORK/asan_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/bounds_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: bounds ASan output mismatch"
    exit 1
fi
cp tests/bounds_project/pos.co "$WORK/pos.co"
if ! "$COME" build "$WORK/pos.co" -o "$WORK/pos_app" > "$WORK/pos_build.log" 2>&1; then
    echo "FAIL: bounds positives fixture did not build"
    cat "$WORK/pos_build.log" | head -30
    exit 1
fi
if ! "$WORK/pos_app" > "$WORK/pos_out.txt" 2> "$WORK/pos_err.txt"; then
    echo "FAIL: bounds positives fixture failed to run"
    cat "$WORK/pos_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/bounds_project/expected_pos.txt "$WORK/pos_out.txt"; then
    echo "FAIL: bounds positives output mismatch"
    exit 1
fi
GENPOS="$REPO/.ccache/pos.co.c"
if ! grep -F -q ".data))[" "$GENPOS"; then
    echo "FAIL: no raw access in positives generated C"
    exit 1
fi
if grep -q "COME_ARR_GET_T" "$GENPOS"; then
    echo "FAIL: checked access in positives generated C (a guarded access kept its check)"
    grep -n "COME_ARR_GET_T" "$GENPOS" | head -5
    exit 1
fi
cp tests/bounds_project/ctl.co "$WORK/ctl.co"
if ! "$COME" build "$WORK/ctl.co" -o "$WORK/ctl_app" > "$WORK/ctl_build.log" 2>&1; then
    echo "FAIL: bounds controls fixture did not build"
    cat "$WORK/ctl_build.log" | head -30
    exit 1
fi
if ! "$WORK/ctl_app" > "$WORK/ctl_out.txt" 2> "$WORK/ctl_err.txt"; then
    echo "FAIL: bounds controls fixture failed to run"
    cat "$WORK/ctl_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/bounds_project/expected_ctl.txt "$WORK/ctl_out.txt"; then
    echo "FAIL: bounds controls output mismatch"
    exit 1
fi
GENCTL="$REPO/.ccache/ctl.co.c"
if ! grep -q "COME_ARR_GET_T" "$GENCTL"; then
    echo "FAIL: no checked access in controls generated C"
    exit 1
fi
if grep -F -q ".data))[" "$GENCTL"; then
    echo "FAIL: raw access in controls generated C (a control wrongly lost its check)"
    grep -F -n ".data))[" "$GENCTL" | head -5
    exit 1
fi
echo "bounds: OK"
