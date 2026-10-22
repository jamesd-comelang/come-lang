# Testing Convention

## COME Language Tests

Following Perl's convention, COME unit tests are placed in a `t/` subdirectory within each module.

### Structure

```
src/string/
├── string.c           # Implementation
└── t/                 # Tests directory
    ├── 01-basic.co    # Basic functionality tests
    ├── 02-search.co   # Search method tests
    └── 03-transform.co # Transformation tests

src/net/http/
├── http.c
└── t/
    ├── 01-request.co
    └── 02-response.co
```

### Naming Convention

- Test directory: `t/` in each module directory
- Test files: Numbered prefixes for execution order (e.g., `01-basic.co`, `02-advanced.co`)
- Or descriptive names without numbers if order doesn't matter

### Running Tests

```bash
# Run all COME tests
make test-come

# Run specific module tests
for test in src/string/t/*.co; do
    ./build/come build "$test" -o /tmp/test && /tmp/test && echo "✓ $test" || echo "✗ $test"
done
```

### Example Test File

```come
// src/string/t/01-basic.co
module main

import std
import string

int main() {
    // Test string creation
    string s = "Hello"
    if (s.len() != 5) {
        std.printf("FAIL: Expected length 5, got %d\n", s.len())
        return 1
    }
    
    // Test string comparison
    string s2 = "Hello"
    if (s.cmp(s2) != 0) {
        std.printf("FAIL: Strings should be equal\n")
        return 1
    }
    
    std.printf("PASS: All basic string tests passed\n")
    return 0
}
```

### Benefits

- **Organized**: All tests in one place per module
- **Scalable**: Easy to add many test files
- **Ordered**: Numbered prefixes allow controlling test execution order
- **Clean**: Keeps module directory uncluttered

---

# Spec Coverage Checklist (COIL-127)

Every section of `docs/Come_spec.md`, the tests that prove
it, and the state found by the audit. "Fixed" items were
spec violations repaired in COIL-127; the conformance suite
(`make test` from clean) runs all of them.

| Spec | Claim | Proven by | State |
| :--- | :--- | :--- | :--- |
| 1 | Design goals | the suite as a whole | ✓ |
| 2.1 | Four entry signatures; `args[0]` is the program name; int return is the exit code; void implies 0 | `tests/run_spec_test.sh` (entry_args/entry_void/entry_void_args) | ✓ **fixed**: both `void main` forms failed to compile (the generated prototype hardcoded `int`) |
| 2.2 | `//` and `/* */` comments | `src/core/t/12-const-alias.co` | ✓ |
| 3.1 | Import forms (single, grouped, parenthesized block); dot-notation references; `alias printf = std.out.printf` | `tests/run_spec_test.sh` (use_grouped), module project | ✓ |
| 3.2 | Resolution order `./X.co`, `./X/`, `./modules/X/`, depot; both file+dir is an error; `as` forces system import | `tests/run_module_project.sh`; `tests/run_spec_test.sh` (use_as) | ✓ **fixed**: `as` was accepted but ignored — it now skips local resolution |
| 3.3 | Local vs system modules; `extern` only in system modules | `tests/run_extern_test.sh` (COIL-111) | ✓ |
| 3.4 | Symbols private by default; only `export (...)` names are visible externally | `tests/run_spec_test.sh` (use_ok/use_secret) | ✓ **fixed** for function calls: non-exported calls now error naming the rule. **Gap:** constants and types are not namespaced per module today, so `geom.PI` / `geom.Point` do not resolve cross-module at all; enforcement covers the function-call path that exists |
| 3.5 | `module_init`/`module_exit`: import order, exact-reverse exit | `tests/run_module_project.sh` (COIL-108) | ✓ |
| 4.1 | `const`, grouped consts, trailing commas, inferred immutable types | `src/core/t/12-const-alias.co` | ✓ |
| 4.2 | `enum` value generator, auto-increment, `enum(start)` | `src/core/t/12-const-alias.co` | ✓ |
| 5.1 | Type alias; grouped paired form `alias (a, b) = (T1, T2)` | `src/core/t/12-const-alias.co` | ✓ **fixed**: the paired form was misparsed. Re-declaring a built-in alias (`i8`, …) as its own type is a no-op; as another type it errors |
| 5.2 | Constant alias, incl. as a fixed-array size (the section's example) | `src/core/t/12-const-alias.co` | ✓ **fixed**: an alias size resolved to 0 |
| 5.3 | Macro alias `alias SQUARE(x) = ((x) * (x))` | `src/core/t/12-const-alias.co` | ✓ **fixed**: macros were parsed and discarded (link error at use); they now expand at call sites |
| 6.1 | Primitive table; `'A'`/`'🤔'` literals; digit separators; `L` suffix | `src/core/t/01_types.co`, `08-numeric.co`, `12-const-alias.co` | ✓ (values verified arithmetically, not via `%d` on a long) |
| 6.2 | Structs; designated init; `packed`/`align(n)`; methods with implicit `self` | `src/core/t/05_aggregates.co`, `06-layout.co`, `14-var-ownership.co` | ✓ |
| 6.3 | Unions, overlapping C-compatible storage | `src/core/t/14-var-ownership.co` | ✓ |
| 6.4 | Fixed and dynamic arrays; bounds-checked indexing | `src/array/t/*`, `src/core/t/14-var-ownership.co` | ✓ **fixed**: a fixed array with no initializer was left null (length 0, writes segfaulted); it now allocates zero-filled |
| 6.5 | Immutable character-first strings; `s[i]` is `wchar`; `+` concatenates into a new string | `src/string/t/*`, `src/core/t/03_strings.co`, `10-strings.co`, `15-string-ops.co` | ✓ **fixed**: `+` failed to compile in every operand order; it now routes to a runtime concat |
| 6.6 | `map{K, V}`, `:` literals, missing key → zero value + `ERR` | `src/map/t/*` (COIL-102/103) | ✓ |
| 6.7 | The `link` type and its three methods | `src/coro/t/04-links.co` (COIL-123) | ✓ |
| 6.8 | No implicit narrowing; lossless widening only; `T(x)` casts | `src/core/t/08-numeric.co` (COIL-107) | ✓ |
| 7.1 | No raw pointers, no alloc/free; headered buffers; owners | `src/mem/t/*` | ✓ |
| 7.2 | Assignment aliases; `.chown()`; a reference may not outlive its owner — error **or promotion** | `src/core/t/14-var-ownership.co` (the section's own scope example), `src/mem/t/*` | ⚠ **Behavior pinned, text pending the founder's call.** The section's example compiles and the buffer stays usable (the context model promotes it). Whether the spec should describe promotion as the rule (A) or the compiler should reject (B) is an open design decision; the implementation today promotes. See the COIL-127 commit message |
| 7.3 | `resize()` semantics; `resize(0)` empty-never-null; fixed-array resize is an error; no string resize | `src/array/t/02-resize.co`, `06-oob.co`; codegen negatives (COIL-101/110) | ✓ |
| 7.4 | Universal `.type()/.bytesize()/.length()/.owner()/.chown()/.dup()` on every composite | arrays `src/array/t/*`; strings (COIL-119); maps (COIL-103); structs `src/core/t/14-var-ownership.co` | ✓ **fixed**: on structs, `.length()` emitted a string-list call (C type error); structs now answer field count, `sizeof`, and `"struct"` |
| 7.5 | `.dup(0)` shares, `.dup()` deep-copies, plain `=` aliases | `src/map/t/03-methods.co`, `src/core/t/15-string-ops.co`, `examples/ownership_demo.co` | ✓ |
| 8.1 | Declarations and definitions; prototypes | parsed on every build (std.co/string.co are prototype-heavy); COIL-108 fixtures | ✓ |
| 8.2 | Multiple return values via tuples; explicit destructure | `src/core/t/16-tuples.co` (the section's example), COIL-127 codegen block | ✓ **implemented in COIL-127**: previously the return type parsed to `void` and values were dropped. The destructure form binds two names (the parser's `(a, b)` pattern), matching the spec's examples |
| 8.3 | Functions passable only as arguments; exact signature match | `src/core/t/09-fnargs.co` (COIL-109) | ✓ |
| 9.1–9.3 | Coroutines: isolation, ownership transfer, links, `wait`, scheduling, lifecycle | `src/coro/t/*`, `tests/run_coro_test.sh` (COIL-120–126) | ✓ |
| 10.1 | `var` locks at first assignment; a later different type is a compile error | `src/core/t/14-var-ownership.co`; COIL-127 codegen negative | ✓ **fixed**: `var y = "hello"` registered the type as `var` (uses printed garbage) and type changes were silently accepted; the locked type is now recorded and enforced |
| 11.1 | If / else | `src/core/t/02_flow.co` | ✓ |
| 11.2 | Switch: no implicit fallthrough; explicit `fallthrough`; optional `default` | `src/core/t/13-switch-loops.co` | ✓ **fixed**: `fallthrough` was parsed and thrown away, so every case broke |
| 11.3 | `for` / `while` / `do-while`; `break` / `continue` | `src/core/t/02_flow.co`, `13-switch-loops.co` | ✓ |
| 12 | `ERR.no/str/set/fatal`; no exceptions | `src/core/t/07-err.co`, `src/std/t/03-err.co` (COIL-105/118) | ✓ |
| 13 | `extern "C"` confined to system modules | `tests/run_extern_test.sh` (COIL-111) | ✓ |
| 14 | Operators; ternary; `==` on composites is identity, contents via methods like `.cmp()` | `src/core/t/15-string-ops.co`, `08-numeric.co` | ✓ **fixed**: string ternaries decayed to `char*` (empty/garbage results), and `==` between two string variables ran a broken content compare; two variables now compare by identity, literals keep the content compare, per this section |
| 15 | `;` optional; separates statements on one line | `src/core/t/12-const-alias.co` | ✓ |
| 16 | Keyword list | — | Deferred to COIL-128 (the grammar story) by the roadmap's own dependency |
| 17 | Design summary table | rows map to the sections above | ✓ except the 7.2 wording (see 7.2) |
