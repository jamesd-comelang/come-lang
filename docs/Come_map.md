# Come Map Type Specification

The `map` type is a string-keyed, dynamically typed associative container.

Each entry stores:

- a string key
- a value
- a runtime type tag

Maps are composite objects and participate in Come ownership contexts.

## 1. Overview

A `map` is intended for flexible structured data such as:

- configuration
- decoded text formats
- metadata
- loosely shaped nested objects

Example:

```come
map config = {
    "host": "localhost",
    "port": 8080,
    "debug": true
}
```

Keys are always `string`.

Values may be any supported map value type.

## 2. Supported Value Types

A map entry stores a runtime tag describing the value currently associated with the key.

Supported value categories:

- `string`
- integer
- floating-point
- `bool`
- `map`
- array

Example:

```come
map m = {}
m["name"] = "John"
m["age"] = 30
m["score"] = 9.5
m["active"] = true
m["meta"] = {}
m["tags"] = ["a", "b"]
```

Reassigning a key replaces both the stored value and its runtime type tag:

```come
m["port"] = "8080"
m["port"] = 8080
```

## 3. Declaration and Initialization

```come
map m = {}
map server = {
    "host": "localhost",
    "port": 8080,
    "tls": true
}
```

Trailing commas are allowed.

## 4. Access Rules

Map access is runtime-checked.

The target variable type determines the expected type of the retrieved value.

```come
map m = { "name": "John", "age": 30 }

string name = m["name"]
int age = m["age"]
```

If the stored value type matches the target type, the read succeeds.

If the key is missing or the stored value type does not match the target type:

- the zero value of the target type is returned
- `ERR` is set

Example:

```come
string bad = m["age"]
int missing = m["none"]
```

## 5. Type Inference with `var`

`var` cannot infer a map access result directly, because map values are runtime-tagged.

```come
var x = m["age"]   // compile error
```

Use an explicit target type:

```come
int x = m["age"]
string y = m["name"]
```

This rule applies to both literal keys and variable keys.

## 6. Key Lookup

Keys may be accessed by literal or variable:

```come
string host = config["host"]

string key = "port"
int port = config[key]
```

Both forms use the same runtime type-checking rule.

Literal keys do not receive special compile-time typing behavior.

## 7. Methods

### Query Methods

- `m.length() -> int`
  Returns the number of key-value entries.

- `m.size() -> long`
  Returns the total memory size of the map, including internal storage.

- `m.type() -> string`
  Returns `"map"`.

- `m.owner()`
  Returns the current owner context.

- `m.has(key) -> bool`
  Returns `true` if the key exists.

- `m.typeof(key) -> string`
  Returns the runtime type name of the value stored at `key`.
  If the key does not exist, returns `""` and sets `ERR`.

- `m.keys() -> string[]`
  Returns an array of all keys.
  The returned array is owned by the current local context by default.

### Mutation Methods

- `m.delete(key)`
  Removes the entry for `key`.
  If the key does not exist, `ERR` is set.

- `m.clear()`
  Removes all entries.

### Copy and Ownership Methods

- `m.dup([depth]) -> map`
  Returns a new map owned by the current local context by default.

- `m.chown(new_owner)`
  Transfers ownership to a new owner context.

## 8. Iteration

Use `.keys()` to iterate over a map:

```come
map m = { "name": "John", "age": 30, "active": true }

string keys[] = m.keys()
for (int i = 0; i < keys.length(); i++) {
    std.out.printf("key: %s, type: %s\n", keys[i], m.typeof(keys[i]))
}
```

Type-specific processing is done with explicit reads:

```come
string keys[] = m.keys()
for (int i = 0; i < keys.length(); i++) {
    string t = m.typeof(keys[i])

    if (t == "string") {
        string s = m[keys[i]]
        std.out.printf("%s = %s\n", keys[i], s)
    }

    if (t == "int") {
        int n = m[keys[i]]
        std.out.printf("%s = %d\n", keys[i], n)
    }
}
```

## 9. Nested Maps

Maps may contain nested maps and arrays:

```come
map server = {
    "host": "localhost",
    "port": 8080,
    "tls": {
        "enabled": true,
        "cert": "/etc/ssl/cert.pem"
    }
}

map tls = server["tls"]
string cert = tls["cert"]
```

Nested composite values follow normal ownership rules.

## 10. Ownership and Lifetime

A `map` is a composite object and belongs to an ownership context.

A newly created map is owned by the current active local context unless explicitly transferred.

```come
map m = { "key": "value" }
m.chown(cache)
```

After transfer, `m` is owned by module `cache`.

Composite values retained by a map become part of the map's owned object graph and are reclaimed when the map's owner context ends.

## 11. Automatic Reclamation

User code does not manually free ordinary map memory.

Map memory is reclaimed automatically when its owner context ends:

- local-owned maps at local scope end
- module-owned maps at module finalization
- process-owned maps at program termination

## 12. Summary

- `map` is string-keyed and dynamically typed.
- Each entry stores a runtime type tag.
- Reads are runtime-checked.
- Missing keys and type mismatches return the target zero value and set `ERR`.
- `var` does not infer directly from map access.
- New maps are owned by the current local context by default.
- Ownership may be promoted with `chown()`.
- Ordinary map memory is reclaimed automatically.
