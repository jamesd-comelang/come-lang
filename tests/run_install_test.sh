#!/bin/bash
# COIL-130: a clean-machine install runs hello.co.
#
# "Clean machine" here means: the distribution produced by
# `make dist-local` is copied to a fresh prefix (the layout
# the .deb installs to /usr/local/comelang), and hello.co
# is built and run from an unrelated directory with a
# scrubbed environment - no repository checkout anywhere
# in the compiler's search paths, no COME_* variables, no
# reliance on the current working directory. The compiler
# must find its depot (lib/modules, include, libcome.a)
# relative to its own executable and nothing else.
set -e
cd "$(dirname "$0")/.."

make dist-local > /tmp/install_dist.log 2>&1 || {
    cat /tmp/install_dist.log
    echo "INSTALL: make dist-local failed"
    exit 1
}

CLEAN=$(mktemp -d)
trap 'rm -rf "$CLEAN"' EXIT
PREFIX="$CLEAN/usr/local/comelang"
mkdir -p "$PREFIX" "$CLEAN/usr/bin" "$CLEAN/work"
cp -r build/dist/* "$PREFIX/"
ln -s ../local/comelang/bin/come "$CLEAN/usr/bin/come"
cp examples/hello.co "$CLEAN/work/"

# Build with a scrubbed environment from the work dir.
cd "$CLEAN/work"
env -i PATH=/usr/bin:/bin HOME="$CLEAN" "$CLEAN/usr/bin/come" build hello.co > build.log 2>&1 || {
    cat build.log
    echo "INSTALL: clean-room build of hello.co failed"
    exit 1
}

OUT=$(./hello)
if [ "$OUT" != "Hello world!" ]; then
    echo "INSTALL: hello printed '$OUT', expected 'Hello world!'"
    exit 1
fi
echo "INSTALL: clean-room install built and ran hello.co ('$OUT')"
