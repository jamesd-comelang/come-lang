#!/bin/bash
# COIL-115 end-to-end: atexit callbacks run in reverse
# registration order at exit, and std.proc.exit(status) runs the
# callbacks and terminates with that status.
set -e
cd "$(dirname "$0")/.."

BIN1=/tmp/come_proc_test_$$
BIN2=/tmp/come_proc_exit_$$
trap 'rm -f "$BIN1" "$BIN2"' EXIT

./build/come build src/std/t/02-proc.co -o "$BIN1" > /dev/null
out=$("$BIN1")
expected="main-done
cb-second
cb-first"
if [ "$out" != "$expected" ]; then
    echo "FAIL: atexit order. Got:"
    echo "$out"
    exit 1
fi

./build/come build tests/proc_exit_probe.co -o "$BIN2" > /dev/null
set +e
out2=$("$BIN2")
code=$?
set -e
if [ "$code" != "3" ]; then
    echo "FAIL: proc.exit(3) gave exit code $code"
    exit 1
fi
expected2="before-exit
bye-cb"
if [ "$out2" != "$expected2" ]; then
    echo "FAIL: proc.exit output. Got:"
    echo "$out2"
    exit 1
fi

echo "run_proc_test: atexit order and proc.exit verified"
