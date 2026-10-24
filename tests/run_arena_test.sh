#!/bin/bash
# Design 2 (redesign), phase 1: bump arenas for temporary
# string concats. Temporary concats compile to
# come_string_concat_arena (bump allocation, bulk-free
# at frame pop); storing positions keep the talloc
# concat. Checks the output, the generated C for both
# forms plus the adoption safety net, and an ASan run
# (a dangling arena temporary or a stale pointer across
# a block boundary would show up as use-after-free).
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
cp tests/arena_project/main.co "$WORK/main.co"
if ! "$COME" check "$WORK/main.co" > "$WORK/check.log" 2>&1; then
    echo "FAIL: arena fixture did not pass come check"
    cat "$WORK/check.log" | head -30
    exit 1
fi
if ! "$COME" build "$WORK/main.co" -o "$WORK/app" > "$WORK/build.log" 2>&1; then
    echo "FAIL: arena fixture did not build"
    cat "$WORK/build.log" | head -30
    exit 1
fi
if ! "$WORK/app" > "$WORK/out.txt" 2> "$WORK/err.txt"; then
    echo "FAIL: arena fixture failed to run"
    cat "$WORK/err.txt" | head -30
    exit 1
fi
if ! diff -u tests/arena_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: arena output mismatch (see diff above)"
    exit 1
fi
# Generated C must contain BOTH concat forms, plus the
# adoption safety net (emitted at adopt sites even
# though it never fires for correctly routed values).
GENC="$REPO/.ccache/main.co.c"
if ! grep -q "come_string_concat_arena(" "$GENC"; then
    echo "FAIL: no arena concat in generated C"
    exit 1
fi
if ! grep -q "come_string_concat(COME_CTX" "$GENC"; then
    echo "FAIL: no talloc concat in generated C"
    exit 1
fi
if ! grep -q "come_arena_contains(" "$GENC"; then
    echo "FAIL: no arena adoption net in generated C"
    exit 1
fi
if ! grep -q "come_string_promote(" "$GENC"; then
    echo "FAIL: no arena promotion in generated C"
    exit 1
fi
if ! ( cd "$WORK" && COME_CFLAGS="-fsanitize=address -g" "$COME" build main.co -o app_asan > build_asan.log 2>&1 ); then
    echo "FAIL: arena project did not build under ASan"
    cat "$WORK/build_asan.log" | head -30
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/asan_err.txt"; then
    echo "FAIL: arena project failed under ASan (use-after-free or similar)"
    cat "$WORK/asan_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/arena_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: arena project ASan output mismatch"
    exit 1
fi
rm -rf "$WORK"
echo "Arena test passed!"
