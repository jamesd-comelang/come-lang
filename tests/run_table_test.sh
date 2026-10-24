#!/bin/bash
# COIL-142: build-once module tables. Builds
# tests/table_project in a scratch dir and diffs the output:
# a library module whose module-level map is filled once by
# its init hook per instance and read by every lookup, with
# two independent instances. Then the negative: a
# module-level map with an initializer in a singleton module
# is a clear Come error, not broken generated C.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp tests/table_project/main.co "$WORK/"
cp -r tests/table_project/kwlib "$WORK/"
if ! ( cd "$WORK" && "$COME" build main.co -o app > build.log 2>&1 ); then
    echo "FAIL: table project did not build"
    cat "$WORK/build.log"
    exit 1
fi
"$WORK/app" > "$WORK/out.txt"
if ! diff -u tests/table_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: table project output mismatch (see diff above)"
    exit 1
fi

cp tests/table_project/bad_init.co "$WORK/"
if ( cd "$WORK" && "$COME" build bad_init.co -o badapp > bad_init.log 2>&1 ); then
    echo "FAIL: a module-level map initializer in a singleton module was accepted"
    exit 1
fi
if ! grep -q "cannot have an initializer in a singleton module" "$WORK/bad_init.log"; then
    echo "FAIL: the initializer error is unclear"
    cat "$WORK/bad_init.log"
    exit 1
fi

echo "COIL-142 module table test passed!"
