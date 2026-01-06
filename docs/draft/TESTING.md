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
