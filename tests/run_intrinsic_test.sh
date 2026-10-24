#!/bin/bash
# Parity step 3 (narrow slice): array/list .length()
# as a header-count intrinsic instead of a
# come_string_list_len call. main.co runs every case
# for output and ASan; pos.co is intrinsic cases only
# (generated C must have `.count` and NO
# come_string_list_len); ctl.co is controls only
# (string length still come_string_len, map length
# still come_map_len, neither a `.count` field read).
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
cp tests/intrinsic_project/main.co "$WORK/main.co"
if ! "$COME" check "$WORK/main.co" > "$WORK/check.log" 2>&1; then
    echo "FAIL: intrinsic fixture did not pass come check"
    cat "$WORK/check.log" | head -30
    exit 1
fi
if ! "$COME" build "$WORK/main.co" -o "$WORK/app" > "$WORK/build.log" 2>&1; then
    echo "FAIL: intrinsic fixture did not build"
    cat "$WORK/build.log" | head -30
    exit 1
fi
if ! "$WORK/app" > "$WORK/out.txt" 2> "$WORK/err.txt"; then
    echo "FAIL: intrinsic fixture failed to run"
    cat "$WORK/err.txt" | head -30
    exit 1
fi
if ! diff -u tests/intrinsic_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: intrinsic output mismatch (see diff above)"
    exit 1
fi
GENC="$REPO/.ccache/main.co.c"
if ! grep -F -q ".count" "$GENC"; then
    echo "FAIL: no intrinsic .count in combined generated C"
    exit 1
fi
if ! grep -q "come_string_len" "$GENC"; then
    echo "FAIL: no come_string_len in combined generated C (string control lost its call)"
    exit 1
fi
if ! grep -q "come_map_len" "$GENC"; then
    echo "FAIL: no come_map_len in combined generated C (map control lost its call)"
    exit 1
fi
if grep -q "come_string_list_len" "$GENC"; then
    echo "FAIL: come_string_list_len in combined generated C (an array length was not intrinsic)"
    grep -n "come_string_list_len" "$GENC" | head -5
    exit 1
fi
if ! ( cd "$WORK" && COME_CFLAGS="-fsanitize=address -g" "$COME" build main.co -o app_asan > build_asan.log 2>&1 ); then
    echo "FAIL: intrinsic project did not build under ASan"
    cat "$WORK/build_asan.log" | head -30
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/asan_err.txt"; then
    echo "FAIL: intrinsic project failed under ASan"
    cat "$WORK/asan_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/intrinsic_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: intrinsic ASan output mismatch"
    exit 1
fi
cp tests/intrinsic_project/pos.co "$WORK/pos.co"
if ! "$COME" build "$WORK/pos.co" -o "$WORK/pos_app" > "$WORK/pos_build.log" 2>&1; then
    echo "FAIL: intrinsic positives fixture did not build"
    cat "$WORK/pos_build.log" | head -30
    exit 1
fi
if ! "$WORK/pos_app" > "$WORK/pos_out.txt" 2> "$WORK/pos_err.txt"; then
    echo "FAIL: intrinsic positives fixture failed to run"
    cat "$WORK/pos_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/intrinsic_project/expected_pos.txt "$WORK/pos_out.txt"; then
    echo "FAIL: intrinsic positives output mismatch"
    exit 1
fi
GENPOS="$REPO/.ccache/pos.co.c"
if ! grep -F -q ".count" "$GENPOS"; then
    echo "FAIL: no intrinsic .count in positives generated C"
    exit 1
fi
if grep -q "come_string_list_len" "$GENPOS"; then
    echo "FAIL: come_string_list_len in positives generated C (an array length was not intrinsic)"
    grep -n "come_string_list_len" "$GENPOS" | head -5
    exit 1
fi
cp tests/intrinsic_project/ctl.co "$WORK/ctl.co"
if ! "$COME" build "$WORK/ctl.co" -o "$WORK/ctl_app" > "$WORK/ctl_build.log" 2>&1; then
    echo "FAIL: intrinsic controls fixture did not build"
    cat "$WORK/ctl_build.log" | head -30
    exit 1
fi
if ! "$WORK/ctl_app" > "$WORK/ctl_out.txt" 2> "$WORK/ctl_err.txt"; then
    echo "FAIL: intrinsic controls fixture failed to run"
    cat "$WORK/ctl_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/intrinsic_project/expected_ctl.txt "$WORK/ctl_out.txt"; then
    echo "FAIL: intrinsic controls output mismatch"
    exit 1
fi
GENCTL="$REPO/.ccache/ctl.co.c"
# Function-level pins: each control keeps its call
# and does not become a field read.
STR_LINE=$(grep -n -F "come_main__string_len" "$GENCTL" | grep -F ") {" | head -1 | cut -d: -f1)
STR_BODY=$(sed -n "${STR_LINE},/^}/p" "$GENCTL")
if ! echo "$STR_BODY" | grep -q "come_string_len"; then
    echo "FAIL: string_len is not come_string_len in controls generated C"
    exit 1
fi
if echo "$STR_BODY" | grep -F -q ".count"; then
    echo "FAIL: string_len became a .count field read in controls generated C"
    exit 1
fi
MAP_LINE=$(grep -n -F "come_main__map_len" "$GENCTL" | grep -F ") {" | head -1 | cut -d: -f1)
MAP_BODY=$(sed -n "${MAP_LINE},/^}/p" "$GENCTL")
if ! echo "$MAP_BODY" | grep -q "come_map_len"; then
    echo "FAIL: map_len is not come_map_len in controls generated C"
    exit 1
fi
if echo "$MAP_BODY" | grep -F -q ".count"; then
    echo "FAIL: map_len became a .count field read in controls generated C"
    exit 1
fi
echo "intrinsic: OK"
