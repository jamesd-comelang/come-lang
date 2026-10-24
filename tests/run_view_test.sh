#!/bin/bash
# Design 3 (redesign): string views and byte views.
# Temporary views compile to come_string_view /
# come_string_byte_view (no allocation); views in
# storing positions (declaration, assignment, return,
# container store, call argument, printf) promote to
# copies (come_string_substr / come_string_byte_range),
# so no view variable can dangle. Checks the output,
# the generated C for both forms, and an ASan run
# (a missed promotion would read freed parent data).
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
cp tests/view_project/main.co "$WORK/main.co"
if ! "$COME" check "$WORK/main.co" > "$WORK/check.log" 2>&1; then
    echo "FAIL: view fixture did not pass come check"
    cat "$WORK/check.log" | head -30
    exit 1
fi
if ! "$COME" build "$WORK/main.co" -o "$WORK/app" > "$WORK/build.log" 2>&1; then
    echo "FAIL: view fixture did not build"
    cat "$WORK/build.log" | head -30
    exit 1
fi
if ! "$WORK/app" > "$WORK/out.txt" 2> "$WORK/err.txt"; then
    echo "FAIL: view fixture failed to run"
    cat "$WORK/err.txt" | head -30
    exit 1
fi
if ! diff -u tests/view_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: view output mismatch (see diff above)"
    exit 1
fi
# Generated C must contain BOTH forms: true views for
# temporaries, copies for storing positions.
GENC="$REPO/.ccache/main.co.c"
if ! grep -q "come_string_view(" "$GENC"; then
    echo "FAIL: no true string view in generated C"
    exit 1
fi
if ! grep -q "come_string_byte_view(" "$GENC"; then
    echo "FAIL: no true byte view in generated C"
    exit 1
fi
if ! grep -q "come_string_substr(" "$GENC"; then
    echo "FAIL: no promoted string copy (substr) in generated C"
    exit 1
fi
if ! grep -q "come_string_byte_range(" "$GENC"; then
    echo "FAIL: no promoted byte copy (byte_range) in generated C"
    exit 1
fi
if ! ( cd "$WORK" && COME_CFLAGS="-fsanitize=address -g" "$COME" build main.co -o app_asan > build_asan.log 2>&1 ); then
    echo "FAIL: view project did not build under ASan"
    cat "$WORK/build_asan.log" | head -30
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/asan_err.txt"; then
    echo "FAIL: view project failed under ASan (use-after-free or similar)"
    cat "$WORK/asan_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/view_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: view project ASan output mismatch"
    exit 1
fi
rm -rf "$WORK"
echo "View test passed!"
