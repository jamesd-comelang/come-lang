#!/bin/bash
# COIL-120 e2e: coroutine scheduling is deterministic, so the
# fixture's whole stdout must match the expected trace exactly.
set -u
cd "$(dirname "$0")/.."

./build/come build tests/coro_interleave.co -o build/tests/coro_interleave || exit 1
out=$(./build/tests/coro_interleave)
expected=$(printf 'M0 A0 B0 M1 A1 B1 M2 A2 B2 M3 \nnoop\nsummer total=15 vals=5 last=50\ndone')
if [ "$out" != "$expected" ]; then
    echo "FAIL: coro interleave output mismatch"
    echo "--- got ---"
    echo "$out"
    echo "--- want ---"
    echo "$expected"
    exit 1
fi
echo "coro interleave e2e: OK"
