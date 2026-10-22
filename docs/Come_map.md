# Come Map Type Specification

The `map{K, V}` type is a statically typed, unordered key-value collection.

- `K` is the key type. V1 key types: integer types, `bool`, `wchar`, `string` (hashed by contents), and enums.
- `V` is the value type: one single type per map — a primitive, a `string`, a struct, an array, or another map.

Maps are composite objects and participate in Come ownership contexts.

## 1. Overview

A map associates keys of one fixed type with values of one fixed type, checked at compile time:

```come
map{string, string} labels = {"host": "localhost", "env": "prod"}
map{string, int} limits = {"port": 8080, "retries": 3}
```

Mixing value types in one map is a compile error — use two maps, or a struct when the shape is known.

## 2. Declaration and Initialization

```come
map{string, int} scores
map{string, int} m = {"alice": 90, "bob": 85}
```

Literals use `{key: value, ...}` pairs with `:`. Every key must be of type `K` and every value of type `V`.

## 3. Access Rules

```come
map{string, int} m = {"name": 0, "age": 30}

int age = m["age"]
m["age"] = 31
```

Reads and writes are statically typed: `m[key]` has type `V`, and storing anything other than a `V` is a compile error.

Reading a key that does not exist:

- returns the zero value of `V`
- sets `ERR`

## 4. Type Inference with `var`

`var` infers the full map type from the literal, like any other type:

```come
var m = {"a": 1}    // map{string, int}
int x = m["a"]      // reads are typed V (int here)
```

## 5. Key Lookup

Keys may be literals or variables of type `K`; both forms behave the same:

```come
map{string, int} limits = {"port": 8080, "retries": 3}

int p1 = limits["port"]

string key = "port"
int p2 = limits[key]
```

## 6. Methods

### Query Methods

- `m.length() -> int`
  Returns the number of key-value entries.

- `m.type() -> string`
  Returns `"map"`.

- `m.owner()`
  Returns the current owner context.

- `m.has(key) -> bool`
  Returns `true` if the key exists.

- `m.keys() -> K[]`
  Returns an array of all keys.
  The returned array is owned by the current local context by default.

### Mutation Methods

- `m.delete(key)`
  Removes the entry for `key`.
  If the key does not exist, `ERR` is set.

- `m.clear()`
  Removes all entries.

### Copy and Ownership Methods

- `m.dup(depth) -> map{K, V}`
  Returns a copy of the map (`.dup(0)` shares the buffers; no argument = deep copy), owned by the current local context by default.

- `m.chown(new_owner)`
  Transfers ownership to a new owner context.

## 7. Iteration

Use `.keys()` to iterate over a map. Iteration order is unspecified:

```come
map{string, int} m = {"alice": 90, "bob": 85}

string keys[] = m.keys()
for (int i = 0; i < keys.length(); i++) {
    std.out.printf("%s = %d\n", keys[i], m[keys[i]])
}
```

Values read back are statically typed, so no runtime type checks or dispatch are needed.

## 8. Nested Maps

A map's value type may itself be a map or an array:

```come
map{string, map{string, int}} table = {
    "row1": {"a": 1, "b": 2},
    "row2": {"a": 3, "b": 4}
}

map{string, int} row = table["row1"]
int a = row["a"]
```

Nested composite values follow normal ownership rules.

## 9. Ownership and Lifetime

A `map` is a composite object and belongs to an ownership context.

A newly created map is owned by the current active local context unless explicitly transferred.

```come
map{string, string} m = {"key": "value"}
m.chown(cache)
```

After transfer, `m` is owned by module `cache`.

Composite values retained by a map become part of the map's owned object graph and are reclaimed when the map's owner context ends.

## 10. Automatic Reclamation

User code does not manually free ordinary map memory.

Map memory is reclaimed automatically when its owner context ends:

- local-owned maps at local scope end
- module-owned maps at module finalization
- process-owned maps at program termination

## 11. Summary

- `map{K, V}` is statically typed and unordered.
- V1 key types: integer types, `bool`, `wchar`, `string`, enums.
- Reads are statically typed; missing-key reads return the zero value of `V` and set `ERR`.
- `var` infers map types from literals.
- New maps are owned by the current local context by default.
- Ownership may be promoted with `chown()`.
- Ordinary map memory is reclaimed automatically.
