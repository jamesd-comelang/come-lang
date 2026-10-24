#!/bin/bash
# Design 1 (redesign): may-escape analysis. Runs
# `come escapes` on tests/escapes_project and diffs
# the classification against expected.txt, then runs
# `come check` on the fixture so the fixture is
# verified as compilable Come, not just parseable.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
if ! "$COME" escapes tests/escapes_project/main.co > /tmp/escapes_out.txt 2> /tmp/escapes_err.txt; then
    echo "FAIL: come escapes failed on the fixture"
    cat /tmp/escapes_err.txt
    exit 1
fi
if ! diff -u tests/escapes_project/expected.txt /tmp/escapes_out.txt; then
    echo "FAIL: escapes classification mismatch (see diff above)"
    exit 1
fi
if ! "$COME" check tests/escapes_project/main.co > /tmp/escapes_check.txt 2>&1; then
    echo "FAIL: escapes fixture did not pass come check"
    cat /tmp/escapes_check.txt | head -30
    exit 1
fi
echo "Escapes test passed!"
