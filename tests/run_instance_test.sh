#!/bin/bash
# COIL-140: two ways to use a module (spec 3.6). Builds
# tests/instance_project in a scratch dir and diffs the
# program's output against expected.txt: independent
# per-instance state (scalar, string, and buffer fields),
# init/exit hooks bound to the instance, an instance passed
# as a parameter, a group (directory) module with its state
# in one file and functions in another, destroyed-instance
# calls returning zero values with ERR set, an idempotent
# destroy, and the stateless namespace still working in a
# stateful module. Then two negative projects: a namespace
# call to a stateful function (the error names the witness
# chain) and create() on a stateless module.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp tests/instance_project/main.co tests/instance_project/counter.co \
   tests/instance_project/plain.co "$WORK/"
cp -r tests/instance_project/panel "$WORK/"
if ! ( cd "$WORK" && "$COME" build main.co -o app > build.log 2>&1 ); then
    echo "FAIL: instance project did not build"
    cat "$WORK/build.log"
    exit 1
fi
"$WORK/app" > "$WORK/out.txt"
if ! diff -u tests/instance_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: instance project output mismatch (see diff above)"
    exit 1
fi

cp tests/instance_project/bad_ns.co "$WORK/"
if ( cd "$WORK" && "$COME" build bad_ns.co -o badapp > bad_ns.log 2>&1 ); then
    echo "FAIL: a namespace call to a stateful function was accepted"
    exit 1
fi
if ! grep -q "cannot be called on the namespace: it uses module state (bump -> count)" "$WORK/bad_ns.log"; then
    echo "FAIL: the namespace error does not name the state chain"
    cat "$WORK/bad_ns.log"
    exit 1
fi

cp tests/instance_project/bad_create.co "$WORK/"
if ( cd "$WORK" && "$COME" build bad_create.co -o badapp2 > bad_create.log 2>&1 ); then
    echo "FAIL: create() on a stateless module was accepted"
    exit 1
fi
if ! grep -q "module 'plain' has no state to instantiate" "$WORK/bad_create.log"; then
    echo "FAIL: the create() error is unclear"
    cat "$WORK/bad_create.log"
    exit 1
fi

echo "COIL-140 module instance test passed!"
