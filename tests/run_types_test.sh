#!/bin/bash
# COIL-146: tuple and struct returns cross module
# boundaries. Builds tests/types_project in a scratch dir
# and diffs the output: destructuring another module's
# tuple function, a string return, a struct by value in
# both directions, forwarding a tuple call through a local
# wrapper, a directory module whose files use each other's
# struct and tuple functions, and a stateful module whose
# unexported tuple function (sitting after an uninitialized
# module variable) must not break its importers.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp tests/types_project/*.co "$WORK/"
cp -r tests/types_project/duo "$WORK/"
if ! ( cd "$WORK" && "$COME" build main.co -o app > build.log 2>&1 ); then
    echo "FAIL: types project did not build"
    cat "$WORK/build.log"
    exit 1
fi
"$WORK/app" > "$WORK/out.txt"
if ! diff -u tests/types_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: types project output mismatch (see diff above)"
    exit 1
fi

echo "COIL-146 cross-module types test passed!"
