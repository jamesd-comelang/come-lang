#!/bin/bash
# COIL-111: extern "C" only in system modules (spec 13 / 3.3).
# Positive: the user program in tests/extern_project imports the
# system module src/exttest.co (in the compiler's src/ tree) and
# calls libc abs() through its Come wrapper. The compiler runs
# from the repo root so src/ resolution finds the system module.
# Negative: the same declaration in a user module is an error.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if ! "$REPO/build/come" build tests/extern_project/main.co -o "$WORK/app" > "$WORK/pos.log" 2>&1; then
    echo "FAIL: system-module extern build did not succeed"
    cat "$WORK/pos.log"
    exit 1
fi
out=$("$WORK/app")
if [ "$out" != "magnitude: 42" ]; then
    echo "FAIL: expected 'magnitude: 42', got '$out'"
    exit 1
fi

mkdir -p "$WORK/neg"
cat > "$WORK/neg/main.co" <<'EOF'
module main
import std

extern "C" int abs(int x)

int main() {
    return abs(0 - 1)
}
EOF
if ( cd "$WORK/neg" && "$REPO/build/come" build main.co -o negapp > neg.log 2>&1 ); then
    echo "FAIL: extern in a user module was accepted"
    exit 1
fi
if ! grep -q 'extern "C" is only allowed in system modules' "$WORK/neg/neg.log"; then
    echo "FAIL: the spec-13 error was not reported"
    cat "$WORK/neg/neg.log"
    exit 1
fi

echo "COIL-111 extern test passed!"
