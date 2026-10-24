#!/bin/bash
# A5 (perf-experiments): frameless functions (spec 7 scopes).
# Builds tests/frameless_project, checks its output, then
# greps the generated C: the scalar leaves and the
# pass-through buffer return must carry no marker push,
# while the concatenating, break-using, and framed-calling
# functions must keep one. Runs under AddressSanitizer too:
# a stray pop-to-marker in a frameless function would free
# the caller's frame and show up as use-after-free.
set -u
cd "$(dirname "$0")/.."
REPO=$(pwd)
COME="$REPO/build/come"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp tests/frameless_project/main.co "$WORK/"
if ! ( cd "$WORK" && "$COME" build main.co -o app > build.log 2>&1 ); then
    echo "FAIL: frameless project did not build"
    cat "$WORK/build.log"
    exit 1
fi
"$WORK/app" > "$WORK/out.txt"
if ! diff -u tests/frameless_project/expected.txt "$WORK/out.txt"; then
    echo "FAIL: frameless project output mismatch (see diff above)"
    exit 1
fi
if ! ( cd "$WORK" && COME_CFLAGS="-fsanitize=address -g" "$COME" build main.co -o app_asan > build_asan.log 2>&1 ); then
    echo "FAIL: frameless project did not build under ASan"
    cat "$WORK/build_asan.log"
    exit 1
fi
if ! ASAN_OPTIONS=detect_leaks=0 "$WORK/app_asan" > "$WORK/out_asan.txt" 2> "$WORK/asan_err.txt"; then
    echo "FAIL: frameless project failed under ASan (use-after-free or similar)"
    cat "$WORK/asan_err.txt" | head -30
    exit 1
fi
if ! diff -u tests/frameless_project/expected.txt "$WORK/out_asan.txt"; then
    echo "FAIL: frameless project ASan output mismatch"
    exit 1
fi
python3 - "$WORK/.ccache/main.co.c" <<'PYEOF'
import re, sys
src = open(sys.argv[1]).read()
def body_of(fn):
    m = re.search(r"come_main__" + fn + r"\([^)]*\) \{", src)
    assert m, f"function {fn} not found in generated C"
    i = m.end() - 1
    depth = 0
    j = i
    while j < len(src):
        if src[j] == "{": depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0: break
        j += 1
    return src[i:j+1]
frameless = ["leaf", "leaf2", "strlen_only", "pick"]
framed = ["main", "cat", "with_break", "calls_framed"]
bad = []
for fn in frameless:
    if "come_scope_push_marker" in body_of(fn):
        bad.append(f"{fn} should be frameless but pushes a marker")
for fn in framed:
    if "come_scope_push_marker" not in body_of(fn):
        bad.append(f"{fn} should keep its frame but pushes no marker")
if bad:
    print("FAIL: " + "; ".join(bad))
    sys.exit(1)
print("frameless codegen checks passed")
PYEOF
if [ $? -ne 0 ]; then
    exit 1
fi
echo "A5 frameless test passed!"
