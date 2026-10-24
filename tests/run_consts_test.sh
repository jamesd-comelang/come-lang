#!/bin/bash
# COIL-148: a constant is static to its module. Builds
# tests/consts_project in a scratch dir and diffs the
# output: sibling files share one definition (including a
# constant whose initializer reads another constant, and
# a const group), importers use exported constants
# qualified (duo2.LIMIT), and four negatives fail with
# Come-level errors - an unexported constant, an
# unqualified cross-module use, a duplicate constant in
# one module, and an assignment to a sibling file's
# constant.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp tests/consts_project/*.co "$WORK/"
cp -r tests/consts_project/duo2 tests/consts_project/grp tests/consts_project/dup "$WORK/"
if ! ( cd "$WORK" && "$COME" build main.co -o app > build.log 2>&1 ); then
    echo "FAIL: consts project did not build"
    cat "$WORK/build.log"
    exit 1
fi
"$WORK/app" > "$WORK/out.txt"
if ! diff -u tests/consts_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: consts project output mismatch (see diff above)"
    exit 1
fi

expect_fail() { # file, fragment
    if ( cd "$WORK" && "$COME" build "$1" -o negapp > neg.log 2>&1 ); then
        echo "FAIL: $1 was accepted but must fail"
        exit 1
    fi
    if ! grep -qF -- "$2" "$WORK/neg.log"; then
        echo "FAIL: $1 failed without the expected message:"
        echo "  wanted: $2"
        echo "  got: $(head -2 "$WORK/neg.log" | tr '\n' ' ')"
        exit 1
    fi
}

expect_fail bad_secret.co "'limits.SECRET' is not exported by module 'limits'"
expect_fail bad_unqualified.co "'MAXV' is not defined"
expect_fail bad_dup.co "constant 'X' is declared twice"

cp tests/consts_project/bad_assign.co "$WORK/duo2/bad_assign.co"
expect_fail main.co "cannot assign to 'LIMIT': it is a constant"
rm "$WORK/duo2/bad_assign.co"

echo "COIL-148 module constants test passed!"
