// macOS backend for FsWatcher: FSEvents on a private CFRunLoop.
//
// One CFRunLoop runs on the watcher thread. The FSEventStream is scheduled
// on that loop, so events fire as native callbacks on the watcher thread —
// the callback is the sole producer for the event ring.
//
// Wakeup: stopNative atomically reads the CFRunLoopRef the watcher thread
// stored at startup and calls CFRunLoopStop. The thread observes loop
// termination, tears down the stream, and exits.
//
// FSEvents is recursive by nature. For non-recursive watches we filter at
// dispatch time, only forwarding events whose path is a direct child of the
// watched root.

#if defined(__APPLE__)

#include "api/fs_watch.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <CoreServices/CoreServices.h>

namespace brokit::api {

namespace {

struct MacOSState {
    FSEventStreamRef                stream  = nullptr;
    std::atomic<CFRunLoopRef>       runLoop{nullptr};
    std::atomic<bool>               started{false};
    std::string                     rootPath;
    bool                            recursive = false;
};

constexpr FSEventStreamEventFlags kRenameFlags =
    kFSEventStreamEventFlagItemCreated  |
    kFSEventStreamEventFlagItemRemoved  |
    kFSEventStreamEventFlagItemRenamed;

constexpr FSEventStreamEventFlags kChangeFlags =
    kFSEventStreamEventFlagItemModified       |
    kFSEventStreamEventFlagItemInodeMetaMod   |
    kFSEventStreamEventFlagItemFinderInfoMod  |
    kFSEventStreamEventFlagItemChangeOwner    |
    kFSEventStreamEventFlagItemXattrMod;

void streamCallback(ConstFSEventStreamRef /*stream*/,
                    void* clientCallBackInfo,
                    size_t numEvents,
                    void* eventPaths,
                    const FSEventStreamEventFlags eventFlags[],
                    const FSEventStreamEventId   /*eventIds*/[])
{
    auto*  self = static_cast<FsWatcher*>(clientCallBackInfo);
    char** paths = static_cast<char**>(eventPaths);

    // Find the platform state via a friend? We don't have one — but the
    // FsWatcher's path() and recursive flag are enough for filename
    // computation here.
    const std::string& root = self->path();
    bool recursive = self->isRecursive();

    for (size_t i = 0; i < numEvents; i++) {
        std::string p = paths[i];

        // Strip root prefix → relative path.
        std::string rel;
        if (p.size() >= root.size() &&
            p.compare(0, root.size(), root) == 0)
        {
            rel = p.substr(root.size());
            if (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
        } else {
            rel = p; // out of root; report as-is
        }

        // Non-recursive: only direct children of the root.
        if (!recursive && rel.find('/') != std::string::npos) continue;

        FSEventStreamEventFlags f = eventFlags[i];

        if (f & kRenameFlags) {
            self->pushEvent(FsWatcher::EventType::Rename, rel);
        }
        if (f & kChangeFlags) {
            self->pushEvent(FsWatcher::EventType::Change, rel);
        }
        if (f & (kFSEventStreamEventFlagRootChanged |
                 kFSEventStreamEventFlagMustScanSubDirs))
        {
            self->pushEvent(FsWatcher::EventType::Rename, rel);
        }
    }
}

void watcherLoop(FsWatcher* self, MacOSState* st)
{
    CFRunLoopRef rl = CFRunLoopGetCurrent();
    st->runLoop.store(rl, std::memory_order_release);

    FSEventStreamContext ctx{};
    ctx.info = self;

    CFStringRef path = CFStringCreateWithCString(
        kCFAllocatorDefault, st->rootPath.c_str(), kCFStringEncodingUTF8);
    CFArrayRef paths = CFArrayCreate(
        kCFAllocatorDefault, (const void**)&path, 1, &kCFTypeArrayCallBacks);

    FSEventStreamCreateFlags flags =
        kFSEventStreamCreateFlagFileEvents |
        kFSEventStreamCreateFlagNoDefer;
    if (st->recursive) {
        flags |= kFSEventStreamCreateFlagWatchRoot;
    }

    st->stream = FSEventStreamCreate(
        kCFAllocatorDefault, &streamCallback, &ctx, paths,
        kFSEventStreamEventIdSinceNow,
        /*latency seconds*/ 0.05,
        flags);

    CFRelease(paths);
    CFRelease(path);

    if (!st->stream) {
        self->pushEvent(FsWatcher::EventType::Error, "FSEventStreamCreate failed");
        st->started.store(true, std::memory_order_release);
        return;
    }

    FSEventStreamScheduleWithRunLoop(st->stream, rl, kCFRunLoopDefaultMode);
    if (!FSEventStreamStart(st->stream)) {
        self->pushEvent(FsWatcher::EventType::Error, "FSEventStreamStart failed");
        FSEventStreamInvalidate(st->stream);
        FSEventStreamRelease(st->stream);
        st->stream = nullptr;
        st->started.store(true, std::memory_order_release);
        return;
    }

    st->started.store(true, std::memory_order_release);

    CFRunLoopRun();

    if (st->stream) {
        FSEventStreamStop(st->stream);
        FSEventStreamInvalidate(st->stream);
        FSEventStreamRelease(st->stream);
        st->stream = nullptr;
    }
}

} // namespace

bool FsWatcher::startNative(const std::string& path, bool recursive)
{
    auto* st = new MacOSState();
    st->rootPath  = path;
    if (!st->rootPath.empty() && st->rootPath.back() == '/') st->rootPath.pop_back();
    st->recursive = recursive;
    native_ = st;

    thread_ = std::thread(watcherLoop, this, st);

    // Wait briefly for the loop to capture its CFRunLoopRef. Spin-wait on an
    // atomic — no condvar (project rule: no mutexes). The loop sets `started`
    // once initialization completes (success or failure).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!st->started.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) {
            errMsg_ = "fs.watch: macOS init timed out";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!st->stream) {
        errMsg_ = "fs.watch: FSEventStream init failed";
        return false;
    }
    return true;
}

void FsWatcher::stopNative()
{
    auto* st = static_cast<MacOSState*>(native_);
    if (!st) return;

    CFRunLoopRef rl = st->runLoop.load(std::memory_order_acquire);
    if (rl) CFRunLoopStop(rl);

    if (thread_.joinable()) thread_.join();

    delete st;
    native_ = nullptr;
}

} // namespace brokit::api

#endif // __APPLE__
