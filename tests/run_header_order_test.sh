#!/bin/bash
# Step 7a header-order gate: the prologue must define
# COME_SCOPE_NO_PROTO and COME_ERR_NO_ARM_PROTO at the very top,
# before ANY #include. The original 7a bug defined them only before
# the direct talloc.h include, but come_coro.h (included earlier in
# the prologue) transitively includes talloc.h first, so talloc.h's
# own include guard made the second inclusion a no-op and the scope
# decls arrived from the first (unguarded) inclusion, causing
# "static declaration follows non-static declaration" at link time.
# This test pins the order in generated C, plus the static-inline
# definitions and the absence of conflicting extern decls.
# NOTE: 7a-specific. It passes only on compilers with the 7a prologue
# (step7a-inline and later); on perf2/pre-7a compilers the guards and
# inlines are absent and this test correctly fails.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp tests/header_order_project/main.co tests/header_order_project/expected.txt "$WORK/"
if ! ( cd "$WORK" && "$COME" build main.co -o app > build.log 2>&1 ); then
    echo "FAIL: header-order project did not build"
    cat "$WORK/build.log"
    exit 1
fi
"$WORK/app" > "$WORK/out.txt"
if ! diff -u tests/header_order_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: header-order output mismatch"
    exit 1
fi
GEN="$WORK/.ccache/main.co.c"
if [ ! -f "$GEN" ]; then
    echo "FAIL: generated C not found at $GEN"
    exit 1
fi
GSCOPE=$(grep -n "#define COME_SCOPE_NO_PROTO" "$GEN" | head -1 | cut -d: -f1)
GERR=$(grep -n "#define COME_ERR_NO_ARM_PROTO" "$GEN" | head -1 | cut -d: -f1)
FIRSTINC=$(grep -n "#include" "$GEN" | head -1 | cut -d: -f1)
TALLOC=$(grep -n '#include "mem/talloc.h"' "$GEN" | head -1 | cut -d: -f1)
CORO=$(grep -n '#include "come_coro.h"' "$GEN" | head -1 | cut -d: -f1)
if [ -z "$GSCOPE" ] || [ -z "$GERR" ] || [ -z "$FIRSTINC" ]; then
    echo "FAIL: guards or includes missing in generated C (scope=$GSCOPE err=$GERR first_include=$FIRSTINC)"
    exit 1
fi
if [ "$GSCOPE" -gt "$FIRSTINC" ] || [ "$GERR" -gt "$FIRSTINC" ]; then
    echo "FAIL: guards not before first include (scope=$GSCOPE err=$GERR first_include=$FIRSTINC)"
    exit 1
fi
if [ -n "$TALLOC" ] && [ "$TALLOC" -lt "$GSCOPE" ]; then
    echo "FAIL: talloc.h included before guards (talloc=$TALLOC scope=$GSCOPE)"
    exit 1
fi
if [ -n "$CORO" ] && [ "$CORO" -lt "$GSCOPE" ]; then
    echo "FAIL: come_coro.h included before guards (coro=$CORO scope=$GSCOPE)"
    exit 1
fi
for fn in "come_scope_push" "come_scope_pop" "come_err_arm"; do
    if ! grep -q "static inline void $fn" "$GEN"; then
        echo "FAIL: static inline $fn missing in generated C"
        exit 1
    fi
    if grep -q "^extern.*$fn" "$GEN"; then
        echo "FAIL: conflicting extern decl for $fn in generated C"
        grep -n "^extern.*$fn" "$GEN" | head -5
        exit 1
    fi
done
if ! ( cd "$WORK" && COME_CFLAGS="-fsanitize=address -g" "$COME" build main.co -o app_asan > build_asan.log 2>&1 ); then
    echo "FAIL: header-order project did not build under ASan"
    cat "$WORK/build_asan.log"
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/asan_err.txt"; then
    echo "FAIL: header-order project failed under ASan"
    cat "$WORK/asan_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/header_order_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: header-order ASan output mismatch"
    exit 1
fi
echo "header-order: OK"
