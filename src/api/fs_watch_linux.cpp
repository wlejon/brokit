// Linux backend for FsWatcher: inotify + epoll.
//
// Wakeup: an eventfd is registered alongside the inotify fd in epoll. close()
// writes 1 to the eventfd to unblock epoll_wait so the watcher thread can
// observe stop_ and exit promptly.
//
// Recursion: inotify is non-recursive natively. We bootstrap by walking the
// tree and adding a watch per directory; when IN_CREATE|IN_ISDIR fires we
// add the new directory at runtime. Each wd tracks its path relative to the
// watched root for synthesizing the reported filename.

#if defined(__linux__)

#include "api/fs_watch.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>

namespace fs = std::filesystem;

namespace brokit::api {

namespace {

constexpr uint32_t kWatchMask =
    IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
    IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF;

struct LinuxState {
    int inotifyFd = -1;
    int epollFd   = -1;
    int wakeFd    = -1; // eventfd for shutdown wake
    std::atomic<bool>           stop{false};
    bool                        recursive = false;
    std::string                 rootPath; // absolute, no trailing slash

    // Each watch descriptor maps to the relative subpath of the directory it
    // covers. The root has an empty relative path.
    std::unordered_map<int, std::string> wdToRel;

    int addWatch(const std::string& absDir, const std::string& rel) {
        int wd = inotify_add_watch(inotifyFd, absDir.c_str(), kWatchMask);
        if (wd >= 0) wdToRel[wd] = rel;
        return wd;
    }

    void addRecursiveSubtree(const std::string& absDir, const std::string& rel) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(
                 absDir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); ++it) {
            std::error_code ec2;
            if (!it->is_directory(ec2) || ec2) continue;
            std::string abs = it->path().string();
            std::string sub = fs::relative(it->path(), rootPath, ec2).generic_string();
            if (ec2) continue;
            addWatch(abs, sub);
        }
    }
};

std::string joinRel(const std::string& dir, const char* name)
{
    if (dir.empty()) return name;
    return dir + "/" + name;
}

void watcherLoop(FsWatcher* self, LinuxState* st)
{
    constexpr size_t kBufSize = 64 * 1024;
    std::vector<char> buf(kBufSize);

    while (!st->stop.load(std::memory_order_acquire)) {
        epoll_event events[2];
        int n = epoll_wait(st->epollFd, events, 2, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            self->pushEvent(FsWatcher::EventType::Error,
                            std::string("epoll_wait: ") + std::strerror(errno));
            break;
        }
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == st->wakeFd) {
                uint64_t v = 0;
                ssize_t r = read(st->wakeFd, &v, sizeof(v));
                (void)r;
                continue;
            }

            // Inotify fd readable: drain everything.
            for (;;) {
                ssize_t got = read(st->inotifyFd, buf.data(), buf.size());
                if (got <= 0) {
                    if (got < 0 && errno != EAGAIN && errno != EINTR) {
                        self->pushEvent(FsWatcher::EventType::Error,
                                        std::string("read(inotify): ") + std::strerror(errno));
                    }
                    break;
                }

                ssize_t off = 0;
                while (off < got) {
                    auto* ev = reinterpret_cast<inotify_event*>(buf.data() + off);
                    off += sizeof(inotify_event) + ev->len;

                    auto it = st->wdToRel.find(ev->wd);
                    if (it == st->wdToRel.end()) continue;
                    const std::string& rel = it->second;

                    const char* name = ev->len > 0 ? ev->name : "";

                    // Root went away: synthesize a rename + stop.
                    if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                        self->pushEvent(FsWatcher::EventType::Rename, rel);
                        st->wdToRel.erase(it);
                        if (rel.empty()) { // the root itself
                            st->stop.store(true, std::memory_order_release);
                        }
                        continue;
                    }
                    if (ev->mask & IN_IGNORED) {
                        st->wdToRel.erase(it);
                        continue;
                    }

                    std::string filename = joinRel(rel, name);
                    bool isRename = (ev->mask & (IN_CREATE | IN_DELETE |
                                                 IN_MOVED_FROM | IN_MOVED_TO)) != 0;
                    self->pushEvent(isRename ? FsWatcher::EventType::Rename
                                             : FsWatcher::EventType::Change,
                                    filename);

                    // Auto-add new subdirectories when watching recursively.
                    if (st->recursive && (ev->mask & IN_CREATE) && (ev->mask & IN_ISDIR)) {
                        std::string newAbs = st->rootPath + "/" + filename;
                        st->addWatch(newAbs, filename);
                        st->addRecursiveSubtree(newAbs, filename);
                    }
                }
            }
        }
    }
}

} // namespace

bool FsWatcher::startNative(const std::string& path, bool recursive)
{
    auto* st = new LinuxState();
    st->recursive = recursive;
    st->rootPath  = path;
    if (!st->rootPath.empty() && st->rootPath.back() == '/') st->rootPath.pop_back();
    native_ = st;

    st->inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (st->inotifyFd < 0) {
        errMsg_ = std::string("inotify_init1: ") + std::strerror(errno);
        return false;
    }
    st->wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (st->wakeFd < 0) {
        errMsg_ = std::string("eventfd: ") + std::strerror(errno);
        return false;
    }
    st->epollFd = epoll_create1(EPOLL_CLOEXEC);
    if (st->epollFd < 0) {
        errMsg_ = std::string("epoll_create1: ") + std::strerror(errno);
        return false;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;

    ev.data.fd = st->inotifyFd;
    if (epoll_ctl(st->epollFd, EPOLL_CTL_ADD, st->inotifyFd, &ev) < 0) {
        errMsg_ = std::string("epoll_ctl(inotify): ") + std::strerror(errno);
        return false;
    }
    ev.data.fd = st->wakeFd;
    if (epoll_ctl(st->epollFd, EPOLL_CTL_ADD, st->wakeFd, &ev) < 0) {
        errMsg_ = std::string("epoll_ctl(wake): ") + std::strerror(errno);
        return false;
    }

    if (st->addWatch(st->rootPath, "") < 0) {
        errMsg_ = std::string("inotify_add_watch: ") + std::strerror(errno);
        return false;
    }
    if (recursive) {
        st->addRecursiveSubtree(st->rootPath, "");
    }

    thread_ = std::thread(watcherLoop, this, st);
    return true;
}

void FsWatcher::stopNative()
{
    auto* st = static_cast<LinuxState*>(native_);
    if (!st) return;

    st->stop.store(true, std::memory_order_release);
    if (st->wakeFd >= 0) {
        uint64_t one = 1;
        ssize_t r = write(st->wakeFd, &one, sizeof(one));
        (void)r;
    }
    if (thread_.joinable()) thread_.join();

    if (st->epollFd   >= 0) close(st->epollFd);
    if (st->wakeFd    >= 0) close(st->wakeFd);
    if (st->inotifyFd >= 0) close(st->inotifyFd);

    delete st;
    native_ = nullptr;
}

} // namespace brokit::api

#endif // __linux__
