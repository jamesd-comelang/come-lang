#!/bin/bash
# COIL-127: spec-level program fixtures - the four entry
# signatures (2.1), export visibility (3.4), and `as`
# forcing system resolution (3.2). Each check builds a
# whole program in a scratch dir (the compiler's module
# resolution and transpile cache are directory-relative).
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $1"; exit 1; }

# --- Entry signatures (spec 2.1) ---
mkdir -p "$WORK/entry"
cp tests/spec_programs/entry_args.co tests/spec_programs/entry_void.co \
   tests/spec_programs/entry_void_args.co "$WORK/entry/"

( cd "$WORK/entry" && "$COME" build entry_args.co -o ea > log 2>&1 ) \
    || fail "int main(string args[]) builds"
out=$("$WORK/entry/ea" hello); rc=$?
[ $rc -eq 3 ] || fail "entry_args exit code $rc (want 3)"
[ "$out" = "entry args ok" ] || fail "entry_args output '$out'"
echo "spec entry int+args: OK"

( cd "$WORK/entry" && "$COME" build entry_void.co -o ev > log 2>&1 ) \
    || fail "void main() builds"
out=$("$WORK/entry/ev"); rc=$?
[ $rc -eq 0 ] || fail "entry_void exit code $rc (want 0)"
[ "$out" = "entry void ok" ] || fail "entry_void output '$out'"
echo "spec entry void: OK"

( cd "$WORK/entry" && "$COME" build entry_void_args.co -o eva > log 2>&1 ) \
    || fail "void main(string args[]) builds"
out=$("$WORK/entry/eva" there); rc=$?
[ $rc -eq 0 ] || fail "entry_void_args exit code $rc (want 0)"
[ "$out" = "entry void args ok there" ] || fail "entry_void_args output '$out'"
echo "spec entry void+args: OK"

# --- Export visibility (spec 3.4) ---
mkdir -p "$WORK/exp"
cp tests/spec_programs/exports/*.co "$WORK/exp/"

( cd "$WORK/exp" && "$COME" build use_ok.co -o ok > log 2>&1 ) \
    || fail "exported symbol call builds"
out=$("$WORK/exp/ok")
[ "$out" = "export ok 42" ] || fail "use_ok output '$out'"
echo "spec export visible: OK"

# Grouped imports and the import alias (spec 3.1).
cp tests/spec_programs/exports/use_grouped.co "$WORK/exp/"
( cd "$WORK/exp" && "$COME" build use_grouped.co -o grp > log3 2>&1 ) \
    || fail "grouped import + alias build"
out=$("$WORK/exp/grp")
[ "$out" = "grouped 3" ] || fail "use_grouped output '$out'"
echo "spec import forms: OK"

if ( cd "$WORK/exp" && "$COME" build use_secret.co -o bad > log 2>&1 ); then
    fail "non-exported symbol call must not build"
fi
grep -q "not exported by module 'geom'" "$WORK/exp/log" \
    || fail "non-exported error does not name the rule"
echo "spec export hidden: OK"

# --- `as` forces system resolution (spec 3.2) ---
if ( cd "$WORK/exp" && "$COME" build use_as.co -o asapp > log2 2>&1 ); then
    fail "'import geom as sys' must not resolve the local geom"
fi
grep -q "Could not resolve import: geom" "$WORK/exp/log2" \
    || fail "as-import failure not reported as unresolved"
echo "spec import as: OK"
