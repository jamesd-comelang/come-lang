#!/bin/bash
# Lazy scope markers (perf2 A) gate: nested-callee first allocation,
# .chown() across frames, escaping returns, unwinding through
# unlinked (never-materialized) frames. Lazy markers are runtime-only
# (no codegen change), so the gate is output + ASan, not generated-C
# counts: a broken materialize/adopt path shows up as wrong output
# or use-after-free, not as a different push count.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp tests/lazy_marker_project/main.co tests/lazy_marker_project/expected.txt "$WORK/"
if ! ( cd "$WORK" && "$COME" build main.co -o app > build.log 2>&1 ); then
    echo "FAIL: lazy-marker project did not build"
    cat "$WORK/build.log"
    exit 1
fi
"$WORK/app" > "$WORK/out.txt"
if ! diff -u tests/lazy_marker_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: lazy-marker output mismatch (see diff above)"
    exit 1
fi
if ! ( cd "$WORK" && COME_CFLAGS="-fsanitize=address -g" "$COME" build main.co -o app_asan > build_asan.log 2>&1 ); then
    echo "FAIL: lazy-marker project did not build under ASan"
    cat "$WORK/build_asan.log"
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/asan_err.txt"; then
    echo "FAIL: lazy-marker project failed under ASan (use-after-free or similar)"
    cat "$WORK/asan_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/lazy_marker_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: lazy-marker ASan output mismatch"
    exit 1
fi
echo "lazy-marker: OK"
