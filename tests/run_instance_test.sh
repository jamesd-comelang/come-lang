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
# stateful module. COIL-156 coverage: the lifecycle pair
# is the criterion - a variable-less module with the pair
# (beacon) instantiates and its hooks also run in the
# program chain, and a module with state but no pair
# (hoard) is namespace-only singleton state. Then the
# negative projects: a namespace call to a stateful
# function (the error names the witness chain), create()
# on a module without the pair (stateless plain, and
# stateful hoard), and create() at module scope
# (COIL-149: an instance is created inside a function
# only).
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp tests/instance_project/main.co tests/instance_project/counter.co \
   tests/instance_project/plain.co tests/instance_project/libw.co \
   tests/instance_project/hoard.co tests/instance_project/beacon.co "$WORK/"
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
if ! grep -q "module 'plain' cannot be instantiated: it does not define the create/destroy lifecycle pair" "$WORK/bad_create.log"; then
    echo "FAIL: the create() error is unclear"
    cat "$WORK/bad_create.log"
    exit 1
fi

cp tests/instance_project/bad_create2.co tests/instance_project/hoard.co "$WORK/"
if ( cd "$WORK" && "$COME" build bad_create2.co -o badapp4 > bad_create2.log 2>&1 ); then
    echo "FAIL: create() on a stateful module without the pair was accepted"
    exit 1
fi
if ! grep -q "module 'hoard' cannot be instantiated: it does not define the create/destroy lifecycle pair" "$WORK/bad_create2.log"; then
    echo "FAIL: the create() error for a pair-less stateful module is unclear"
    cat "$WORK/bad_create2.log"
    exit 1
fi

cp tests/instance_project/bad_pair.co tests/instance_project/halflib.co "$WORK/"
if ( cd "$WORK" && "$COME" build bad_pair.co -o badapp5 > bad_pair.log 2>&1 ); then
    echo "FAIL: a module with half a lifecycle pair was accepted"
    exit 1
fi
if ! grep -q "module 'halflib' defines create() but no destroy(): the lifecycle hooks come in a pair" "$WORK/bad_pair.log"; then
    echo "FAIL: the half-pair error does not name the missing half"
    cat "$WORK/bad_pair.log"
    exit 1
fi

cp tests/instance_project/bad_global.co "$WORK/"
if ( cd "$WORK" && "$COME" build bad_global.co -o badapp3 > bad_global.log 2>&1 ); then
    echo "FAIL: an instance created at module scope was accepted"
    exit 1
fi
if ! grep -q "a module instance can only be created inside a function" "$WORK/bad_global.log"; then
    echo "FAIL: the module-scope create() error is unclear"
    cat "$WORK/bad_global.log"
    exit 1
fi

echo "COIL-140 module instance test passed!"
