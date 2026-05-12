# brokit

brokit (broke-it) is a standalone C++20 JavaScript runtime library built on [QuickJS](https://bellard.org/quickjs/). Provides web-standard APIs (WinterCG-aligned) and system APIs (Node.js conventions) without owning a DOM or rendering engine.

Built as a dependency for [bro](https://github.com/wlejon/bro).

## APIs

### Web platform

| API | Implementation |
|-----|---------------|
| **console** | log/warn/error/debug/info/assert/time/timeEnd/timeLog |
| **timers** | setTimeout, setInterval, clearTimeout, clearInterval, queueMicrotask, performance.now |
| **URL / URLSearchParams** | Full WHATWG URL parsing, resolution, path normalization; URLSearchParams with iterators |
| **URL.createObjectURL** | Blob URL registry with createObjectURL/revokeObjectURL |
| **crypto** | randomUUID (v4), getRandomValues (BCryptGenRandom / /dev/urandom) |
| **TextEncoder / TextDecoder** | Native C++ UTF-8 encode/decode |
| **TreeWalker / NodeFilter** | Full DOM traversal with SHOW_* flags and custom filter functions |
| **AbortController / AbortSignal** | abort(), throwIfAborted(), static abort/timeout/any factories, DOMException |
| **structuredClone** | Deep clone with circular reference support |
| **Blob / File** | Native C++ opaque storage; slice(), text(), arrayBuffer(); File adds name/lastModified |
| **FormData** | append/set/get/getAll/has/delete/forEach, iterators, Blob-to-File wrapping, multipart/form-data serialization for fetch |
| **Headers** | Case-insensitive, append/set/get/has/delete/forEach, iterators |
| **Request** | Constructor from URL or Request, method/headers/body, clone() |
| **Response** | Constructor, text/json/arrayBuffer/blob/clone, static error/redirect/json factories |
| **fetch** | Real HTTP via libcurl. True streaming: response.body is a ReadableStream. GET/POST/PUT/PATCH. Headers/Request/Response classes, FormData/Blob/ArrayBuffer/URLSearchParams body. Native TLS (Schannel on Windows) |
| **ReadableStream** | Full spec: underlying source, reader, controller, tee(), pipeThrough(), pipeTo(), async iteration, ReadableStream.from(), TextDecoderStream |
| **WritableStream** | Full spec: underlying sink, writer, queue-based write processing |
| **TransformStream** | Custom transform/flush/start, identity default, TextEncoderStream |
| **EventSource (SSE)** | Full wire protocol with auto-reconnect and Last-Event-ID |
| **WebSocket** | Native curl WebSocket, text + binary frames, auto-pong |
| **localStorage / sessionStorage** | localStorage with JSON file persistence, sessionStorage in-memory |
| **IndexedDB** | SQLite-backed. open/deleteDatabase, transactions, object stores with put/add/get/delete/clear/getAll/count, version upgrades |
| **atob / btoa** | Base64 encode/decode with full Latin1 support |
| **Event / CustomEvent** | Event constructor with bubbles/cancelable/composed, CustomEvent with detail |
| **EventTarget** | addEventListener/removeEventListener/dispatchEvent, once option, handleEvent interface |
| **MessageChannel / MessagePort** | Paired ports with postMessage, start/close, message queuing, MessageEvent |
| **navigator** | userAgent, language, languages, onLine, platform, hardwareConcurrency |

### Procedural generation

| API | Implementation |
|-----|---------------|
| **FastNoise** | SIMD-accelerated noise via FastNoise2. `FastNoise.create(type)` for ~50 node types, `node.set(name, value)` metadata-driven config, `genUniformGrid2D/3D` (+ `Into` variants for zero-alloc reuse), `genSingle2D/3D`, `genTileable2D`. Generators, fractals, cellular, domain warp, operators (Add/Multiply/Min/Max/Fade), modifiers (Remap/Terrace/DomainScale). Gated by `BROKIT_ENABLE_NOISE` (ON by default) |

### System (Node.js-style)

| API | Implementation |
|-----|---------------|
| **fs** | readFileSync/writeFileSync/appendFileSync, stat/lstat, readdirSync (withFileTypes), existsSync, mkdirSync (recursive), rmSync (recursive+force), renameSync, copyFileSync, chmodSync, realpathSync. Async wrappers + fs.promises |
| **child_process** | execSync, exec, execFileSync, execFile, spawnSync. Cross-platform (CreateProcess / fork+exec), stdout/stderr capture, stdin, cwd, timeout |
| **path** | join, resolve, dirname, basename, extname, parse, format, isAbsolute, normalize, sep, delimiter |
| **os** | platform(), type(), arch(), homedir(), tmpdir(), hostname(), EOL |
| **process** | process.env (read/write/delete), process.cwd(), process.exit(), process.platform |

## Build

Requires CMake 3.24+ and a C++20 compiler (MSVC on Windows, GCC/Clang on Linux).

```bash
cmake -B build
cmake --build build --config Debug    # or Release
```

Dependencies (all bundled):
- **QuickJS** — JavaScript engine (git submodule)
- **libcurl 8.19.0** — HTTP/WebSocket (git submodule, static, Schannel TLS on Windows)
- **SQLite** — IndexedDB persistence (amalgamation)
- **FastNoise2** — SIMD noise generation (git submodule, gated by `BROKIT_ENABLE_NOISE`)

## Test

2307 tests across 29 test files.

```bash
# Windows (MSVC)
./build/tests/Debug/brokit_test.exe tests/js

# Linux
./build/tests/brokit_test tests/js
```

## Usage

### As a library dependency

```cmake
add_subdirectory(path/to/brokit)
target_link_libraries(my_target PUBLIC brokit)
```

### In code

```cpp
#include "runtime/runtime.h"
#include "api/api.h"

brokit::Runtime rt;
brokit::api::installAll(rt.context());  // install all APIs
rt.eval("fetch('https://example.com').then(r => r.text()).then(console.log)", "<main>");
rt.executePendingJobs();
```

Individual APIs can be installed selectively:

```cpp
brokit::api::installConsole(rt.context());
brokit::api::installFetch(rt.context());
brokit::api::installFS(rt.context());
```

## Architecture

```
src/runtime/   Runtime class: QuickJS wrapper, ES module loader, exception handling
src/api/       Modular API installers (one .cpp per API, optional .js polyfill)
src/api/js/    JS polyfills embedded into C++ at build time via cmake/embed_js.cmake
tests/         C++ test harness that evals JS test files and pumps async subsystems
third_party/   QuickJS, libcurl, SQLite, FastNoise2
```

**Design principles:**
- No DOM, no rendering, no windowing — purely JS runtime + platform APIs
- Each API is independently installable (`installAll()` or pick-and-choose)
- All state is per-JSContext, no globals
- JS polyfills for complex logic, native C++ for performance-critical ops
- Static CRT on Windows (runs in Windows Sandbox without vcruntime DLLs)

## License

[MIT](LICENSE)
