#!/bin/bash
# Parity exhaust, safe slice: interval analysis
# for single-bound while loops. A while with
# `i < b.length()` and i provably nonneg at entry
# (constant, length, nonneg identifier, or i =
# i + <nonneg>) proves (b, i) at body top; reads
# before any modification to i are raw, reads
# after one are checked (i++/i-- now void proofs,
# a soundness fix for step 2's bodies too).
# Positives: decl, i++, and assignment nonneg.
# Controls: a parameter index (unknown sign) and
# a read after i-- (voided) stay checked.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
cp tests/interval_project/main.co "$WORK/main.co"
if ! "$COME" check "$WORK/main.co" > "$WORK/check.log" 2>&1; then
    echo "FAIL: interval fixture did not pass come check"
    cat "$WORK/check.log" | head -30
    exit 1
fi
if ! "$COME" build "$WORK/main.co" -o "$WORK/app" > "$WORK/build.log" 2>&1; then
    echo "FAIL: interval fixture did not build"
    cat "$WORK/build.log" | head -30
    exit 1
fi
if ! "$WORK/app" > "$WORK/out.txt" 2> "$WORK/err.txt"; then
    echo "FAIL: interval fixture failed to run"
    cat "$WORK/err.txt" | head -30
    exit 1
fi
if ! diff -u tests/interval_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: interval output mismatch (see diff above)"
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
for fn in pos_single pos_inc pos_assign; do
    if fn_body "$fn" | grep -q "COME_ARR_GET_T"; then
        echo "FAIL: $fn should be raw (no checked read)"
        exit 1
    fi
done
for fn in ctl_param ctl_dec; do
    if ! fn_body "$fn" | grep -q "COME_ARR_GET_T"; then
        echo "FAIL: $fn should stay checked"
        exit 1
    fi
done
if ! COME_CFLAGS="-fsanitize=address -g" \
    "$COME" build "$WORK/main.co" -o "$WORK/app_asan" > "$WORK/asan_build.log" 2>&1; then
    echo "FAIL: interval ASan build failed"
    cat "$WORK/asan_build.log" | head -30
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/err_asan.txt"; then
    echo "FAIL: interval ASan run failed"
    cat "$WORK/err_asan.txt" | head -30
    exit 1
fi
if ! diff -u tests/interval_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: interval ASan output mismatch (see diff above)"
    exit 1
fi
rm -rf "$WORK"
echo "interval: OK"
