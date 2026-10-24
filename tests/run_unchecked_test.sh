#!/bin/bash
# Parity exhaust: opt-in unchecked region.
# A computed-index read (i*2 % len, not an
# identifier pair) is checked by default and raw
# inside `unchecked { }`; a variable named
# unchecked still parses and runs. The
# --unchecked build flag makes every user read
# raw. Both binaries must print the same output
# (all reads here are in bounds, so raw == checked).
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
cp tests/unchecked_project/main.co "$WORK/main.co"
if ! "$COME" check "$WORK/main.co" > "$WORK/check.log" 2>&1; then
    echo "FAIL: unchecked fixture did not pass come check"
    cat "$WORK/check.log" | head -30
    exit 1
fi
if ! "$COME" build "$WORK/main.co" -o "$WORK/app" > "$WORK/build.log" 2>&1; then
    echo "FAIL: unchecked fixture did not build"
    cat "$WORK/build.log" | head -30
    exit 1
fi
if ! "$WORK/app" > "$WORK/out.txt" 2> "$WORK/err.txt"; then
    echo "FAIL: unchecked fixture failed to run"
    cat "$WORK/err.txt" | head -30
    exit 1
fi
if ! diff -u tests/unchecked_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: unchecked output mismatch (see diff above)"
    exit 1
fi
GENC="$REPO/.ccache/main.co.c"
fn_body() {
    local start end
    start=$(grep -n "come_main__$1(" "$GENC" | grep "{" | head -1 | cut -d: -f1)
    if [ -z "$start" ]; then echo "FAIL: function $1 not found"; exit 1; fi
    end=$(awk -v s="$start" 'NR > s && /^}/ { print NR; exit }' "$GENC")
    sed -n "${start},${end}p" "$GENC"
}
if ! fn_body checked_sum | grep -q "COME_ARR_GET_T"; then
    echo "FAIL: checked_sum should stay checked by default"
    exit 1
fi
if fn_body unchecked_sum | grep -q "COME_ARR_GET_T"; then
    echo "FAIL: unchecked_sum should be raw inside unchecked { }"
    exit 1
fi
if ! fn_body unchecked_sum | grep -F -q ".data))["; then
    echo "FAIL: unchecked_sum has no raw read"
    exit 1
fi
if ! "$COME" build --unchecked "$WORK/main.co" -o "$WORK/app_unchecked" > "$WORK/build_unchecked.log" 2>&1; then
    echo "FAIL: --unchecked build failed"
    cat "$WORK/build_unchecked.log" | head -30
    exit 1
fi
if ! "$WORK/app_unchecked" > "$WORK/out_unchecked.txt" 2> "$WORK/err_unchecked.txt"; then
    echo "FAIL: --unchecked binary failed to run"
    cat "$WORK/err_unchecked.txt" | head -30
    exit 1
fi
if ! diff -u tests/unchecked_project/expected.txt "$WORK/out_unchecked.txt"; then
    echo "FAIL: --unchecked output mismatch (see diff above)"
    exit 1
fi
GENC_UNCHECKED="$REPO/.ccache/main.co.unchecked.c"
if [ ! -f "$GENC_UNCHECKED" ]; then
    echo "FAIL: --unchecked generated C not found (cache separation)"
    exit 1
fi
if grep -q "COME_ARR_GET_T" "$GENC_UNCHECKED"; then
    echo "FAIL: --unchecked build should have no checked reads"
    exit 1
fi
if ! COME_CFLAGS="-fsanitize=address -g" \
    "$COME" build "$WORK/main.co" -o "$WORK/app_asan" > "$WORK/asan_build.log" 2>&1; then
    echo "FAIL: unchecked ASan build failed"
    cat "$WORK/asan_build.log" | head -30
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/err_asan.txt"; then
    echo "FAIL: unchecked ASan run failed"
    cat "$WORK/err_asan.txt" | head -30
    exit 1
fi
if ! diff -u tests/unchecked_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: unchecked ASan output mismatch (see diff above)"
    exit 1
fi
rm -rf "$WORK"
echo "unchecked: OK"
