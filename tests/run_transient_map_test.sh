#!/bin/bash
# Parity step 4c-maps: transient maps. A map local
# that never escapes (only index puts/reads and
# built-in method use, no return, call arg, store,
# alias, chown, or outer-loop store) holds arena
# keys/values without eager promotion on put.
# Literal construction always promotes (it builds
# a temporary that is then adopted), even for a
# transient variable -- pinned function-level in
# main.co. main.co runs every case for output and
# ASan; pos.co is positives only (NO promote and
# NO scope-adopt anywhere); ctl.co is controls
# only (a return, a call arg, and an outer-loop
# store each disqualify -- promote present).
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
cp tests/transient_map_project/main.co "$WORK/main.co"
if ! "$COME" check "$WORK/main.co" > "$WORK/check.log" 2>&1; then
    echo "FAIL: transient-map fixture did not pass come check"
    cat "$WORK/check.log" | head -30
    exit 1
fi
if ! "$COME" build "$WORK/main.co" -o "$WORK/app" > "$WORK/build.log" 2>&1; then
    echo "FAIL: transient-map fixture did not build"
    cat "$WORK/build.log" | head -30
    exit 1
fi
if ! "$WORK/app" > "$WORK/out.txt" 2> "$WORK/err.txt"; then
    echo "FAIL: transient-map fixture failed to run"
    cat "$WORK/err.txt" | head -30
    exit 1
fi
if ! diff -u tests/transient_map_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: transient-map output mismatch (see diff above)"
    exit 1
fi
GENC="$REPO/.ccache/main.co.c"
if ! grep -q "come_string_promote" "$GENC"; then
    echo "FAIL: no promote in combined generated C (a control lost its eager adopt)"
    exit 1
fi
LIT_START=$(grep -n "come_main__map_literal(void)" "$GENC" | head -1 | cut -d: -f1)
if [ -z "$LIT_START" ]; then
    echo "FAIL: map_literal function not found in combined generated C"
    exit 1
fi
LIT_END=$(awk -v s="$LIT_START" 'NR > s && /^}/ { print NR; exit }' "$GENC")
if [ -z "$LIT_END" ]; then
    echo "FAIL: map_literal function end not found"
    exit 1
fi
if ! sed -n "${LIT_START},${LIT_END}p" "$GENC" | grep -q "come_string_promote"; then
    echo "FAIL: literal construction did not promote (the base==NULL guard is broken)"
    exit 1
fi
if ! ( cd "$WORK" && COME_CFLAGS="-fsanitize=address -g" "$COME" build main.co -o app_asan > build_asan.log 2>&1 ); then
    echo "FAIL: transient-map project did not build under ASan"
    cat "$WORK/build_asan.log" | head -30
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/asan_err.txt"; then
    echo "FAIL: transient-map project failed under ASan"
    cat "$WORK/asan_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/transient_map_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: transient-map ASan output mismatch"
    exit 1
fi
cp tests/transient_map_project/pos.co "$WORK/pos.co"
if ! "$COME" build "$WORK/pos.co" -o "$WORK/pos_app" > "$WORK/pos_build.log" 2>&1; then
    echo "FAIL: transient-map positives fixture did not build"
    cat "$WORK/pos_build.log" | head -30
    exit 1
fi
if ! "$WORK/pos_app" > "$WORK/pos_out.txt" 2> "$WORK/pos_err.txt"; then
    echo "FAIL: transient-map positives fixture failed to run"
    cat "$WORK/pos_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/transient_map_project/expected_pos.txt "$WORK/pos_out.txt"; then
    echo "FAIL: transient-map positives output mismatch"
    exit 1
fi
GENPOS="$REPO/.ccache/pos.co.c"
if grep -q "come_string_promote" "$GENPOS"; then
    echo "FAIL: promote in positives generated C (a transient put did not suppress adopt)"
    grep -n "come_string_promote" "$GENPOS" | head -5
    exit 1
fi
if grep -q "come_scope_adopt" "$GENPOS"; then
    echo "FAIL: scope-adopt in positives generated C (a transient put did not suppress adopt)"
    grep -n "come_scope_adopt" "$GENPOS" | head -5
    exit 1
fi
cp tests/transient_map_project/ctl.co "$WORK/ctl.co"
if ! "$COME" build "$WORK/ctl.co" -o "$WORK/ctl_app" > "$WORK/ctl_build.log" 2>&1; then
    echo "FAIL: transient-map controls fixture did not build"
    cat "$WORK/ctl_build.log" | head -30
    exit 1
fi
if ! "$WORK/ctl_app" > "$WORK/ctl_out.txt" 2> "$WORK/ctl_err.txt"; then
    echo "FAIL: transient-map controls fixture failed to run"
    cat "$WORK/ctl_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/transient_map_project/expected_ctl.txt "$WORK/ctl_out.txt"; then
    echo "FAIL: transient-map controls output mismatch"
    exit 1
fi
GENCTL="$REPO/.ccache/ctl.co.c"
if ! grep -q "come_string_promote" "$GENCTL"; then
    echo "FAIL: no promote in controls generated C (a control wrongly went transient)"
    exit 1
fi
echo "transient-map: OK"
