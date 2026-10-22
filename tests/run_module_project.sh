#!/bin/bash
# COIL-108: directory modules and the module lifecycle (spec 3.2/3.5).
# Builds tests/module_project in a scratch dir and diffs the
# program's output against expected.txt (init order, cross-file
# calls, exact-reverse exit order), then checks that a module
# provided as both a file and a directory is rejected.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp tests/module_project/main.co tests/module_project/alpha.co "$WORK/"
cp -r tests/module_project/greeter "$WORK/"
if ! ( cd "$WORK" && "$COME" build main.co -o app > build.log 2>&1 ); then
    echo "FAIL: module project did not build"
    cat "$WORK/build.log"
    exit 1
fi
"$WORK/app" > "$WORK/out.txt"
if ! diff -u tests/module_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: module project output mismatch (see diff above)"
    exit 1
fi

mkdir -p "$WORK/amb/foo"
cp tests/module_project/amb/main.co tests/module_project/amb/foo.co "$WORK/amb/"
cp tests/module_project/amb/foo/x.co "$WORK/amb/foo/"
if ( cd "$WORK/amb" && "$COME" build main.co -o ambapp > amb.log 2>&1 ); then
    echo "FAIL: ambiguous module (foo.co and foo/) was accepted"
    exit 1
fi
if ! grep -q "ambiguous module 'foo'" "$WORK/amb/amb.log"; then
    echo "FAIL: ambiguity was not reported clearly"
    cat "$WORK/amb/amb.log"
    exit 1
fi

echo "COIL-108 module project test passed!"
