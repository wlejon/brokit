#include "api/fs_watch.h"

#include "api/api.h"
#include "runtime/runtime.h"
#include "fs_watch.js.h"

#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace brokit::api {

std::atomic<int> FsWatcher::s_nextId_{1};

// ---------------------------------------------------------------------------
// FsWatcher: generic (queue + lifecycle). Platform members live in the
// per-OS fs_watch_*.cpp files.
// ---------------------------------------------------------------------------

FsWatcher::FsWatcher(int id, std::string path, bool recursive)
    : id_(id), path_(std::move(path)), recursive_(recursive),
      ring_(kCap)
{
}

FsWatcher::~FsWatcher()
{
    stopNative();
}

void FsWatcher::pushEvent(EventType type, std::string filename)
{
    size_t t = tail_.load(std::memory_order_relaxed);
    size_t n = (t + 1) % kCap;
    if (n == head_.load(std::memory_order_acquire)) {
        // Ring full. Mark overflow; drop the event. Drain will surface this
        // as a synthetic Error event so the JS side knows it missed something.
        overflow_.store(true, std::memory_order_release);
        return;
    }
    ring_[t].type     = type;
    ring_[t].filename = std::move(filename);
    tail_.store(n, std::memory_order_release);
}

void FsWatcher::drain(std::vector<Event>& out)
{
    for (;;) {
        size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) break;
        out.push_back(std::move(ring_[h]));
        ring_[h].filename.clear();
        head_.store((h + 1) % kCap, std::memory_order_release);
    }
    if (overflow_.exchange(false, std::memory_order_acq_rel)) {
        out.push_back({EventType::Error, "fs.watch ring overflow — events dropped"});
    }
}

std::unique_ptr<FsWatcher> FsWatcher::create(const std::string& absPath,
                                             bool recursive,
                                             std::string* errOut)
{
    int id = s_nextId_.fetch_add(1, std::memory_order_relaxed);
    auto w = std::unique_ptr<FsWatcher>(new FsWatcher(id, absPath, recursive));
    if (!w->startNative(absPath, recursive)) {
        if (errOut) *errOut = w->errMsg_;
        return nullptr;
    }
    return w;
}

// ---------------------------------------------------------------------------
// JS bindings.
// ---------------------------------------------------------------------------

// Per-context registry: id -> FsWatcher. Lives in C++ statics keyed by JSContext.
// Using a plain unordered_map is safe — JS is single-threaded and FsWatcher
// owns its background thread internally.
namespace {

struct CtxState {
    std::unordered_map<int, std::unique_ptr<FsWatcher>> watchers;
};

static std::unordered_map<JSContext*, CtxState> g_state;

CtxState& stateOf(JSContext* ctx) { return g_state[ctx]; }

// Reuse fs.cpp's path resolver via the same global key. We re-implement it
// here (rather than exposing a header symbol) because resolveFsPath is static
// in fs.cpp; copying ~30 lines is cheaper than refactoring its visibility.
static const char* kFsBasePathsKey = "__brokit_fs_base_paths";

std::string resolvePath(JSContext* ctx, const char* path)
{
    fs::path p(path);
    if (p.is_absolute()) return path;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue arr    = JS_GetPropertyStr(ctx, global, kFsBasePathsKey);
    std::string out = path;
    if (JS_IsArray(arr)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (int32_t i = len - 1; i >= 0; --i) {
            JSValue elem = JS_GetPropertyUint32(ctx, arr, i);
            const char* base = JS_ToCString(ctx, elem);
            JS_FreeValue(ctx, elem);
            if (!base) continue;
            fs::path candidate = fs::path(base) / path;
            JS_FreeCString(ctx, base);
            std::error_code ec;
            if (fs::exists(candidate, ec)) { out = candidate.string(); break; }
        }
    }
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, global);
    return out;
}

const char* eventName(FsWatcher::EventType t)
{
    switch (t) {
        case FsWatcher::EventType::Change: return "change";
        case FsWatcher::EventType::Rename: return "rename";
        case FsWatcher::EventType::Error:  return "error";
    }
    return "change";
}

} // namespace

// __brokit_fs_watch_create(path, recursive) -> id (int) | throws
static JSValue js_fs_watch_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "fs.watch: path required");

    const char* rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) return JS_EXCEPTION;
    std::string resolved = resolvePath(ctx, rawPath);
    JS_FreeCString(ctx, rawPath);

    bool recursive = false;
    if (argc >= 2) recursive = JS_ToBool(ctx, argv[1]);

    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(resolved, ec);
    std::string absPath = ec ? resolved : canonical.string();

    if (!fs::exists(absPath)) {
        return JS_ThrowReferenceError(
            ctx, "fs.watch: path does not exist: %s", absPath.c_str());
    }

    std::string err;
    auto w = FsWatcher::create(absPath, recursive, &err);
    if (!w) {
        return JS_ThrowInternalError(
            ctx, "fs.watch: %s", err.empty() ? "failed to start watcher" : err.c_str());
    }

    int id = w->id();
    stateOf(ctx).watchers[id] = std::move(w);
    return JS_NewInt32(ctx, id);
}

// __brokit_fs_watch_close(id)
static JSValue js_fs_watch_close(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_UNDEFINED;
    int id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    auto& s = stateOf(ctx);
    auto it = s.watchers.find(id);
    if (it != s.watchers.end()) {
        // Destructor joins the watcher thread.
        s.watchers.erase(it);
    }
    return JS_UNDEFINED;
}

// __brokit_fs_watch_tick() — drain every watcher, fire JS callbacks.
// JS side installs `__brokit_fs_watch_dispatch(id, type, filename)` which
// looks up the FSWatcher object and emits the appropriate event.
static JSValue js_fs_watch_tick(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    auto stateIt = g_state.find(ctx);
    if (stateIt == g_state.end()) return JS_UNDEFINED;

    JSValue global   = JS_GetGlobalObject(ctx);
    JSValue dispatch = JS_GetPropertyStr(ctx, global, "__brokit_fs_watch_dispatch");
    bool haveDispatch = JS_IsFunction(ctx, dispatch);

    // Drain everything into a flat (id, event) list first, then dispatch.
    // A JS callback calling watcher.close() during dispatch will erase the
    // watcher from this context's map; iterating that map directly would be
    // a use-after-free. The JS-side dispatcher silently drops events for
    // closed watchers (matches Node's fs.watch contract).
    struct Pending { int id; FsWatcher::Event ev; };
    std::vector<Pending> pending;
    {
        std::vector<FsWatcher::Event> buf;
        for (auto& [id, w] : stateIt->second.watchers) {
            buf.clear();
            w->drain(buf);
            for (auto& ev : buf) pending.push_back({id, std::move(ev)});
        }
    }

    if (haveDispatch) {
        for (auto& p : pending) {
            JSValue args[3] = {
                JS_NewInt32(ctx, p.id),
                JS_NewString(ctx, eventName(p.ev.type)),
                JS_NewString(ctx, p.ev.filename.c_str()),
            };
            JSValue ret = JS_Call(ctx, dispatch, JS_UNDEFINED, 3, args);
            JS_FreeValue(ctx, args[0]);
            JS_FreeValue(ctx, args[1]);
            JS_FreeValue(ctx, args[2]);
            if (JS_IsException(ret)) {
                Runtime::checkException(ctx, ret);
            } else {
                JS_FreeValue(ctx, ret);
            }
        }
    }

    JS_FreeValue(ctx, dispatch);
    JS_FreeValue(ctx, global);
    return JS_UNDEFINED;
}

// __brokit_fs_watch_has_pending() -> bool. Kept for symmetry with fetch / ws
// and so test harnesses can decide when to stop pumping.
static JSValue js_fs_watch_has_pending(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    auto stateIt = g_state.find(ctx);
    if (stateIt == g_state.end()) return JS_NewBool(ctx, false);
    return JS_NewBool(ctx, !stateIt->second.watchers.empty());
}

void installFSWatch(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__brokit_fs_watch_create",
        JS_NewCFunction(ctx, js_fs_watch_create, "__brokit_fs_watch_create", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_fs_watch_close",
        JS_NewCFunction(ctx, js_fs_watch_close, "__brokit_fs_watch_close", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_fs_watch_tick",
        JS_NewCFunction(ctx, js_fs_watch_tick, "__brokit_fs_watch_tick", 0));
    JS_SetPropertyStr(ctx, global, "__brokit_fs_watch_has_pending",
        JS_NewCFunction(ctx, js_fs_watch_has_pending, "__brokit_fs_watch_has_pending", 0));
    JS_FreeValue(ctx, global);

    // Install the JS facade (FSWatcher class + fs.watch wiring).
    JSValue r = JS_Eval(ctx, js_fs_watch, strlen(js_fs_watch),
                        "<fs_watch>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) Runtime::checkException(ctx, r);
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
