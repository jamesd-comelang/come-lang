#!/bin/bash
# Parity step 2: loop guards (while positive-form, counted
# for) plus the step-1 leak fix (loop bodies start with
# a cleared proof set). main.co runs every case for
# output and ASan; pos.co is loop-guard positives only
# (generated C must have raw `.data))[` and NO
# COME_ARR_GET_T); ctl.co is controls only (generated C
# must have COME_ARR_GET_T and NO raw access). The
# for_neg control reads b[-1]: under ASan, a wrongly
# raw access there fails the run (heap-buffer-overflow),
# so ASan pins the soundness, not just the output.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
cp tests/bounds_loop_project/main.co "$WORK/main.co"
if ! "$COME" check "$WORK/main.co" > "$WORK/check.log" 2>&1; then
    echo "FAIL: bounds-loop fixture did not pass come check"
    cat "$WORK/check.log" | head -30
    exit 1
fi
if ! "$COME" build "$WORK/main.co" -o "$WORK/app" > "$WORK/build.log" 2>&1; then
    echo "FAIL: bounds-loop fixture did not build"
    cat "$WORK/build.log" | head -30
    exit 1
fi
if ! "$WORK/app" > "$WORK/out.txt" 2> "$WORK/err.txt"; then
    echo "FAIL: bounds-loop fixture failed to run"
    cat "$WORK/err.txt" | head -30
    exit 1
fi
if ! diff -u tests/bounds_loop_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: bounds-loop output mismatch (see diff above)"
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
    echo "FAIL: bounds-loop project did not build under ASan"
    cat "$WORK/build_asan.log" | head -30
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/asan_err.txt"; then
    echo "FAIL: bounds-loop project failed under ASan"
    cat "$WORK/asan_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/bounds_loop_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: bounds-loop ASan output mismatch"
    exit 1
fi
cp tests/bounds_loop_project/pos.co "$WORK/pos.co"
if ! "$COME" build "$WORK/pos.co" -o "$WORK/pos_app" > "$WORK/pos_build.log" 2>&1; then
    echo "FAIL: bounds-loop positives fixture did not build"
    cat "$WORK/pos_build.log" | head -30
    exit 1
fi
if ! "$WORK/pos_app" > "$WORK/pos_out.txt" 2> "$WORK/pos_err.txt"; then
    echo "FAIL: bounds-loop positives fixture failed to run"
    cat "$WORK/pos_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/bounds_loop_project/expected_pos.txt "$WORK/pos_out.txt"; then
    echo "FAIL: bounds-loop positives output mismatch"
    exit 1
fi
GENPOS="$REPO/.ccache/pos.co.c"
if ! grep -F -q ".data))[" "$GENPOS"; then
    echo "FAIL: no raw access in positives generated C"
    exit 1
fi
if grep -q "COME_ARR_GET_T" "$GENPOS"; then
    echo "FAIL: checked access in positives generated C (a loop guard did not fire)"
    grep -n "COME_ARR_GET_T" "$GENPOS" | head -5
    exit 1
fi
cp tests/bounds_loop_project/ctl.co "$WORK/ctl.co"
if ! "$COME" build "$WORK/ctl.co" -o "$WORK/ctl_app" > "$WORK/ctl_build.log" 2>&1; then
    echo "FAIL: bounds-loop controls fixture did not build"
    cat "$WORK/ctl_build.log" | head -30
    exit 1
fi
if ! "$WORK/ctl_app" > "$WORK/ctl_out.txt" 2> "$WORK/ctl_err.txt"; then
    echo "FAIL: bounds-loop controls fixture failed to run"
    cat "$WORK/ctl_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/bounds_loop_project/expected_ctl.txt "$WORK/ctl_out.txt"; then
    echo "FAIL: bounds-loop controls output mismatch"
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
# for_neg specifically: its function is uncalled (its
# runtime is not asserted -- finding 38), but its
# access must still be checked, not raw.
FORNEG_LINE=$(grep -n -F "come_main__sum_for_neg" "$GENCTL" | grep -F "b) {" | head -1 | cut -d: -f1)
FORNEG_BODY=$(sed -n "${FORNEG_LINE},/^}/p" "$GENCTL")
if ! echo "$FORNEG_BODY" | grep -q "COME_ARR_GET_T"; then
    echo "FAIL: for_neg access is not checked in controls generated C"
    exit 1
fi
if echo "$FORNEG_BODY" | grep -F -q ".data))["; then
    echo "FAIL: for_neg access is raw in controls generated C"
    exit 1
fi
echo "bounds-loop: OK"
