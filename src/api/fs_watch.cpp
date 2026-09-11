#include "api/fs_watch.h"
#include "api/api.h"
#include "api/arg_reader.h"
#include "api/object_builder.h"

#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

extern "C" void bronze_fs_watch_main();

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

namespace {

struct CtxState {
    std::unordered_map<int, std::unique_ptr<FsWatcher>> watchers;
};

thread_local CtxState g_state;

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
static bronze::Value js_fs_watch_create(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("fs.watch: path required");

    std::string rawPath = ev::toUtf8(a[0]);
    std::string resolved = resolveAssetPath(rawPath);

    bool recursive = a.size() >= 2 && ev::isBool(a[1]) && ev::toBool(a[1]);

    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(resolved, ec);
    std::string absPath = ec ? resolved : canonical.string();

    if (!fs::exists(absPath)) {
        return ev::throwTypeError(("fs.watch: path does not exist: " + absPath).c_str());
    }

    std::string err;
    auto w = FsWatcher::create(absPath, recursive, &err);
    if (!w) {
        return ev::throwTypeError(
            ("fs.watch: " + (err.empty() ? "failed to start watcher" : err)).c_str());
    }

    int id = w->id();
    g_state.watchers[id] = std::move(w);
    return ev::fromDouble(id);
}

// __brokit_fs_watch_close(id)
static bronze::Value js_fs_watch_close(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::undefined();
    int id = i32At(a, 0);
    g_state.watchers.erase(id);
    return ev::undefined();
}

// __brokit_fs_watch_tick() — drain every watcher, fire JS callbacks.
static bronze::Value js_fs_watch_tick(bronze::Value, std::span<const bronze::Value>)
{
    bronze::Value dispatch = ev::getGlobal("__brokit_fs_watch_dispatch");
    if (!ev::isFunction(dispatch)) return ev::undefined();

    struct Pending { int id; FsWatcher::Event ev; };
    std::vector<Pending> pending;
    {
        std::vector<FsWatcher::Event> buf;
        for (auto& [id, w] : g_state.watchers) {
            buf.clear();
            w->drain(buf);
            for (auto& ev : buf) pending.push_back({id, std::move(ev)});
        }
    }

    ev::Persistent dispP{dispatch};
    for (auto& p : pending) {
        ev::Persistent idVal{ev::fromDouble(p.id)};
        ev::Persistent typeVal{ev::fromUtf8(eventName(p.ev.type))};
        ev::Persistent fileVal{ev::fromUtf8(p.ev.filename)};
        bronze::Value args[3] = { idVal.get(), typeVal.get(), fileVal.get() };
        ev::call(dispP.get(), ev::undefined(), std::span<const bronze::Value>(args, 3));
    }

    return ev::undefined();
}

// __brokit_fs_watch_has_pending() -> bool.
static bronze::Value js_fs_watch_has_pending(bronze::Value, std::span<const bronze::Value>)
{
    return ev::fromBool(!g_state.watchers.empty());
}

void installFSWatch()
{
    ev::setGlobalFunction("__brokit_fs_watch_create", 2, js_fs_watch_create);
    ev::setGlobalFunction("__brokit_fs_watch_close", 1, js_fs_watch_close);
    ev::setGlobalFunction("__brokit_fs_watch_tick", 0, js_fs_watch_tick);
    ev::setGlobalFunction("__brokit_fs_watch_has_pending", 0, js_fs_watch_has_pending);

    bronze::embed::runEntry(bronze_fs_watch_main);
}

} // namespace brokit::api
