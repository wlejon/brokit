#pragma once

// Native filesystem watcher.
//
// One OS thread per watcher. That thread owns the platform handle (inotify fd
// / FSEventStream / ReadDirectoryChangesW IOCP) and is the sole producer for
// this watcher's event queue. The JS thread is the sole consumer and drains
// via `drain()` from the per-tick pump. No shared mutable state crosses
// threads except a lock-free SPSC ring and a few atomics — no std::mutex
// anywhere, per the project's no-mutex rule.
//
// Backends:
//   Linux   — inotify + epoll on (inotify_fd, eventfd) for wakeup
//   macOS   — FSEvents on a private CFRunLoop, woken by CFRunLoopStop
//   Windows — ReadDirectoryChangesW + IOCP, woken by PostQueuedCompletionStatus
//
// Recursion semantics match Node's fs.watch:
//   recursive=false → events report the basename
//   recursive=true  → events report a forward-slash path relative to the
//                     watched root

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace brokit::api {

class FsWatcher {
public:
    enum class EventType { Change, Rename, Error };

    struct Event {
        EventType   type;
        std::string filename; // Or error message when type == Error
    };

    // Factory. Returns nullptr on failure; *errOut receives the reason.
    static std::unique_ptr<FsWatcher> create(const std::string& absPath,
                                             bool recursive,
                                             std::string* errOut);

    ~FsWatcher();

    FsWatcher(const FsWatcher&)            = delete;
    FsWatcher& operator=(const FsWatcher&) = delete;

    // Pop everything currently in the ring into `out`. Called only from the
    // JS thread. Appends; does not clear `out` first.
    void drain(std::vector<Event>& out);

    int  id()          const { return id_; }
    bool isRecursive() const { return recursive_; }
    const std::string& path() const { return path_; }

    // Platform-side: producer pushes here. Public so the OS-specific .cpp
    // implementations (which are friends of nothing — they just include this
    // header and operate on a FsWatcher*) can append to the ring.
    void pushEvent(EventType type, std::string filename);

private:
    FsWatcher(int id, std::string path, bool recursive);

    // Platform-specific. Implemented in fs_watch_{linux,macos,windows}.cpp.
    // startNative initializes native_ and spawns thread_; on failure it
    // sets errMsg_ and leaves thread_ unjoinable. stopNative signals the
    // watcher thread to exit, joins it, and frees all native state.
    bool startNative(const std::string& path, bool recursive);
    void stopNative();

    int         id_        = 0;
    std::string path_;
    bool        recursive_ = false;

    // Lock-free SPSC ring.
    static constexpr size_t kCap = 1024;
    std::vector<Event>  ring_;
    std::atomic<size_t> head_{0}; // consumer reads from here (JS thread)
    std::atomic<size_t> tail_{0}; // producer writes to here (watcher thread)
    std::atomic<bool>   overflow_{false};

    // Owned by the platform layer.
    void*       native_   = nullptr;
    std::thread thread_;
    std::string errMsg_;

    static std::atomic<int> s_nextId_;
};

// JS bindings: install __brokit_fs_watch_* globals.
void installFSWatch(JSContext* ctx);

} // namespace brokit::api
