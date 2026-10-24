#!/bin/bash
# COIL-153: a string literal passed to another module's
# function materializes as a string object when the
# callee's parameter is a string - in plain calls and in
# tuple destructuring alike (the destructure's call rides
# the same emission path). Builds tests/strargs_project in
# a scratch dir and diffs the output.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp tests/strargs_project/*.co tests/strargs_project/expected.txt "$WORK/"
if ! ( cd "$WORK" && "$COME" build main.co -o app > build.log 2>&1 ); then
    echo "FAIL: strargs project did not build"
    cat "$WORK/build.log"
    exit 1
fi
"$WORK/app" > "$WORK/out.txt"
if ! diff -u tests/strargs_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: strargs project output mismatch (see diff above)"
    exit 1
fi
echo "COIL-153 string argument test passed!"
