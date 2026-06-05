# CLAUDE.md

## What This Project Is

brokit is a standalone C++20 library providing a JavaScript runtime with web-standard and system APIs, built on QuickJS. It is consumed by other projects (like `bro`) as a dependency. It does NOT own a DOM, render anything, or manage windows.

The API surface borrows from WinterCG (web-standard APIs) and Node.js (system APIs). Each API is independently installable.

## Build Commands

```bash
# Configure
cmake -B build

# Build (debug)
cmake --build build --config Debug

# Build (release)
cmake --build build --config Release
```

**Run tests:**

Windows (MSVC):
```bash
./build/tests/Debug/brokit_test.exe tests/js
```

Linux:
```bash
./build/tests/brokit_test tests/js
```

## Project Structure

```
src/runtime/   — QuickJS runtime wrapper (Runtime class)
src/api/       — Web/system API implementations (console, timers, URL, crypto + crypto.subtle,
                 encoding, fetch, streams, storage, IndexedDB, fs, fs.watch, child_process,
                 WebSocket, EventSource, noise, image, etc.)
src/api/js/    — JS polyfills embedded into C++ at build time (cmake/embed_js.cmake)
tests/         — C++ test harness (tests/main.cpp)
tests/js/      — JavaScript test files (one per API)
third_party/   — QuickJS, libcurl, SQLite, FastNoise2 (bundled); broimage links from ../broimage
```

Optional, both default ON: `BROKIT_ENABLE_NOISE` (FastNoise2 → `BROKIT_HAS_NOISE`) and
`BROKIT_ENABLE_IMAGE` (broimage-backed `bro.image` → `BROKIT_HAS_IMAGE`). The matching
installers are guarded by those `BROKIT_HAS_*` defines in `api.h` and `installAll()`.

## Namespace

- `brokit` — Runtime class
- `brokit::api` — API installer functions

## Key Design Points

- **Consumer provides the DOM** — brokit has no DOM. APIs like TreeWalker operate on JS node objects via standard properties (childNodes, parentNode, nodeType).
- **Pick-and-choose APIs** — consumers can call `installAll(ctx)` or individual `installConsole(ctx)`, `installTimers(ctx)`, etc.
- **No global state** — all state is per-JSContext.
- **No rendering, no windowing** — purely JS runtime + platform APIs.
- **JS polyfills backed by native C++** — complex logic (URL parsing, TreeWalker traversal) is in JS for readability and debuggability. Performance-critical operations (crypto, encoding, image kernels) are native C++.
- **Node-style `require()`** — a synchronous resolver mapping `fs`/`path`/`os`/`child_process` (and `node:` prefixes) to their `__brokit_*` globals. Installed *last* by `installAll()`, after the modules it maps to.

## Adding New APIs

1. Create `src/api/myapi.cpp` implementing `void installMyApi(JSContext* ctx)`
2. Add the declaration to `src/api/api.h`
3. Call it from `installAll()` in `src/api/api.cpp`
4. Add the .cpp to `src/api/CMakeLists.txt`
5. Write `tests/js/test_myapi.js` using the `assert()` and `assertEqual()` test helpers (the harness auto-discovers files in `tests/js/`)
6. Build and run tests

For an optional API that depends on a heavy library, gate it behind a `BROKIT_ENABLE_*`
CMake option that defines a `BROKIT_HAS_*` macro, wrap the installer declaration/call in
`#ifdef BROKIT_HAS_*` (see `installNoise` / `installImage`), and append the `.cpp`
conditionally in `src/api/CMakeLists.txt`. If the API should be reachable via `require()`,
add the name mapping in `installRequire()` in `src/api/api.cpp`.

## Integration with bro

bro's `third_party/CMakeLists.txt` should add brokit similar to htmlayout:

```cmake
set(BROKIT_DIR "${CMAKE_SOURCE_DIR}/../brokit" CACHE PATH "Path to standalone brokit repo")
if(EXISTS "${BROKIT_DIR}/CMakeLists.txt")
    add_subdirectory("${BROKIT_DIR}" "${CMAKE_BINARY_DIR}/brokit" EXCLUDE_FROM_ALL)
endif()
```

Then link with `target_link_libraries(bro_js PUBLIC brokit)` and call `brokit::api::installAll(ctx)` during engine initialization.
