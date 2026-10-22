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

# COIL-125: a deadlocked program is reported, not left
# hanging (spec 9.3). The run must exit 1 with 'deadlock'
# on stderr, quickly.
./build/come build tests/coro_deadlock.co -o build/tests/coro_deadlock || exit 1
timeout 10 ./build/tests/coro_deadlock >build/tests/deadlock.out 2>build/tests/deadlock.err
rc=$?
if [ $rc -ne 1 ]; then
    echo "FAIL: deadlock program exit code $rc (want 1)"
    exit 1
fi
if ! grep -q "deadlock" build/tests/deadlock.err; then
    echo "FAIL: deadlock not reported on stderr"
    cat build/tests/deadlock.err
    exit 1
fi
echo "coro deadlock e2e: OK"

# COIL-125: main returning ends the program even with a
# coroutine that would run forever (spec 9.3).
./build/come build tests/coro_main_return.co -o build/tests/coro_main_return || exit 1
out=$(timeout 10 ./build/tests/coro_main_return)
rc=$?
if [ $rc -ne 0 ] || [ "$out" != "main leaving" ]; then
    echo "FAIL: main-return exit rc=$rc out='$out'"
    exit 1
fi
echo "coro main-return e2e: OK"

# COIL-126: the spec's own section 9 examples, run as
# written, plus the request/reply pattern under load.
./build/come build tests/coro_spec91.co -o build/tests/coro_spec91 || exit 1
out=$(./build/tests/coro_spec91) || { echo "FAIL: spec 9.1 example"; exit 1; }
if [ "$out" != "spec 9.1 ok: echo: ping" ]; then
    echo "FAIL: spec 9.1 output '$out'"
    exit 1
fi
echo "coro spec 9.1 e2e: OK"

./build/come build tests/coro_spec92.co -o build/tests/coro_spec92 || exit 1
out=$(./build/tests/coro_spec92)
expected=$(printf 'handled: one\ncoordinator done')
if [ "$out" != "$expected" ]; then
    echo "FAIL: spec 9.2 output mismatch"
    echo "$out"
    exit 1
fi
echo "coro spec 9.2 e2e: OK"

./build/come build tests/coro_spec_load.co -o build/tests/coro_spec_load || exit 1
out=$(timeout 20 ./build/tests/coro_spec_load) || { echo "FAIL: spec load run"; exit 1; }
expected=$(printf 'echo rounds ok\nspec load ok')
if [ "$out" != "$expected" ]; then
    echo "FAIL: spec load output mismatch"
    echo "$out"
    exit 1
fi
echo "coro spec load e2e: OK"
