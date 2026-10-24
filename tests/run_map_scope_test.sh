#!/bin/bash
# COIL-157: scope contexts (spec 7). Builds
# tests/map_scope_project in a scratch dir, diffs the escape
# lines exactly, and bounds peak RSS: 20,000 builds of a
# 147-entry map must peak under 50 MB (the unfixed compiler
# leaks ~30 KB per build, about 600 MB here). The project
# also runs under AddressSanitizer: a whole map assigned to
# an instance field in create() used to free its keys with
# create()'s scope, and plain runs read the freed keys
# intact by luck - only ASan catches that use-after-free
# deterministically.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp tests/map_scope_project/main.co "$WORK/"
cp tests/map_scope_project/kwtab.co "$WORK/"
if ! ( cd "$WORK" && "$COME" build main.co -o app > build.log 2>&1 ); then
    echo "FAIL: map scope project did not build"
    cat "$WORK/build.log"
    exit 1
fi
"$WORK/app" > "$WORK/out.txt"
grep -v "VmHWM:" "$WORK/out.txt" > "$WORK/escapes.txt"
if ! diff -u tests/map_scope_project/expected.txt "$WORK/escapes.txt"; then
    echo "FAIL: map scope project output mismatch (see diff above)"
    exit 1
fi
PEAK=$(grep "VmHWM:" "$WORK/out.txt" | awk '{print $2}')
if [ -z "$PEAK" ]; then
    echo "FAIL: no VmHWM line in map scope output"
    cat "$WORK/out.txt"
    exit 1
fi
if [ "$PEAK" -ge 50000 ]; then
    echo "FAIL: map scope peak RSS ${PEAK} kB exceeds 50000 kB - maps built in a loop are leaking again"
    exit 1
fi
if ! ( cd "$WORK" && COME_CFLAGS="-fsanitize=address -g" "$COME" build main.co -o app_asan > build_asan.log 2>&1 ); then
    echo "FAIL: map scope project did not build under ASan"
    cat "$WORK/build_asan.log"
    exit 1
fi
# Leak detection off: hand-written runtime C (std.c) parents its
# allocations to std's module context, which lives to program
# end by design - those program-end leftovers are noise here.
# The gate targets use-after-free and overflow, the class
# that bit the tokenizer.
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/asan_err.txt"; then
    echo "FAIL: map scope project failed under ASan (use-after-free or similar)"
    cat "$WORK/asan_err.txt" | head -30
    exit 1
fi
echo "COIL-157 map scope test passed! (peak RSS ${PEAK} kB)"
