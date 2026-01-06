# COME Memory Allocation: Hybrid Mode

COME uses a **context-based memory model** for safe and efficient memory management. Functions and methods can allocate memory in multiple scopes depending on the lifetime requirements.

---

## 1. Memory Scopes

| Scope | Lifetime | Use Case | Notes |
|-------|---------|----------|-------|
| **C Stack** | Function call | Primitive types (`int`, `bool`), small structs | Fast, no explicit free, cannot return pointers to stack memory. |
| **Function Context** | Function call | Temporary complex objects (`strings`, `arrays`, objects) | Allocated in a temporary memory context; automatically freed at function exit. |
| **Module Context** | Module lifetime | Objects that live beyond function calls (runtime objects, state) | Encapsulated within the module; freed when module unloads or context is destroyed. |

---

## 2. Example Usage

```come
server() {
    // 1. Fast primitive on stack
    int port = 443

    // 2. Temporary object in function context
    var tmp_ctx = module.create_function_ctx()
    net_tcp_connection* conn = mem_talloc_alloc(sizeof(net_tcp_connection), tmp_ctx)

    // 3. Persistent object in module context
    var listener = mem_talloc_alloc(sizeof(net_tcp_listener), main.module_ctx)
}
```

---

## 3. Guidelines

- Use **stack memory** for short-lived primitives.  
- Use **function context** for temporary objects within a function.  
- Use **module context** for state or objects that need to persist beyond a function call.  
- Hybrid mode allows **efficient, safe, and predictable memory usage** without a garbage collector.

---

## 4. Benefits

- **Safety:** Objects are automatically freed when their context ends.  
- **Performance:** Stack allocation for primitives keeps it fast.  
- **Clarity:** Memory ownership is explicit, consistent with COME’s module and runtime model.