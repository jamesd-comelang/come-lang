#!/bin/bash
# COIL-128: grammar/parser conformance.
#
# The published grammar (docs/grammar.ebnf) must accept
# exactly what the compiler accepts on the test corpus:
#   1. The keyword sets agree: spec section 16, the lexer,
#      and the grammar's quoted terminals. The only
#      sanctioned differences are the contextual keywords
#      (as, packed, align - identifiers outside their
#      grammar positions) and "method" (reserved by the
#      lexer for a legacy in-struct marker the grammar
#      deliberately omits).
#   2. Every grammar production family is exercised by at
#      least one corpus file (coverage map below).
#   3. Corpus programs build (the sweep in run_tests.sh
#      runs them; here the standalone files are compiled).
#   4. Programs the grammar excludes fail to build.
set -e
cd "$(dirname "$0")/.."
COME=./build/come
GRAMMAR=docs/grammar.ebnf
fail() { echo "GRAMMAR: $1"; exit 1; }

# --- 1. Keyword sets -------------------------------------------------
sed -n '/^# 16\. Keywords/,/^# 17\./p' docs/Come_spec.md \
    | grep -o '`[a-z_][a-z0-9_]*`' | tr -d '`' | sort -u > /tmp/gram_spec_kw.txt
grep -o 'MATCH_KEYWORD("[a-z_]*"' src/core/lexer.c \
    | sed 's/MATCH_KEYWORD("//; s/"//' | sort -u > /tmp/gram_lexer_kw.txt
grep -o '"[a-z][a-z0-9_]*"' "$GRAMMAR" | tr -d '"' | sort -u > /tmp/gram_grammar_kw.txt

# spec-only words must be exactly the contextual keywords
[ "$(comm -23 /tmp/gram_spec_kw.txt /tmp/gram_lexer_kw.txt | tr '\n' ' ')" = "align as packed " ] \
    || fail "spec/lexer keyword drift (spec-only): $(comm -23 /tmp/gram_spec_kw.txt /tmp/gram_lexer_kw.txt)"
# lexer-only words must be exactly the reserved legacy marker
[ "$(comm -13 /tmp/gram_spec_kw.txt /tmp/gram_lexer_kw.txt | tr '\n' ' ')" = "method " ] \
    || fail "spec/lexer keyword drift (lexer-only): $(comm -13 /tmp/gram_spec_kw.txt /tmp/gram_lexer_kw.txt)"
# every spec keyword appears as a quoted terminal in the grammar
# ("main" is tokenized by the lexer but is an ordinary
# identifier in the grammar - the entry point's name)
missing=$(comm -23 /tmp/gram_spec_kw.txt /tmp/gram_grammar_kw.txt | grep -v '^main$' || true)
[ -z "$missing" ] || fail "grammar is missing spec keywords: $missing"
echo "GRAMMAR: keyword sets agree (spec = lexer + contextual as/packed/align - reserved method)"

# --- 2. Production coverage ------------------------------------------
CORPUS=$(find src tests examples -name '*.co' -not -path '*/draft/*' | sort)
cover() { # production, pattern
    if ! grep -lqE -- "$2" $CORPUS; then
        fail "production '$1' is not exercised by any corpus file (pattern: $2)"
    fi
}
cover module_decl        '^module '
cover import_stmt        '^import '
cover import_grouped     'import \('
cover import_as          'import [a-z_.]+ as '
cover export_decl        'export \('
cover const_group        'const \('
cover enum_value         'enum'
cover enum_sized         'enum\('
cover alias_single       'alias [A-Za-z_.]+ ='
cover alias_grouped      'alias \('
cover alias_dotted       'alias [A-Za-z_]+ = [a-z]+\.[a-z]+'
cover map_type           'map\{'
cover packed_struct      'packed struct'
cover align_struct       'align\('
cover lifecycle          'module_(init|exit)'
cover coroutine_decl     '^coroutine '
cover spawn_stmt         'spawn '
cover colink             'colink\(\)'
cover wait_builtin       'wait\('
cover link_type          '\blink\b'
cover extern_decl        'extern "C"'
cover tuple_return       '^\((int|string|long|bool), '
cover destructure        '\([a-z_]+, [a-z_]+\) ='
cover fn_param           'compare\(int a, int b\)'
cover method_decl        '^[a-z]+ [A-Z][A-Za-z]*\.[a-z_]+\('
cover fallthrough        'fallthrough'
cover switch_stmt        'switch \('
cover do_while           'do \{'
cover digit_separator    "[0-9]'[0-9]"
cover long_suffix        "[0-9]L\b"
cover hex_literal        '0x[0-9A-Fa-f]'
cover struct_literal     '\{ \.[a-z]+ ='
cover compound_assign    '(\+=|%=|<<=|>>=|&=|\|=|\^=)'
cover bitwise_ops        '(<<|>>| \| | \^ | & )'
cover ternary            '\? .+ : '
echo "GRAMMAR: all production families are exercised by the corpus"

# --- 3. Standalone corpus files compile ------------------------------
for f in src/*/t/*.co examples/*.co; do
    rm -rf .ccache
    if ! $COME build "$f" -o /tmp/gram_probe > /tmp/gram_probe.log 2>&1; then
        cat /tmp/gram_probe.log
        fail "corpus file $f no longer compiles"
    fi
done
echo "GRAMMAR: standalone corpus files compile"

# --- 4. Excluded programs fail ---------------------------------------
SCRATCH=$(mktemp -d)
negative() { # name, source
    printf '%s\n' "$2" > "$SCRATCH/$1.co"
    rm -rf .ccache
    if $COME build "$SCRATCH/$1.co" -o "$SCRATCH/$1" > "$SCRATCH/$1.log" 2>&1; then
        fail "program '$1' builds but the grammar excludes it"
    fi
}
negative bracket_literal 'module neg1
import std

int main() {
    int a[] = [1, 2, 3]
    return 0
}'
negative interface_decl 'module neg2
import std

interface Shape {
    int area()
}

int main() {
    return 0
}'
negative no_module 'import std

int main() {
    return 0
}'
rm -rf "$SCRATCH"
echo "GRAMMAR: excluded programs fail to build"
echo "Grammar conformance: OK"
