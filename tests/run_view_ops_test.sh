#!/bin/bash
# Design 3 follow-up (redesign): string operations on
# non-suffix views must stay within the view, not read
# into the parent. These are runtime fixes (no codegen
# forms to pin), so this test checks output plus an
# ASan run only.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
cp tests/view_ops_project/main.co "$WORK/main.co"
if ! "$COME" check "$WORK/main.co" > "$WORK/check.log" 2>&1; then
    echo "FAIL: view-ops fixture did not pass come check"
    cat "$WORK/check.log" | head -30
    exit 1
fi
if ! "$COME" build "$WORK/main.co" -o "$WORK/app" > "$WORK/build.log" 2>&1; then
    echo "FAIL: view-ops fixture did not build"
    cat "$WORK/build.log" | head -30
    exit 1
fi
if ! "$WORK/app" > "$WORK/out.txt" 2> "$WORK/err.txt"; then
    echo "FAIL: view-ops fixture failed to run"
    cat "$WORK/err.txt" | head -30
    exit 1
fi
if ! diff -u tests/view_ops_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: view-ops output mismatch (see diff above)"
    exit 1
fi
if ! ( cd "$WORK" && COME_CFLAGS="-fsanitize=address -g" "$COME" build main.co -o app_asan > build_asan.log 2>&1 ); then
    echo "FAIL: view-ops project did not build under ASan"
    cat "$WORK/build_asan.log" | head -30
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/asan_err.txt"; then
    echo "FAIL: view-ops project failed under ASan (use-after-free or similar)"
    cat "$WORK/asan_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/view_ops_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: view-ops project ASan output mismatch"
    exit 1
fi
rm -rf "$WORK"
echo "View-ops test passed!"
