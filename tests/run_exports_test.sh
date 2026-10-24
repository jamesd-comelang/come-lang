#!/bin/bash
# COIL-152: exports do not silently cap at 64. Generates a
# module exporting 200 constants (the tokenizer's shape:
# 186 TK_* constants plus functions) and checks the names
# past the old cap resolve from an importer; then a
# directory module exporting 1100 names in total must be
# refused loudly, not truncated.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

gen_consts() { # file, first, last
    local f=$1 a=$2 b=$3 i
    {
        echo "module $(basename "$f" .co)"
    } > /dev/null
    {
        echo "module big"
        printf "export ("
        for i in $(seq "$a" "$b"); do
            printf "C%d%s" "$i" "$([ "$i" -lt "$b" ] && echo ", ")"
        done
        echo ")"
        for i in $(seq "$a" "$b"); do echo "const C$i = $i"; done
    } > "$f"
}

gen_consts "$WORK/big.co" 1 200
cat > "$WORK/main.co" <<'EOF'
module main
import std
import big

int main() {
    std.out.printf("%d %d %d %d\n", big.C1, big.C64, big.C65, big.C200)
    return 0
}
EOF
if ! ( cd "$WORK" && "$COME" build main.co -o app > build.log 2>&1 ); then
    echo "FAIL: 200-export module did not build"
    cat "$WORK/build.log"
    exit 1
fi
got=$("$WORK/app")
if [ "$got" != "1 64 65 200" ]; then
    echo "FAIL: exports past the old cap resolved wrong: got '$got'"
    exit 1
fi

# Over the total limit (1100 across two files of one
# directory module): refused, with the limit named.
mkdir -p "$WORK/huge"
gen_consts "$WORK/huge/h1.co" 1 600
sed -i 's/module big/module huge/' "$WORK/huge/h1.co"
gen_consts "$WORK/huge/h2.co" 601 1100
sed -i 's/module big/module huge/; s/\bC\([0-9]\)/D\1/g' "$WORK/huge/h2.co"
sed -i 's/\bC\([0-9]\)/D\1/g' "$WORK/huge/h1.co"
cat > "$WORK/mainh.co" <<'EOF'
module main
import std
import huge

int main() {
    std.out.printf("%d\n", huge.D5)
    return 0
}
EOF
if ( cd "$WORK" && "$COME" build mainh.co -o apph > buildh.log 2>&1 ); then
    echo "FAIL: a module exporting 1100 symbols was accepted"
    exit 1
fi
if ! grep -qF "more than 1024 exported symbols" "$WORK/buildh.log"; then
    echo "FAIL: over-limit exports failed without naming the limit:"
    head -2 "$WORK/buildh.log"
    exit 1
fi

echo "COIL-152 export cap test passed!"
