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

# COIL-121: spawned coroutines run to completion, including a
# spawn tree (parents spawning leaves).
./build/come build tests/coro_completion.co -o build/tests/coro_completion || exit 1
out=$(./build/tests/coro_completion)
expected=$(printf 'parent 1 start\nparent 2 start\nleaf 11\nleaf 12\nleaf 21\nleaf 22\nparent 1 end\nparent 2 end\nworker 7 acc 21\nall done')
if [ "$out" != "$expected" ]; then
    echo "FAIL: coro completion output mismatch"
    echo "--- got ---"
    echo "$out"
    echo "--- want ---"
    echo "$expected"
    exit 1
fi
echo "coro completion e2e: OK"

# COIL-123: links move data with ownership attached -
# ping-pong, producer EOF, array payloads, write-after-close.
./build/come build tests/coro_pingpong.co -o build/tests/coro_pingpong || exit 1
out=$(./build/tests/coro_pingpong)
expected=$(printf 'worker got: ping\nmain got: pong\ngot[0] = %s len 3\ngot[1] = %s len 3\ngot[2] = %s len 5\ngot[3] = %s len 0\nworker ERR: link is closed\narray payload: 7 8\nafter close read len 0\nmain ERR: link peer closed' "'one'" "'two'" "'three'" "''")
if [ "$out" != "$expected" ]; then
    echo "FAIL: coro pingpong output mismatch"
    echo "--- got ---"
    echo "$out"
    echo "--- want ---"
    echo "$expected"
    exit 1
fi
echo "coro pingpong e2e: OK"

# COIL-124: one wait listens to many channels; a finished
# producer reports through an empty read and leaves the set.
./build/come build tests/coro_wait.co -o build/tests/coro_wait || exit 1
out=$(./build/tests/coro_wait)
expected=$(printf 'message: fast\nfast worker closed\nquitter closed\nmessage: slow\nwait done')
if [ "$out" != "$expected" ]; then
    echo "FAIL: coro wait output mismatch"
    echo "--- got ---"
    echo "$out"
    echo "--- want ---"
    echo "$expected"
    exit 1
fi
echo "coro wait e2e: OK"
