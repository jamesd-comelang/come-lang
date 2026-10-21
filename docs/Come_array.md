# Come Array Type Specification

Arrays in Come are dynamic headered buffers with bounds-aware access and ownership context tracking.

```text
[ uint size_in_bytes | uint element_count | element data... ]
```

## 1. Declaration and Initialization

```come
int arr[5] = {1, 2, 3, 4, 5}
int nums[] = [10, 20, 30]
int dyn[]
string names[] = ["Alice", "Bob"]
```

Fixed-size array declarations may be promoted to dynamic arrays when dynamic behavior is required, including:

- passing as a composite value
- returning from a function
- resizing
- ownership transfer
- storing into another composite object

## 2. Element Access

```come
int x = arr[0]
arr[2] = 99
```

Array indexing is bounds-checked.

On out-of-bounds read:

- the zero value of the element type is returned
- `ERR` is set

On out-of-bounds write:

- the write does not occur
- `ERR` is set

```come
int bad = arr[100]
```

## 3. Length and Storage

`a.length()` returns the logical number of elements currently stored.

`a.resize(n)` changes the array length to `n`.

- If `n` is greater than the current length, new elements are zero-initialized.
- If `n` is less than the current length, excess elements are discarded.

The implementation may reserve additional internal storage, but such capacity is not part of the language-visible model.

Example:

```come
int data[]
data.resize(1000)
data[999] = 1
```

This is valid because the array length is now 1000.

If explicit storage reservation is later added, it should use a separate method such as `reserve(n)` rather than overloading `resize()`.

## 4. Methods

### Query Methods

| Method | Returns | Description |
| :--- | :--- | :--- |
| `a.length()` | `int` | Returns the number of elements currently stored. |
| `a.size()` | `long` | Returns the total memory size in bytes, including header. |
| `a.type()` | `string` | Returns `"array"`. |
| `a.owner()` | composite | Returns the current owner context. |
| `a.contains(elem)` | `bool` | Returns `true` if `elem` exists in the array. |
| `a.index(elem)` | `int` | Returns the first matching index, or `-1`. |

### Mutation Methods

| Method | Returns | Description |
| :--- | :--- | :--- |
| `a.resize(n)` | `void` | Changes the logical length to `n`. |
| `a.append(elem)` | `void` | Appends `elem` to the end of the array. |
| `a.insert(index, elem)` | `void` | Inserts `elem` at `index`. |
| `a.remove(index)` | `void` | Removes the element at `index`. |
| `a.clear()` | `void` | Removes all elements. |
| `a.sort(cmp)` | `void` | Sorts the array in-place. |
| `a.reverse()` | `void` | Reverses the array in-place. |
| `a.set(value)` | `void` | Sets all elements to `value`. |
| `a.cpy(src)` | `void` | Copies elements from `src` into `a`. |

### New Array Methods

| Method | Returns | Description |
| :--- | :--- | :--- |
| `a.slice(start, end)` | `T[]` | Returns a new array from `start` (inclusive) to `end` (exclusive). |
| `a.dup([depth])` | `T[]` | Returns a copy of the array. |

Arrays returned by `slice()` and `dup()` are owned by the current local context by default.

### Ownership Methods

| Method | Returns | Description |
| :--- | :--- | :--- |
| `a.owner()` | composite | Returns the current owner context. |
| `a.chown(new_owner)` | `void` | Transfers ownership to a new owner context. |
| `a.dup([depth])` | `T[]` | Returns a copy of the array. |

## 5. Dynamic Growth

Arrays grow automatically on `append()` and `insert()`.

```come
int results[]
for (int i = 0; i < 100; i++) {
    results.append(i * i)
}
```

## 6. Ownership and Lifetime

Arrays are composite objects and therefore belong to ownership contexts.

A newly created array is owned by the current active local context unless explicitly transferred.

```come
int values[] = [1, 2, 3]
values.chown(cache)
```

After transfer, `values` is owned by module `cache`.

Nested composite elements stored in an array remain governed by normal composite ownership rules. If the array becomes the owning container for those objects, they are reclaimed when the array's owner context ends.

## 7. Automatic Reclamation

User code does not manually free ordinary array memory.

Array memory is reclaimed automatically when its owner context ends:

- local-owned arrays at local scope end
- module-owned arrays at module finalization
- process-owned arrays at program termination

## 8. Summary

- Arrays are dynamic headered buffers.
- Indexing is bounds-checked.
- `resize(n)` changes logical length.
- `append()` and `insert()` grow automatically.
- New arrays are owned by the current local context by default.
- Ownership may be promoted with `chown()`.
- Ordinary array memory is reclaimed automatically.
