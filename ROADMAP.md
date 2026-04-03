# brokit Roadmap

## Goal

brokit is bro's standalone JavaScript runtime library. It provides web-standard and system APIs on top of QuickJS so that bro (and future apps) get a comprehensive platform layer without coupling to the DOM or rendering engine.

The API surface borrows from established standards:
- **WinterCG** (Web-interoperable Runtimes) for web APIs — the same subset that Deno, Bun, and Cloudflare Workers align on
- **Node.js** for system APIs — fs, child_process, path, os

The driving use case is running complex web applications inside bro — specifically, getting pi-mono's web-ui (Lit-based AI chat interface) functional, and eventually building a custom coding agent UI within bro's rendering system.

## Current State (v0.5)

All implemented, all tests passing (765/765):

| API | Notes |
|-----|-------|
| Runtime | QuickJS wrapper, ES module loader, exception handling, configurable logging |
| console | log/warn/error/debug/info/assert/time/timeEnd/timeLog |
| timers | setTimeout, setInterval, clearTimeout, clearInterval, queueMicrotask, performance.now |
| URL | Full WHATWG URL constructor with parsing, resolution, path normalization; URLSearchParams with iterators |
| URL.createObjectURL | Blob URL registry with createObjectURL/revokeObjectURL |
| crypto | randomUUID (v4), getRandomValues (BCryptGenRandom on Windows, /dev/urandom on Linux) |
| TextEncoder/TextDecoder | Native C++ UTF-8 encode/decode |
| TreeWalker/NodeFilter | Full traversal (nextNode, previousNode, firstChild, lastChild, nextSibling, previousSibling), SHOW_* flags, custom filter functions |
| AbortController/AbortSignal | abort(), addEventListener, throwIfAborted(), static abort/timeout/any factories, DOMException |
| structuredClone | Deep clone of primitives, objects, arrays, Date, RegExp, Map, Set, ArrayBuffer, TypedArrays, Error; circular ref support; throws on functions/symbols |
| Blob/File | Native C++ opaque storage; Blob constructor from strings/ArrayBuffers/TypedArrays/Blobs; size/type getters, slice(), text(), arrayBuffer() (Promise-returning); File adds name/lastModified |
| fetch | Real HTTP via libcurl (curl 8.19.0). Non-blocking curl_multi integration. **True streaming**: Promise resolves after headers, response.body is ReadableStream with incremental chunk delivery. GET/POST/PUT/PATCH, custom headers, request body. Windows native TLS (Schannel) |
| ReadableStream | Full implementation: constructor with underlying source (start/pull/cancel), ReadableStreamDefaultReader, Controller, tee(), pipeThrough(), pipeTo(), Symbol.asyncIterator, ReadableStream.from(), TextDecoderStream |
| process | process.env (Proxy-based read/write/delete), process.cwd(), process.exit(), process.platform |
| os | platform(), type(), arch(), homedir(), tmpdir(), hostname(), EOL |
| path | join, resolve, dirname, basename, extname, parse, format, isAbsolute, normalize, sep, delimiter |
| fs | readFileSync/writeFileSync/appendFileSync (string + Uint8Array), statSync/lstatSync (isFile/isDirectory/isSymbolicLink), readdirSync (withFileTypes), existsSync, mkdirSync (recursive), rmSync (recursive+force), rmdirSync, unlinkSync, renameSync, copyFileSync, chmodSync, realpathSync. Async callback+Promise wrappers, fs.promises namespace. Node.js-style error codes (ENOENT, EACCES, etc.) |
| child_process | execSync, exec (callback+Promise), execFileSync, execFile, spawnSync. Cross-platform (CreateProcess/fork+exec), stdout/stderr capture, stdin input, cwd, timeout, maxBuffer, encoding options. Node.js-style error properties |
| WritableStream | Full constructor with underlying sink (start/write/close/abort), WritableStreamDefaultWriter, queue-based write processing |
| TransformStream | Custom transform/flush/start, identity default, TransformStreamDefaultController with enqueue/error/terminate |
| TextEncoderStream | String-to-Uint8Array transform via TransformStream |
| EventSource (SSE) | Full SSE wire protocol (data/event/id/retry, comments, multi-line data), auto-reconnect with Last-Event-ID |
| WebSocket | Native curl WebSocket (CONNECT_ONLY + curl_ws_recv/send), JS class with open/message/error/close events, text + binary frames, auto-pong |
| localStorage / sessionStorage | localStorage with JSON file persistence, sessionStorage in-memory only. Standard Web Storage API (getItem/setItem/removeItem/clear/key/length) |
| IndexedDB | SQLite-backed (amalgamation). IDBFactory.open/deleteDatabase, IDBDatabase, IDBTransaction, IDBObjectStore (put/add/get/delete/clear/getAll/getAllKeys/count), version upgrades |
| FormData | JS polyfill. append/set/get/getAll/has/delete/forEach, entries/keys/values iterators, Blob→File wrapping with filename |

Infrastructure:
- CMake build matching bro/htmlayout patterns
- Static CRT on Windows (runs in Windows Sandbox without vcruntime DLLs)
- JS test harness with assert/assertEqual helpers
- sandbox.wsb for safe testing of native code
- JS polyfills in standalone `src/api/js/*.js` files, embedded into C++ headers at build time via `cmake/embed_js.cmake`
- libcurl 8.19.0 as git submodule (static, Schannel TLS, minimal protocol set)

## Tier 1 — Unblock Lit and framework rendering

These APIs are required for Lit (the web component framework pi-mono uses) and most modern JS frameworks to function. They are the minimum viable additions to get real apps rendering in bro.

| API | Why | Complexity |
|-----|-----|-----------|
| ~~**Blob / File**~~ | ~~File handling, image previews, clipboard, download URLs~~ | ~~Medium~~ — **done (v0.2)** |
| ~~**URL.createObjectURL / revokeObjectURL**~~ | ~~Blob URLs for images, downloads, iframes~~ | ~~Medium~~ — **done (v0.3)** |
| ~~**ResizeObserver**~~ | ~~Responsive layouts — ChatPanel uses it for breakpoints~~ | ~~Small~~ — **done (v0.3, in bro)** — DOM-dependent, lives in bro's dom_bindings |
| ~~**AbortController / AbortSignal**~~ | ~~Fetch cancellation, cleanup patterns — used everywhere~~ | ~~Small~~ — **done (v0.2)** |
| ~~**structuredClone**~~ | ~~Deep copy of objects — used by state management~~ | ~~Small~~ — **done (v0.2)** |

## Tier 2 — Network and streaming

Real HTTP fetch is the single biggest unlock. Without it, no app can talk to an API. This turns bro from a local renderer into a connected application runtime.

| API | Why | Complexity |
|-----|-----|-----------|
| ~~**fetch (real HTTP)**~~ | ~~REST APIs, LLM provider calls, asset loading~~ | ~~Large~~ — **done (v0.3)** via libcurl |
| ~~**ReadableStream**~~ | ~~Streaming responses (SSE, LLM token streaming)~~ | ~~Large~~ — **done (v0.4)** with true streaming fetch |
| ~~**WritableStream / TransformStream**~~ | ~~Stream pipeline composition~~ | ~~Medium~~ — **done (v0.5)** with TextEncoderStream |
| ~~**EventSource (SSE)**~~ | ~~Server-Sent Events — common LLM streaming pattern~~ | ~~Medium~~ — **done (v0.5)** JS polyfill on fetch + ReadableStream |
| ~~**WebSocket**~~ | ~~Bidirectional real-time communication~~ | ~~Medium~~ — **done (v0.5)** native curl WS + JS class |

## Tier 3 — Persistent storage

Apps need to persist data. IndexedDB is the standard, but we can start simpler.

| API | Why | Complexity |
|-----|-----|-----------|
| ~~**localStorage / sessionStorage**~~ | ~~Already in bro — move to brokit for standalone use~~ | ~~Small~~ — **done (v0.5)** ported from bro, JSON file persistence |
| ~~**IndexedDB**~~ | ~~pi-mono's session/settings storage, any data-heavy app~~ | ~~Large~~ — **done (v0.5)** backed by SQLite amalgamation |

## Tier 4 — System access

These make bro a real application runtime, not just a browser. They're what would enable a custom coding agent.

| API | Why | Complexity |
|-----|-----|-----------|
| ~~**fs (read/write/stat/readdir)**~~ | ~~File system access — coding agent tools, app data~~ | ~~Medium~~ — **done (v0.4)** |
| ~~**child_process (exec/spawn)**~~ | ~~Run shell commands — coding agent bash tool~~ | ~~Medium~~ — **done (v0.4)** |
| ~~**path**~~ | ~~Cross-platform path manipulation~~ | ~~Small~~ — **done (v0.3)** |
| ~~**os**~~ | ~~Platform info, homedir, tmpdir~~ | ~~Small~~ — **done (v0.3)** |
| ~~**process.env**~~ | ~~Environment variables — API keys, config~~ | ~~Small~~ — **done (v0.3)** |

## Tier 5 — Advanced browser APIs

Nice-to-haves that improve compatibility with the broader web ecosystem.

| API | Why | Complexity |
|-----|-----|-----------|
| **MutationObserver** (full) | bro has a stub — frameworks need real mutation callbacks | Medium |
| **IntersectionObserver** | Lazy loading, infinite scroll | Medium |
| ~~**FormData**~~ | ~~Form submission, file uploads~~ | ~~Small~~ — **done (v0.5)** JS polyfill |
| **DOMParser** | Parse HTML/XML strings into documents | Medium |
| **Worker / SharedWorker** | Background computation, multi-threading | Very large |

## Integration Path

When brokit has enough APIs, bro integrates it:

1. Add brokit to bro's `third_party/CMakeLists.txt` (same pattern as htmlayout)
2. Link `bro_js` against `brokit`
3. Call `brokit::api::installAll(ctx)` during engine initialization
4. Remove duplicate implementations from bro (fetch polyfill, storage, window bindings overlap)
5. bro's DOM bindings call `__brokit_install_createTreeWalker(document)` to wire TreeWalker to the real document

## Non-goals

- **DOM implementation** — stays in bro. brokit has no DOM.
- **Rendering** — no Skia, no SDL, no GPU. Purely JS runtime + platform APIs.
- **Terminal UI** — we won't replicate pi's TUI. The agent UI will be built in bro's HTML/CSS rendering.
- **Full Node.js compatibility** — we borrow conventions, not the entire API surface.
