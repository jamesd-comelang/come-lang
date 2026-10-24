#!/bin/bash
# Parity step 3 (TU): opt-in `come build --lto` for
# user-code-only LTO. The program builds and runs
# identically with and without --lto, and a verbose
# --lto build must show -flto in the actual cc
# commands (proving the flag reaches gcc, not just
# that it is accepted). The bench speedup itself
# (0.64 s -> ~0.37 s on the tokenizer) is measured
# outside this script, not asserted here (timing
# would be flaky in a test).
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
cp tests/lto_project/main.co tests/lto_project/helper.co "$WORK/"
if ! "$COME" build "$WORK/main.co" -o "$WORK/app" > "$WORK/build.log" 2>&1; then
    echo "FAIL: lto fixture did not build without --lto"
    cat "$WORK/build.log" | head -30
    exit 1
fi
if ! "$WORK/app" > "$WORK/out.txt" 2> "$WORK/err.txt"; then
    echo "FAIL: lto fixture failed to run without --lto"
    cat "$WORK/err.txt" | head -30
    exit 1
fi
if ! diff -u tests/lto_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: lto output mismatch without --lto"
    exit 1
fi
if ! "$COME" build -v --lto "$WORK/main.co" -o "$WORK/app_lto" > "$WORK/build_lto.log" 2>&1; then
    echo "FAIL: lto fixture did not build with --lto"
    cat "$WORK/build_lto.log" | head -30
    exit 1
fi
if ! grep -F -q -- "-flto" "$WORK/build_lto.log"; then
    echo "FAIL: no -flto in verbose --lto build commands"
    grep "CMD" "$WORK/build_lto.log" | head -5
    exit 1
fi
if ! grep -F -q -- "-O2" "$WORK/build_lto.log"; then
    echo "FAIL: no -O2 in verbose --lto build commands (LTO at -O0 does not inline)"
    exit 1
fi
# Functional pin: without --lto the helper functions
# are global symbols; with --lto they are inlined
# away (only the internalized ctx/exit_chain remain).
if ! nm "$WORK/app" | grep -q "come_helper__add"; then
    echo "FAIL: helper add symbol missing without --lto (test setup broken)"
    exit 1
fi
if nm "$WORK/app_lto" | grep -q "come_helper__add"; then
    echo "FAIL: helper add symbol still present with --lto (not inlined)"
    exit 1
fi
if nm "$WORK/app_lto" | grep -q "come_helper__mul"; then
    echo "FAIL: helper mul symbol still present with --lto (not inlined)"
    exit 1
fi
if ! "$WORK/app_lto" > "$WORK/out_lto.txt" 2> "$WORK/err_lto.txt"; then
    echo "FAIL: lto fixture failed to run with --lto"
    cat "$WORK/err_lto.txt" | head -30
    exit 1
fi
if ! diff -u tests/lto_project/expected.txt "$WORK/out_lto.txt"; then
    echo "FAIL: lto output mismatch with --lto"
    exit 1
fi
echo "lto: OK"
