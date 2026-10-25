#!/bin/bash
# Step 6 proof gate: mask/modulo range proofs, guard/fixup
# nonneg, length facts, and --debug proof audit. Builds
# tests/proof_project three ways and checks output each
# time, then counts COME_ARR_GET_T (checked reads) in the
# generated C immediately after each build (the next build
# overwrites .ccache): normal 6 (controls only: mask-short,
# mask-variable, modulo-sub, modulo-short, fixup-byte, fixup-int),
# --debug 9 (audit re-inserts checks for all 3 proven reads),
# ASan 6 (audit off: COME_CFLAGS set, so like normal). Values prove
# the reads are correct; macro counts prove proofs apply
# exactly where sound and audit re-inserts them.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp tests/proof_project/main.co "$WORK/"
check_macros() {
    local want="$1" label="$2"
    local got
    got=$(grep -c "COME_ARR_GET_T" "$WORK/.ccache/main.co.c" || true)
    if [ "$got" != "$want" ]; then
        echo "FAIL: $label: expected $want checked reads, found $got"
        grep -n "COME_ARR_GET_T\|\\.data))\\[" "$WORK/.ccache/main.co.c" | head -20
        exit 1
    fi
}
if ! ( cd "$WORK" && "$COME" build main.co -o app > build.log 2>&1 ); then
    echo "FAIL: proof project did not build"
    cat "$WORK/build.log"
    exit 1
fi
"$WORK/app" > "$WORK/out.txt"
if ! diff -u tests/proof_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: proof project output mismatch (see diff above)"
    exit 1
fi
check_macros 6 "normal"
if ! ( cd "$WORK" && "$COME" build --debug main.co -o app_debug > build_debug.log 2>&1 ); then
    echo "FAIL: proof project did not build under --debug"
    cat "$WORK/build_debug.log"
    exit 1
fi
"$WORK/app_debug" > "$WORK/out_debug.txt"
if ! diff -u tests/proof_project/expected.txt "$WORK/out_debug.txt"; then
    echo "FAIL: proof project --debug output mismatch"
    exit 1
fi
check_macros 9 "debug-audit"
if ! ( cd "$WORK" && COME_CFLAGS="-fsanitize=address -g" "$COME" build main.co -o app_asan > build_asan.log 2>&1 ); then
    echo "FAIL: proof project did not build under ASan"
    cat "$WORK/build_asan.log"
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/asan_err.txt"; then
    echo "FAIL: proof project failed under ASan"
    cat "$WORK/asan_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/proof_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: proof project ASan output mismatch"
    exit 1
fi
check_macros 6 "asan"
echo "proof codegen checks passed"
echo "Proof test passed!"
