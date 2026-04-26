// Windows backend for FsWatcher: ReadDirectoryChangesW + IOCP.
//
// CreateFile opens the watched directory in async mode. The handle is
// associated with an I/O completion port; the watcher thread blocks in
// GetQueuedCompletionStatus and is woken either by a completed read (events
// to deliver, then re-issue) or by PostQueuedCompletionStatus from
// stopNative (shutdown — observe stop_ and exit).
//
// Recursion is a single bWatchSubtree=TRUE flag — Windows handles the
// subtree natively, so no per-subdir bookkeeping is needed.
//
// Filenames in FILE_NOTIFY_INFORMATION are wide (UTF-16, no NUL), with
// backslashes for subdirectories. We convert to UTF-8 and normalize to
// forward slashes for cross-platform consistency.

#if defined(_WIN32)

#include "api/fs_watch.h"

#include <atomic>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace brokit::api {

namespace {

constexpr ULONG_PTR kKeyDir      = 1;
constexpr ULONG_PTR kKeyShutdown = 2;

constexpr DWORD kBufferBytes = 64 * 1024;

constexpr DWORD kFilter =
    FILE_NOTIFY_CHANGE_FILE_NAME  |
    FILE_NOTIFY_CHANGE_DIR_NAME   |
    FILE_NOTIFY_CHANGE_ATTRIBUTES |
    FILE_NOTIFY_CHANGE_SIZE       |
    FILE_NOTIFY_CHANGE_LAST_WRITE |
    FILE_NOTIFY_CHANGE_CREATION;

struct WindowsState {
    HANDLE                 dirHandle = INVALID_HANDLE_VALUE;
    HANDLE                 iocp      = nullptr;
    OVERLAPPED             overlapped{};
    std::vector<uint8_t>   buffer;
    std::atomic<bool>      stop{false};
    bool                   recursive = false;
};

std::string utf16ToUtf8(const wchar_t* w, size_t wlen)
{
    if (wlen == 0) return {};
    int bytes = WideCharToMultiByte(
        CP_UTF8, 0, w, static_cast<int>(wlen), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(bytes, '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, w, static_cast<int>(wlen), out.data(), bytes, nullptr, nullptr);
    for (auto& c : out) if (c == '\\') c = '/';
    return out;
}

bool issueRead(WindowsState* st)
{
    ZeroMemory(&st->overlapped, sizeof(st->overlapped));
    DWORD got = 0;
    BOOL ok = ReadDirectoryChangesW(
        st->dirHandle,
        st->buffer.data(),
        static_cast<DWORD>(st->buffer.size()),
        st->recursive ? TRUE : FALSE,
        kFilter,
        &got,
        &st->overlapped,
        nullptr);
    return ok != FALSE;
}

void watcherLoop(FsWatcher* self, WindowsState* st)
{
    if (!issueRead(st)) {
        DWORD err = GetLastError();
        self->pushEvent(FsWatcher::EventType::Error,
                        "ReadDirectoryChangesW failed: " + std::to_string(err));
        return;
    }

    while (!st->stop.load(std::memory_order_acquire)) {
        DWORD       bytes  = 0;
        ULONG_PTR   key    = 0;
        OVERLAPPED* ovl    = nullptr;

        BOOL ok = GetQueuedCompletionStatus(
            st->iocp, &bytes, &key, &ovl, INFINITE);

        if (st->stop.load(std::memory_order_acquire)) break;
        if (key == kKeyShutdown) break;

        if (!ok) {
            DWORD err = GetLastError();
            self->pushEvent(FsWatcher::EventType::Error,
                            "GetQueuedCompletionStatus failed: " + std::to_string(err));
            break;
        }

        if (key != kKeyDir || bytes == 0) {
            // 0 bytes = buffer overflow on the kernel side; we'll re-arm and
            // continue. Caller already saw an overflow indicator on our ring
            // when it occurs locally.
            if (!issueRead(st)) break;
            continue;
        }

        const uint8_t* p = st->buffer.data();
        for (;;) {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);

            std::string name = utf16ToUtf8(
                info->FileName, info->FileNameLength / sizeof(wchar_t));

            FsWatcher::EventType type;
            switch (info->Action) {
                case FILE_ACTION_ADDED:
                case FILE_ACTION_REMOVED:
                case FILE_ACTION_RENAMED_OLD_NAME:
                case FILE_ACTION_RENAMED_NEW_NAME:
                    type = FsWatcher::EventType::Rename;
                    break;
                case FILE_ACTION_MODIFIED:
                default:
                    type = FsWatcher::EventType::Change;
                    break;
            }
            self->pushEvent(type, std::move(name));

            if (info->NextEntryOffset == 0) break;
            p += info->NextEntryOffset;
        }

        if (!issueRead(st)) break;
    }
}

} // namespace

bool FsWatcher::startNative(const std::string& path, bool recursive)
{
    auto* st = new WindowsState();
    st->recursive = recursive;
    st->buffer.resize(kBufferBytes);
    native_ = st;

    // CreateFileW takes UTF-16; the path arrives as UTF-8.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wlen ? wlen - 1 : 0, L'\0');
    if (wlen) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);

    st->dirHandle = CreateFileW(
        wpath.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (st->dirHandle == INVALID_HANDLE_VALUE) {
        errMsg_ = "CreateFileW failed: " + std::to_string(GetLastError());
        return false;
    }

    st->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (!st->iocp) {
        errMsg_ = "CreateIoCompletionPort failed: " + std::to_string(GetLastError());
        return false;
    }
    if (!CreateIoCompletionPort(st->dirHandle, st->iocp, kKeyDir, 1)) {
        errMsg_ = "CreateIoCompletionPort(assoc) failed: " + std::to_string(GetLastError());
        return false;
    }

    thread_ = std::thread(watcherLoop, this, st);
    return true;
}

void FsWatcher::stopNative()
{
    auto* st = static_cast<WindowsState*>(native_);
    if (!st) return;

    st->stop.store(true, std::memory_order_release);
    if (st->dirHandle != INVALID_HANDLE_VALUE) {
        // Cancel any in-flight read. Both this and the PostQueued below race
        // with the watcher thread; either suffices to wake it.
        CancelIoEx(st->dirHandle, &st->overlapped);
    }
    if (st->iocp) {
        PostQueuedCompletionStatus(st->iocp, 0, kKeyShutdown, nullptr);
    }
    if (thread_.joinable()) thread_.join();

    if (st->dirHandle != INVALID_HANDLE_VALUE) CloseHandle(st->dirHandle);
    if (st->iocp) CloseHandle(st->iocp);

    delete st;
    native_ = nullptr;
}

} // namespace brokit::api

#endif // _WIN32
