# COME Language Module & Property Access Spec

## 1. Module Types

COME distinguishes **two kinds of modules**:

| Type | Description | Key Features |
|------|------------|--------------|
| **Source Module** | Compile-time namespace for organizing methods, types, and constants. | - No runtime state or memory. <br> - Provides static organization across source code. <br> - Exported functions/types visible to other modules. |
| **Runtime Module** | Stateful entity with its own namespace and memory during program execution. | - Encapsulates objects, state, and methods. <br> - Can be instantiated or loaded dynamically. <br> - Provides controlled access to properties via `get`/`set`. |

---

## 2. Objects

- Objects exist **inside modules** (source or runtime). 
- Each object may contain:

| Component | Default Visibility | Notes |
|-----------|-----------------|------|
| `export bool` | Publicly accessible | Provides automatic default `get`/`set` |
| `export int` | Publicly accessible | Provides automatic default `get`/`set` |
| `export string` | Publicly accessible | Provides automatic default `get`/`set` |

---

## 3. Default Property Access

- **All exported properties automatically have `get` and `set` methods**:

```come
obj.get(property_name)
obj.set(property_name, value)

