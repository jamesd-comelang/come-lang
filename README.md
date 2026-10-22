**Come(C Object and Module Extensions)** is a C-inspired **systems language** with:

* No raw pointers in user code
* Explicit, hierarchical ownership via arenas
* Headered dynamic arrays/strings as first-class objects
* Static typing with local inference (`var`)
* Modules as the only compilation unit
* Minimal syntax drift from C

The design consistently favors:

* **Predictability over magic**
* **Compile-time guarantees**
* **Explicitness in memory & visibility**
* **C mental model without C footguns**

---

## Testing

One command runs the whole conformance suite:

```sh
make test
```

It builds the compiler and examples, runs the C unit suites and
the end-to-end project tests (`tests/run_tests.sh`), compiles and
runs every Come-language test under `src/**/t/` (auto-discovered —
a new test joins the suite just by being added there), and runs
the examples as smoke tests. Anything failing stops the run.

## Core pillars

### 1. Modules = isolation + lifecycle

Each file declares exactly one module.
Local modules:

* Have private memory contexts
* Optional `module_init` / `module_exit`
* Deterministic init/exit ordering

System modules:

* API-only
* No memory or lifecycle

This is a clean separation between *code* and *runtime authority*. Good.

---

### 2. Memory model (very clear)

* No pointers in user code
* Arrays & strings are **headered buffers**
* Ownership is hierarchical:

  * Parent frees children
  * `.chown()` explicitly re-parents
* Fixed arrays stay fixed — no promotion to dynamic
* Buffers die with their owner; `resize(n)` is the only memory primitive

This feels like a **manual-but-safe arena system**, closer to:

> C + Rust ownership + Zig-style explicit allocation
> but without borrow checking or lifetimes exposed to the user.

---

### 3. Arrays & strings as unified objects

```text
[ size_in_bytes | element_count | data... ]
```

Key points:

* Strings are UTF-8 by default
* Arrays & strings share:

  * `.length()`
  * `.size()`
  * `.owner()`
  * `.chown()`

Memory rules are explicit and compiler-guaranteed, which avoids the usual “hidden heap allocation” problem found in higher-level languages.

---

### 4. Type system

* Fully static
* `var` is *inference*, not dynamic typing
* First assignment locks type
* No implicit imports
* Symbols are private by default

This encourages **intentional API design** and discourages global leakage.

---

### 5. Control flow safety

* `switch` has **no implicit fallthrough**
* `fallthrough` is explicit
* Semicolons are optional but meaningful

This removes one of C’s most common logic bugs without adding complexity.

---

### 6. Functions & returns

* Multiple return values via tuples
* Assignment destructuring is explicit
* No magic unpacking

This is clean, readable, and predictable.

---

## Early strengths (purely observational)

These stand out as *very well thought-through*:

1. **Memory rules are explicit**

   * Huge improvement over C, Go, and even Rust ergonomics-wise

2. **Ownership is visible but not verbose**

   * `.chown()` is rare but powerful
   * Default behavior is safe

3. **Module-private memory contexts**

   * Excellent for large systems and embedded use

4. **No pointer syntax at all**

   * Forces better abstractions without pretending memory doesn’t exist

5. **Enum-as-generator inside `const`**

   * Simple, expressive, zero extra syntax

