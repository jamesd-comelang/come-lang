#!/bin/bash
# Redesign (Design 5 no-frame tier): clean blocks skip
# their scope frame. Builds tests/cleanblock_project,
# checks its output, then counts plain come_scope_push
# calls in the generated C: exactly 1 (the dirty loop
# body only). Step 5: break/continue no longer defeat
# clean-block detection, so the break loop body and the
# break if block (scalars only) are now clean too; loop
# depths are runtime depths, so unwinding stays exact.
# The clean loop body, the clean if block, and the
# outer-buffer-assign if block must carry none. Runs
# under AddressSanitizer too: a wrongly skipped frame
# would free a live scope and show up as use-after-free.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp tests/cleanblock_project/main.co "$WORK/"
if ! ( cd "$WORK" && "$COME" build main.co -o app > build.log 2>&1 ); then
    echo "FAIL: cleanblock project did not build"
    cat "$WORK/build.log"
    exit 1
fi
"$WORK/app" > "$WORK/out.txt"
if ! diff -u tests/cleanblock_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: cleanblock project output mismatch (see diff above)"
    exit 1
fi
if ! ( cd "$WORK" && COME_CFLAGS="-fsanitize=address -g" "$COME" build main.co -o app_asan > build_asan.log 2>&1 ); then
    echo "FAIL: cleanblock project did not build under ASan"
    cat "$WORK/build_asan.log"
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/asan_err.txt"; then
    echo "FAIL: cleanblock project failed under ASan (use-after-free or similar)"
    cat "$WORK/asan_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/cleanblock_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: cleanblock project ASan output mismatch"
    exit 1
fi
PUSHES=$(grep -c "come_scope_push();" "$WORK/.ccache/main.co.c" || true)
if [ "$PUSHES" != "1" ]; then
    echo "FAIL: expected 1 plain scope push in generated C, found $PUSHES"
    grep -n "come_scope_push" "$WORK/.ccache/main.co.c" | head -20
    exit 1
fi
echo "cleanblock codegen checks passed"
echo "Cleanblock test passed!"
