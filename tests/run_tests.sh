#!/bin/bash
# Any failing build or test aborts the run: a stale binary must
# never masquerade as a pass.
set -e
mkdir -p build/tests
gcc -Wall -g -Isrc/include -Isrc/core/include tests/test_lexer.c src/core/lexer.c -o build/tests/test_lexer
./build/tests/test_lexer

gcc -Wall -g -Isrc/include -Isrc/core/include tests/test_parser.c src/core/parser.c src/core/lexer.c -o build/tests/test_parser
./build/tests/test_parser

gcc -Wall -g -Isrc/include -Isrc/core/include tests/test_codegen.c src/core/parser.c src/core/lexer.c src/core/codegen.c -o build/tests/test_codegen
./build/tests/test_codegen

gcc -Wall -g -D__STDC_WANT_LIB_EXT1__=1 -Isrc/include -Isrc/core/include tests/test_string.c src/string/string.c src/array/array.c src/mem/memory.c src/core/utils.c -o build/tests/test_string
./build/tests/test_string

gcc -Wall -g -Isrc/include -Isrc/core/include tests/test_mem.c src/mem/memory.c -o build/tests/test_mem
./build/tests/test_mem

gcc -Wall -g -Isrc/include -Isrc/core/include tests/test_std.c src/std/std.c src/string/string.c src/array/array.c src/mem/memory.c src/core/utils.c -o build/tests/test_std
./build/tests/test_std

gcc -Wall -g -D__STDC_WANT_LIB_EXT1__=1 -Isrc/include -Isrc/core/include tests/test_array.c src/array/array.c src/string/string.c src/mem/memory.c src/core/utils.c -o build/tests/test_array
./build/tests/test_array

gcc -Wall -g -Isrc/include -Isrc/core/include tests/test_coro.c src/coro/coro.c src/coro/link.c src/string/string.c src/array/array.c src/mem/memory.c src/core/utils.c -o build/tests/test_coro
./build/tests/test_coro

gcc -Wall -g -D__STDC_WANT_LIB_EXT1__=1 -Isrc/include -Isrc/core/include tests/test_oob.c src/array/array.c src/string/string.c src/mem/memory.c src/core/utils.c -o build/tests/test_oob
./build/tests/test_oob

bash tests/run_module_project.sh
bash tests/run_extern_test.sh
bash tests/run_proc_test.sh
bash tests/run_coro_test.sh
bash tests/run_spec_test.sh
bash tests/run_grammar_test.sh
bash tests/run_errors_test.sh
bash tests/run_install_test.sh
bash tests/run_readme_test.sh

# Examples are documentation that must run (COIL-119): every
# example at the top level of examples/ builds and exits 0.
# (examples/draft/ is parked and intentionally excluded.)
for ex in examples/*.co; do
    name=$(basename "$ex" .co)
    ./build/come build "$ex" -o "build/tests/example_$name" > /dev/null
    "./build/tests/example_$name" > /dev/null
    echo "example $name: OK"
done
