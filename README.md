# brokit

[![CI](https://github.com/wlejon/brokit/actions/workflows/ci.yml/badge.svg)](https://github.com/wlejon/brokit/actions/workflows/ci.yml)
[![CodeQL](https://github.com/wlejon/brokit/actions/workflows/codeql.yml/badge.svg)](https://github.com/wlejon/brokit/actions/workflows/codeql.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

brokit (broke-it) is a standalone C++20 JavaScript runtime library built on [bronze](https://github.com/wlejon/bronze) (the JavaScript compiler and runtime) and [brass](https://github.com/wlejon/brass) (its code generator). Provides web-standard APIs (WinterCG-aligned) and system APIs (Node.js conventions) without owning a DOM or rendering engine.

brokit is one of the engine libraries of the [bro ecosystem](https://github.com/wlejon/bro/blob/main/docs/ecosystem.md): [bro](https://github.com/wlejon/bro) links it in every build profile for its web and system APIs. It builds on [broimage](https://github.com/wlejon/broimage) (and through it [bromath](https://github.com/wlejon/bromath)) for `bro.image`.

**Platforms.** Built and tested on Windows (MSVC), Linux (GCC and Clang) and macOS (arm64); JavaScript runs on x86-64 and AArch64, the targets brass generates code for. The one platform gap is TLS: the bundled libcurl is built with Schannel, Windows' native TLS, and with no TLS backend elsewhere, so `https://` and `wss://` requests work on Windows only. Plain `http://`, `ws://`, `file:`, `data:` and `blob:` URLs work everywhere.

## APIs

### Web platform

| API | Implementation |
|-----|---------------|
| **console** | log/warn/error/debug/info/assert/time/timeEnd/timeLog |
| **timers** | setTimeout, setInterval, clearTimeout, clearInterval, queueMicrotask, performance.now |
| **URL / URLSearchParams** | Full WHATWG URL parsing, resolution, path normalization; URLSearchParams with iterators |
| **URL.createObjectURL** | Blob URL registry with createObjectURL/revokeObjectURL |
| **crypto** | randomUUID (v4), getRandomValues (BCryptGenRandom / /dev/urandom) |
| **crypto.subtle** | SubtleCrypto: digest (SHA-1/256/384/512), sign/verify (HMAC), encrypt/decrypt (AES-GCM, AES-CBC), generateKey, importKey, exportKey. CryptoKey class with usage masks |
| **TextEncoder / TextDecoder** | Native C++ UTF-8 encode/decode |
| **TreeWalker / NodeFilter** | Full DOM traversal with SHOW_* flags and custom filter functions |
| **AbortController / AbortSignal** | abort(), throwIfAborted(), static abort/timeout/any factories, DOMException |
| **structuredClone** | Deep clone with circular reference support |
| **Blob / File** | Native C++ opaque storage; slice(), text(), arrayBuffer(); File adds name/lastModified |
| **FormData** | append/set/get/getAll/has/delete/forEach, iterators, Blob-to-File wrapping, multipart/form-data serialization for fetch |
| **Headers** | Case-insensitive, append/set/get/has/delete/forEach, iterators |
| **Request** | Constructor from URL or Request, method/headers/body, clone() |
| **Response** | Constructor, text/json/arrayBuffer/blob/clone, static error/redirect/json factories |
| **fetch** | Real HTTP via libcurl. True streaming: response.body is a ReadableStream. GET/POST/PUT/PATCH. Headers/Request/Response classes, FormData/Blob/ArrayBuffer/URLSearchParams body. `file:`, `data:` and `blob:` URLs. TLS via Schannel on Windows only (see Platforms) |
| **XMLHttpRequest** | JS implementation over `fetch`, firing `Event`s |
| **FileReader** | readAsText/readAsArrayBuffer/readAsDataURL over Blob/File |
| **CompressionStream / DecompressionStream** | gzip, deflate and deflate-raw via miniz |
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
| **bro.image** | Composable typed-array kernels backed by the broimage sibling. Six verbs — `reduce` (sum/mean/minmax/histogram), `map` (affine/pow/sqrt/log/exp/abs), `combine` (add/sub/mul/min/max/lerp/wsum), `lookup`, `stencil`, `resample` — plus `gradient` and `alloc` builders. Caller-supplied dst buffers; op behavior is a struct, never a JS callback. Mounted as `bro.image.*`. Gated by `BROKIT_ENABLE_IMAGE` (ON by default) |

### System (Node.js-style)

| API | Implementation |
|-----|---------------|
| **fs** | readFileSync/writeFileSync/appendFileSync, stat/lstat, readdirSync (withFileTypes), existsSync, mkdirSync (recursive), rmSync (recursive+force), renameSync, copyFileSync, chmodSync, realpathSync. Async wrappers + fs.promises |
| **fs.watch** | Cross-platform native filesystem watcher (FSWatcher). One OS thread per watcher with a lock-free event ring; backends are ReadDirectoryChangesW (Windows), inotify (Linux), FSEvents (macOS). 'rename'/'change' events, recursive option, overflow surfaced as an error event |
| **child_process** | execSync, exec, execFileSync, execFile, spawnSync. Cross-platform (CreateProcess / fork+exec), stdout/stderr capture, stdin, cwd, timeout |
| **path** | join, resolve, dirname, basename, extname, parse, format, isAbsolute, normalize, sep, delimiter |
| **os** | platform(), type(), arch(), homedir(), tmpdir(), hostname(), EOL |
| **process** | process.env (read/write/delete), process.cwd(), process.exit(), process.platform |
| **net / dgram** | Node-compatible TCP and UDP sockets over nonblocking OS sockets, pumped by the host on the JS thread. Listeners bind 127.0.0.1 unless given a host |
| **WebSocketServer** | RFC 6455 server over `net` |
| **events / util / Buffer** | Node's `EventEmitter`, `util` (promisify, callbackify, inspect hooks, ...) and `Buffer` |
| **require** | Node-style synchronous resolver: the built-in modules `fs`, `path`, `os`, `child_process`, `crypto`, `events`, `util`, `buffer`, `url`, `net`, `dgram` and `websocket-server` (with or without a `node:` prefix), plus relative and absolute file paths. Installed last by `installAll()`, after the modules it maps to |

## Build

Requires CMake 3.24+ and a C++20 compiler (MSVC on Windows, GCC/Clang on Linux and macOS).

```bash
# bronze and brass beside brokit; the submodules supply everything else
git clone https://github.com/wlejon/bronze.git
git clone https://github.com/wlejon/brass.git
git clone --recursive https://github.com/wlejon/brokit.git
cmake -S brokit -B brokit/build
cmake --build brokit/build --config Release    # or Debug
```

Dependencies:
- **bronze** — JavaScript compiler & runtime, at `../bronze` (or `-DBRONZE_DIR=<path>`). No submodule: the APIs are bound across bronze's C++ embed boundary, so brokit has to compile against the same bronze as the program that loads it.
- **brass** — bronze's code generator, at `../brass` beside bronze.
- **libcurl** — HTTP/WebSocket (bundled git submodule, static, Schannel TLS on Windows)
- **SQLite** — IndexedDB persistence (bundled amalgamation)
- **miniz** — CompressionStream codecs (bundled)
- **FastNoise2** — SIMD noise generation (bundled git submodule, gated by `BROKIT_ENABLE_NOISE`)
- **broimage** and **bromath** — typed-array image kernels backing `bro.image` (gated by `BROKIT_ENABLE_IMAGE`). They resolve the way every repo in the ecosystem resolves a sibling: an existing target wins (bro adds both first), then a checkout beside the top-level project (`../broimage`, `../bromath`; override with `-DBROIMAGE_DIR` / `-DBROMATH_DIR`), then the `third_party/` submodules, which carry both. broimage's brotensor adapter is off by default here (`BROIMAGE_WITH_TENSOR`), since brokit uses only its host-pointer kernels.

Optional features are gated by CMake options (both default ON): `BROKIT_ENABLE_NOISE` compiles `noise.cpp` and defines `BROKIT_HAS_NOISE`; `BROKIT_ENABLE_IMAGE` compiles `image.cpp`, links broimage, and defines `BROKIT_HAS_IMAGE`.

## Test

One JavaScript test file per API under `tests/js/`. The C++ harness (`tests/main.cpp`, `brokit_test`) compiles and runs a test file, pumps the async subsystems (timers, fetch, sockets), and tallies pass/fail. ctest registers every file twice: once normally and once under GC stress (a collection at every allocation, freed space poisoned), which catches a binding holding a raw value across an allocating call. The two network fetch suites are not registered, so ctest never depends on the internet.

```bash
ctest --test-dir build -C Release --output-on-failure    # -LE gcstress skips the stress runs
./build/tests/Release/brokit_test.exe tests/js/test_url.js   # one file (Windows path)
```

CI runs the suite on Linux (GCC and Clang), Windows (MSVC) and macOS/arm64 against broimage and bromath from main, builds once more from the `third_party/` submodules alone (the fresh-clone path), and reports coverage of `src/`.

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
brokit::api::installAll();  // install all APIs into the runtime
rt.eval("fetch('https://example.com').then(r => r.text()).then(console.log)", "<main>");
rt.executePendingJobs();
```

Individual APIs can be installed selectively:

```cpp
brokit::api::installConsole();
brokit::api::installFetch();
brokit::api::installFS();
```

## Architecture

```
src/runtime/   Runtime class: Bronze module compiler, dlopen module loading, microtask pump
src/api/       Modular API installers (one .cpp per API, HostClass / HostProxy / ObjectBuilder)
src/api/js/    JS polyfills compiled ahead of time with Bronze (bronze_compile_js)
tests/         C++ test harness (tests/main.cpp) that builds & runs JS test files and pumps async subsystems
tests/js/      JavaScript test files (one per API)
third_party/   libcurl, FastNoise2, broimage, bromath (submodules); SQLite, miniz (vendored)
```

**Design principles:**
- No DOM, no rendering, no windowing — purely JS runtime + platform APIs
- Each API is independently installable (`installAll()` or pick-and-choose)
- The JS polyfills (`src/api/js/`) are compiled ahead of time by bronze into native object files
- JS polyfills for complex logic, native C++ for performance-critical ops
- Static CRT on Windows (runs in Windows Sandbox without vcruntime DLLs)

## License

[MIT](LICENSE)
