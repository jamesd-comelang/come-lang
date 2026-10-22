#!/bin/bash
# COIL-131: the README describes the language as it is.
#
# Mechanical floor for a hand-reviewed document: terms
# from before the v2 decisions must not reappear (the
# .size() method was removed in COIL-134 in favour of
# .bytesize()), and the load-bearing v1 facts must be
# present (coroutines, the ERR model, the grammar, the
# current version).
set -e
cd "$(dirname "$0")/.."

if grep -qF '.size()' README.md; then
    echo "README: stale claim - .size() was replaced by .bytesize() (COIL-134)"
    exit 1
fi
for term in '.bytesize()' 'coroutine' 'ERR' 'docs/grammar.ebnf' 'v1.0.0' 'no implicit narrowing'; do
    if ! grep -qiF -- "$term" README.md; then
        echo "README: missing required v1 fact: $term"
        exit 1
    fi
done
echo "README: no pre-v2 claims; v1 facts present"
