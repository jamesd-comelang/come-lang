# String Module Tests

This directory contains unit tests for the COME string module.

## Test Files

- `01-basic.co` - Basic string operations (creation, length, comparison)
- `02-search.co` - Search methods (find, rfind, chr, count)
- `03-transform.co` - Transformation methods (upper, lower, replace, trim)
- `04-split-join.co` - Split and join operations
- `05-regex.co` - Regular expression methods

## Running Tests

From project root:
```bash
for test in src/string/t/*.co; do
    ./build/come build "$test" -o /tmp/test && /tmp/test && echo "✓ $test" || echo "✗ $test"
done
```

## Writing Tests

Each test should:
- Return 0 on success
- Return non-zero on failure
- Print descriptive failure messages
- Use `std.printf` for output
