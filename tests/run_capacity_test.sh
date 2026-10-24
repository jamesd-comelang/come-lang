#!/bin/bash
# COIL-147: capacity. Past the old fixed ceilings a file
# did not just fail - it failed with wandering errors at
# innocent lines, or compiled with constructs silently
# dropped. The token list now grows with the file, and a
# single construct past 1024 elements is a loud error
# naming the fix, never a crash or a wrong binary.
#
# Every shape below is generated in a scratch dir. The
# matrix is fn0(3) == 3 when fn0's if-chain is intact:
# the silent failure mode printed -1 instead.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

gen_matrix() { # nfns nifs file
    python3 - "$1" "$2" "$3" <<'PYEOF'
import sys
nf, ni, path = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
lines = ["module main", "import std", ""]
for f in range(nf):
    lines.append(f"int fn{f}(int x) {{")
    for i in range(ni):
        lines.append(f"    if (x == {i + 1}) {{ return {i + 1} }}")
    lines.append("    return -1")
    lines.append("}")
    lines.append("")
lines.append("int main() {")
lines.append("    std.out.printf(\"%d\\n\", fn0(3))")
lines.append("    return 0")
lines.append("}")
open(path, "w").write("\n".join(lines) + "\n")
PYEOF
}

for shape in 25x15 26x15 13x30 30x15 100x5 1x400 2x400 40x10; do
    nf=${shape%x*}; ni=${shape#*x}
    gen_matrix "$nf" "$ni" "m$shape.co"
    if ! "$COME" build "m$shape.co" -o "m$shape.bin" > "m$shape.log" 2>&1; then
        echo "FAIL: matrix shape $shape did not build"
        head -3 "m$shape.log"
        exit 1
    fi
    got=$("./m$shape.bin")
    if [ "$got" != "3" ]; then
        echo "FAIL: matrix shape $shape printed '$got', expected 3 (constructs dropped?)"
        exit 1
    fi
done

# A block just under the per-construct limit still works.
python3 - <<'PYEOF'
lines = ["module main", "import std", "", "int main() {"]
lines.append("    int v = 0")
for i in range(1000):
    lines.append("    v = v + 1")
lines.append("    std.out.printf(\"%d\\n\", v)")
lines.append("    return 0")
lines.append("}")
open("block1000.co", "w").write("\n".join(lines) + "\n")
lines = ["module main", "import std", "", "int main() {"]
lines.append("    int v = 0")
for i in range(1100):
    lines.append("    v = v + 1")
lines.append("    std.out.printf(\"%d\\n\", v)")
lines.append("    return 0")
lines.append("}")
open("block1100.co", "w").write("\n".join(lines) + "\n")
PYEOF
if ! "$COME" build block1000.co -o block1000.bin > block1000.log 2>&1; then
    echo "FAIL: a 1000-statement block did not build"
    head -3 block1000.log
    exit 1
fi
if [ "$(./block1000.bin)" != "1000" ]; then
    echo "FAIL: 1000-statement block printed the wrong total"
    exit 1
fi

# Past the per-construct limit: loud error, no crash, no
# silent success.
if "$COME" build block1100.co -o block1100.bin > block1100.log 2>&1; then
    echo "FAIL: a 1100-statement block was accepted"
    exit 1
fi
if ! grep -q "more than 1024 elements" block1100.log; then
    echo "FAIL: the over-limit error is unclear"
    head -3 block1100.log
    exit 1
fi

echo "COIL-147 capacity test passed!"
