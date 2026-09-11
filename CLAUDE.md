# CLAUDE.md

## What This Project Is

brokit is a standalone C++20 library providing a JavaScript runtime with web-standard and system APIs, built on Bronze (AOT compiler and runtime) and Brass. It is consumed by other projects in the `bro` ecosystem as a dependency. It does NOT own a DOM, render anything, or manage windows.

The API surface borrows from WinterCG (web-standard APIs) and Node.js (system APIs). Each API is independently installable into Bronze's embed runtime.

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
src/runtime/   — Bronze runtime wrapper (Runtime class: AOT compilation, dlopen module loading)
src/api/       — Web/system API implementations (console, timers, URL, crypto + crypto.subtle,
                 encoding, fetch, streams, storage, IndexedDB, fs, fs.watch, child_process,
                 WebSocket, EventSource, noise, image, etc.)
src/api/js/    — JS polyfills compiled AOT into object files via bronze_compile_js
tests/         — C++ test harness (tests/main.cpp)
tests/js/      — JavaScript test files (one per API, 56 suites)
third_party/   — libcurl, SQLite, FastNoise2 (bundled); bronze (at ../bronze), brass (at ../brass),
                 broimage links from ../broimage
```

Optional, both default ON: `BROKIT_ENABLE_NOISE` (FastNoise2 → `BROKIT_HAS_NOISE`) and
`BROKIT_ENABLE_IMAGE` (broimage-backed `bro.image` → `BROKIT_HAS_IMAGE`). The matching
installers are guarded by those `BROKIT_HAS_*` defines in `api.h` and `installAll()`.

## Namespace

- `brokit` — Runtime class
- `brokit::api` — API installer functions, `HostClass`, `HostProxy`, `ObjectBuilder`

## Key Design Points

- **Consumer provides the DOM** — brokit has no DOM. APIs like TreeWalker operate on JS node objects via standard properties (childNodes, parentNode, nodeType).
- **Pick-and-choose APIs** — consumers can call `installAll()` or individual `installConsole()`, `installTimers()`, etc.
- **Bronze AOT Compilation** — all JS polyfills (32 files) are compiled ahead of time with Bronze into native object files and linked directly into `libbrokit_api.a`. Dynamic JS is compiled to shared libraries and loaded via `bronze::embed::runEntry`.
- **Host Binding Infrastructure** — uses `bronze::embed` primitives with `HostClass` (prototype/constructor registration, native handle management), `HostProxy` (dynamic traps), and `ObjectBuilder`.
- **No rendering, no windowing** — purely JS runtime + platform APIs.
- **Node-style `require()`** — a synchronous resolver mapping registered modules and filesystem paths (`./...`, `../...`, `/...`). Hierarchical directory tracking via `RequireDirGuard`.

## Adding New APIs

1. Create `src/api/myapi.cpp` implementing `void installMyApi()`
2. Add the declaration to `src/api/api.h`
3. Call it from `installAll()` in `src/api/api.cpp`
4. Add the .cpp to `src/api/CMakeLists.txt`
5. If the API has a JS layer, add `src/api/js/myapi.js` and list it in `src/api/CMakeLists.txt` under `BROKIT_JS_POLYFILLS`
6. Write `tests/js/test_myapi.js` using the `assert()` and `assertEqual()` test helpers
7. Build and run tests

## Integration with bro

bro's `third_party/CMakeLists.txt` adds brokit:

```cmake
set(BROKIT_DIR "${CMAKE_SOURCE_DIR}/../brokit" CACHE PATH "Path to standalone brokit repo")
if(EXISTS "${BROKIT_DIR}/CMakeLists.txt")
    add_subdirectory("${BROKIT_DIR}" "${CMAKE_BINARY_DIR}/brokit" EXCLUDE_FROM_ALL)
endif()
```

Then link with `target_link_libraries(bro_js PUBLIC brokit)` and call `brokit::api::installAll()` during engine initialization.
