#!/bin/bash
# Design 1+2 (redesign), straight-line slice: transient string
# locals whose concat initializer stays in the bump arena.
# main.co runs every case (positives + controls) for output
# and ASan; pos.co is positives only (generated C must have
# arena concats and NO talloc concat); ctl.co is controls
# only (generated C must have talloc concats and NO arena
# concat). A positive that fails to register, or a control
# that wrongly registers, breaks its pin.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
cp tests/transient_project/main.co "$WORK/main.co"
if ! "$COME" check "$WORK/main.co" > "$WORK/check.log" 2>&1; then
    echo "FAIL: transient fixture did not pass come check"
    cat "$WORK/check.log" | head -30
    exit 1
fi
if ! "$COME" build "$WORK/main.co" -o "$WORK/app" > "$WORK/build.log" 2>&1; then
    echo "FAIL: transient fixture did not build"
    cat "$WORK/build.log" | head -30
    exit 1
fi
if ! "$WORK/app" > "$WORK/out.txt" 2> "$WORK/err.txt"; then
    echo "FAIL: transient fixture failed to run"
    cat "$WORK/err.txt" | head -30
    exit 1
fi
if ! diff -u tests/transient_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: transient output mismatch (see diff above)"
    exit 1
fi
GENC="$REPO/.ccache/main.co.c"
if ! grep -q "come_string_concat_arena(" "$GENC"; then
    echo "FAIL: no arena concat in combined generated C"
    exit 1
fi
if ! grep -q "come_string_concat(COME_CTX" "$GENC"; then
    echo "FAIL: no talloc concat in combined generated C"
    exit 1
fi
if ! ( cd "$WORK" && COME_CFLAGS="-fsanitize=address -g" "$COME" build main.co -o app_asan > build_asan.log 2>&1 ); then
    echo "FAIL: transient project did not build under ASan"
    cat "$WORK/build_asan.log" | head -30
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/asan_err.txt"; then
    echo "FAIL: transient project failed under ASan"
    cat "$WORK/asan_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/transient_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: transient ASan output mismatch"
    exit 1
fi
# Positives only: every concat must be arena, none talloc.
cp tests/transient_project/pos.co "$WORK/pos.co"
if ! "$COME" build "$WORK/pos.co" -o "$WORK/pos_app" > "$WORK/pos_build.log" 2>&1; then
    echo "FAIL: transient positives fixture did not build"
    cat "$WORK/pos_build.log" | head -30
    exit 1
fi
if ! "$WORK/pos_app" > "$WORK/pos_out.txt" 2> "$WORK/pos_err.txt"; then
    echo "FAIL: transient positives fixture failed to run"
    cat "$WORK/pos_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/transient_project/expected_pos.txt "$WORK/pos_out.txt"; then
    echo "FAIL: transient positives output mismatch"
    exit 1
fi
GENPOS="$REPO/.ccache/pos.co.c"
if ! grep -q "come_string_concat_arena(" "$GENPOS"; then
    echo "FAIL: no arena concat in positives generated C"
    exit 1
fi
if grep -q "come_string_concat(COME_CTX" "$GENPOS"; then
    echo "FAIL: talloc concat in positives generated C (a positive failed to register)"
    grep -n "come_string_concat(COME_CTX" "$GENPOS" | head -5
    exit 1
fi
# Controls only: every concat must be talloc, none arena.
cp tests/transient_project/ctl.co "$WORK/ctl.co"
if ! "$COME" build "$WORK/ctl.co" -o "$WORK/ctl_app" > "$WORK/ctl_build.log" 2>&1; then
    echo "FAIL: transient controls fixture did not build"
    cat "$WORK/ctl_build.log" | head -30
    exit 1
fi
if ! "$WORK/ctl_app" > "$WORK/ctl_out.txt" 2> "$WORK/ctl_err.txt"; then
    echo "FAIL: transient controls fixture failed to run"
    cat "$WORK/ctl_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/transient_project/expected_ctl.txt "$WORK/ctl_out.txt"; then
    echo "FAIL: transient controls output mismatch"
    exit 1
fi
GENCTL="$REPO/.ccache/ctl.co.c"
if ! grep -q "come_string_concat(COME_CTX" "$GENCTL"; then
    echo "FAIL: no talloc concat in controls generated C"
    exit 1
fi
if grep -q "come_string_concat_arena(" "$GENCTL"; then
    echo "FAIL: arena concat in controls generated C (a control wrongly registered)"
    grep -n "come_string_concat_arena(" "$GENCTL" | head -5
    exit 1
fi
echo "transient: OK"
