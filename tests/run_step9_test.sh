#!/bin/bash
# Step 9 reframed: (a) cache invalidation on import change, (b) clear
# Come-level "not exported" error for tuple calls, not C "unknown type".
# No touch of the importer and no .ccache deletion between phases: the
# test fails if stale cached C is reused (phase 2 would succeed, phase 4
# would fail) or if the error is a C-level unknown type.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
cp tests/step9_project/pairmod.co tests/step9_project/main.co tests/step9_project/main_destr.co tests/step9_project/expected.txt "$WORK/"

fail() { echo "FAIL: $1"; cat "$WORK/$2" 2>/dev/null | head -30; rm -rf "$WORK"; exit 1; }

# Phase 1: exported -> build + run OK.
if ! "$COME" build "$WORK/main.co" -o "$WORK/app" > "$WORK/build1.log" 2>&1; then
    fail "phase 1 build failed (exported case should succeed)" build1.log
fi
if ! "$WORK/app" > "$WORK/out1.txt" 2> "$WORK/err1.txt"; then
    fail "phase 1 run failed" err1.txt
fi
if ! diff -u tests/step9_project/expected.txt "$WORK/out1.txt"; then
    fail "phase 1 output mismatch" out1.txt
fi

# Phase 2: remove pair from exports (importer untouched, cache kept).
sed -i 's/export (pair, dummy)/export (dummy)/' "$WORK/pairmod.co"
sleep 1
if "$COME" build "$WORK/main.co" -o "$WORK/app2" > "$WORK/build2.log" 2>&1; then
    fail "phase 2 build succeeded with stale cache (pair removed should fail)" build2.log
fi
if ! grep -q "not exported" "$WORK/build2.log"; then
    fail "phase 2 error is not the clear Come 'not exported' error" build2.log
fi
if grep -q "unknown type" "$WORK/build2.log"; then
    fail "phase 2 error fell through to C 'unknown type'" build2.log
fi

# Phase 3: destructure-only importer, same removed exports -> clear error.
if "$COME" build "$WORK/main_destr.co" -o "$WORK/app3" > "$WORK/build3.log" 2>&1; then
    fail "phase 3 build succeeded with stale cache (destructure, pair removed)" build3.log
fi
if ! grep -q "not exported" "$WORK/build3.log"; then
    fail "phase 3 error is not the clear Come 'not exported' error" build3.log
fi
if grep -q "unknown type" "$WORK/build3.log"; then
    fail "phase 3 error fell through to C 'unknown type'" build3.log
fi

# Phase 4: restore exports (importer untouched, cache kept) -> build + run OK.
sed -i 's/export (dummy)/export (pair, dummy)/' "$WORK/pairmod.co"
sleep 1
if ! "$COME" build "$WORK/main.co" -o "$WORK/app4" > "$WORK/build4.log" 2>&1; then
    fail "phase 4 build failed (restored exports should succeed via invalidation)" build4.log
fi
if ! "$WORK/app4" > "$WORK/out4.txt" 2> "$WORK/err4.txt"; then
    fail "phase 4 run failed" err4.txt
fi
if ! diff -u tests/step9_project/expected.txt "$WORK/out4.txt"; then
    fail "phase 4 output mismatch" out4.txt
fi

rm -rf "$WORK"
echo "step9: OK"
