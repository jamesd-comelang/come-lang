#!/bin/bash
# Parity step 4d: checkpoint frames (Design 5
# checkpoint tier). A block whose allocations are
# all arena (transient string concats of variables,
# scalar work) gets a bump rewind only, no talloc
# frame. Positives: a bare block and a loop body
# with variable-only transient concats (2
# checkpoint pushes). Controls: a literal-operand
# transient concat and a scope-unsafe outer store
# (talloc) each get a full push (2 full pushes).
# main.co runs every case for output and ASan.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
cp tests/checkpoint_project/main.co "$WORK/main.co"
if ! "$COME" check "$WORK/main.co" > "$WORK/check.log" 2>&1; then
    echo "FAIL: checkpoint fixture did not pass come check"
    cat "$WORK/check.log" | head -30
    exit 1
fi
if ! "$COME" build "$WORK/main.co" -o "$WORK/app" > "$WORK/build.log" 2>&1; then
    echo "FAIL: checkpoint fixture did not build"
    cat "$WORK/build.log" | head -30
    exit 1
fi
if ! "$WORK/app" > "$WORK/out.txt" 2> "$WORK/err.txt"; then
    echo "FAIL: checkpoint fixture failed to run"
    cat "$WORK/err.txt" | head -30
    exit 1
fi
if ! diff -u tests/checkpoint_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: checkpoint output mismatch (see diff above)"
    exit 1
fi
GENC="$REPO/.ccache/main.co.c"
CKPT=$(grep -c "come_scope_push_checkpoint" "$GENC")
if [ "$CKPT" -ne 2 ]; then
    echo "FAIL: expected 2 checkpoint pushes, got $CKPT"
    exit 1
fi
FULL=$(grep -c "come_scope_push();" "$GENC")
if [ "$FULL" -ne 2 ]; then
    echo "FAIL: expected 2 full pushes, got $FULL"
    exit 1
fi
if ! COME_CFLAGS="-fsanitize=address -g" \
    "$COME" build "$WORK/main.co" -o "$WORK/app_asan" > "$WORK/asan_build.log" 2>&1; then
    echo "FAIL: checkpoint ASan build failed"
    cat "$WORK/asan_build.log" | head -30
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/err_asan.txt"; then
    echo "FAIL: checkpoint ASan run failed"
    cat "$WORK/err_asan.txt" | head -30
    exit 1
fi
if ! diff -u tests/checkpoint_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: checkpoint ASan output mismatch (see diff above)"
    exit 1
fi
rm -rf "$WORK"
echo "checkpoint: OK"
