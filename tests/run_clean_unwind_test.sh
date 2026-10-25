#!/bin/bash
# Step 5 unwinding gate: break/continue no longer defeat
# clean-block detection, and loop depths are runtime depths
# (lexical minus clean ancestors). Builds
# tests/clean_unwind_project, checks its output, runs it
# under ASan (a wrongly skipped frame would free a live
# scope and show up as use-after-free), then checks the
# generated C: no plain pushes (all blocks clean or
# checkpoint), exactly 1 checkpoint push (case C body),
# and every pop_to_depth is (0) -- cases A and E enclose
# their loops in 1 and 2 clean blocks, so lexical depths
# would be (1) and (2); runtime depths are (0).
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp tests/clean_unwind_project/main.co "$WORK/"
if ! ( cd "$WORK" && "$COME" build main.co -o app > build.log 2>&1 ); then
    echo "FAIL: clean_unwind project did not build"
    cat "$WORK/build.log"
    exit 1
fi
"$WORK/app" > "$WORK/out.txt"
if ! diff -u tests/clean_unwind_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: clean_unwind project output mismatch (see diff above)"
    exit 1
fi
if ! ( cd "$WORK" && COME_CFLAGS="-fsanitize=address -g" "$COME" build main.co -o app_asan > build_asan.log 2>&1 ); then
    echo "FAIL: clean_unwind project did not build under ASan"
    cat "$WORK/build_asan.log"
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/asan_err.txt"; then
    echo "FAIL: clean_unwind project failed under ASan (use-after-free or similar)"
    cat "$WORK/asan_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/clean_unwind_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: clean_unwind project ASan output mismatch"
    exit 1
fi
PUSHES=$(grep -c "come_scope_push();" "$WORK/.ccache/main.co.c" || true)
if [ "$PUSHES" != "0" ]; then
    echo "FAIL: expected 0 plain scope pushes in generated C, found $PUSHES"
    grep -n "come_scope_push" "$WORK/.ccache/main.co.c" | head -20
    exit 1
fi
CKPT=$(grep -c "come_scope_push_checkpoint();" "$WORK/.ccache/main.co.c" || true)
if [ "$CKPT" != "1" ]; then
    echo "FAIL: expected 1 checkpoint push in generated C, found $CKPT"
    grep -n "come_scope_push" "$WORK/.ccache/main.co.c" | head -20
    exit 1
fi
POPS=$(grep -o "come_scope_pop_to_depth([0-9]*)" "$WORK/.ccache/main.co.c" || true)
POPN=$(echo "$POPS" | grep -c . || true)
if [ "$POPN" != "4" ]; then
    echo "FAIL: expected 4 pop_to_depth lines in generated C, found $POPN"
    echo "$POPS"
    exit 1
fi
BAD=$(echo "$POPS" | grep -v "come_scope_pop_to_depth(0)" || true)
if [ -n "$BAD" ]; then
    echo "FAIL: expected all pop_to_depth to be (0) (runtime), found:"
    echo "$BAD"
    exit 1
fi
echo "clean_unwind codegen checks passed"
echo "Clean unwind test passed!"
