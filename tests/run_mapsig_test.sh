#!/bin/bash
# COIL-151: map{...} in a function signature. Builds
# tests/mapsig_project in a scratch dir and diffs the
# output: a stateless module and a stateful (instance)
# module both take and return map{string, int} - the type
# must reach C as come_map_t in definitions, prototypes,
# and wrappers alike, never pasted raw.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp tests/mapsig_project/*.co tests/mapsig_project/expected.txt "$WORK/"
if ! ( cd "$WORK" && "$COME" build main.co -o app > build.log 2>&1 ); then
    echo "FAIL: mapsig project did not build"
    cat "$WORK/build.log"
    exit 1
fi
"$WORK/app" > "$WORK/out.txt"
if ! diff -u tests/mapsig_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: mapsig project output mismatch (see diff above)"
    exit 1
fi
echo "COIL-151 map signature test passed!"
