# Come Compilation Model: Project Layout & Module Resolution

This document defines the standard for source file discovery, intermediate file management, and build artifact organization in the Come programming language.

## 1. File Naming Conventions

To ensure clarity and prevent collisions with native C files, the following naming convention is enforced:
**Source File:** filename.co
**Transpiled File:** filename.co.c (Stored in the .ccache/ directory)
**Object File:** filename.o (Stored in the build/ directory)

---

## 2. Module Resolution Algorithm

When the compiler encounters an import abc statement, it performs a multi-tiered search. The search is designed to be flexible, supporting both flat and structured project layouts.

### Phase 1: Local Context Search
The compiler first looks in the directory from which it was executed (the **Root**):
1.  ./abc.co
2.  ./abc/abc.co
3.  ./modules/abc.co

### Phase 2: Source Directory Search (./src)
If the module is not found in the Root, the compiler looks for a ./src directory and repeats the pattern:
1.  ./src/abc.co
2.  ./src/abc/abc.co
3.  ./src/modules/abc.co

### Phase 3: System Repository Search
If all local searches fail, the compiler falls back to the global installation path:
1.  /usr/local/lib/come/modules/abc.co (or the equivalent path on your OS)



---

## 3. Directory Structures

The compiler automatically manages two working directories to keep the source tree clean.

### The .ccache/ Directory
**Purpose:** Stores intermediate .co.c files.
**Logic:** Created in the Project Root. 
**Mirroring:** If the source is located at src/network/client.co, the transpiled code will be at .ccache/src/network/client.co.c.

### The build/ Directory
**Purpose:** Stores binary object files and final targets (executables/libraries).
**Logic:** Generated only during project-wide builds (come build .).



---

## 4. Build Modes

| Command | Entry Point | Transpilation Path | Output Location |
| :--- | :--- | :--- | :--- |
| come build <file>.co | Specific File | .ccache/<file>.co.c | Current Working Directory |
| come build . | main.co | .ccache/ (mirrored) | build/ |

---

## 5. Incremental Compilation

To minimize power consumption ("Low-Watt" philosophy) and maximize speed, the compiler uses timestamp validation:
1.  Compare mtime of source.co with .ccache/source.co.c.
2.  If the .co file is newer, re-transpile.
3.  If the .co.c file is newer, skip transpilation and proceed to linking/compilation of the existing object file.

---

## 6. Implementation Notes for the Transpiler
When generating C code for imports:
import abc should map to the logic found in the resolved .co file.
If a module requires state, the transpiler must ensure abc.init() is called before use and abc.exit() is called upon program termination.

