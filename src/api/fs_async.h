#pragma once
// fs's asynchronous half and shared filesystem helpers: fs.promises and the
// callback functions run their I/O on a small pool of threads and settle on
// the JS thread that asked, at its next __brokit_fs_async_tick.

#include "api/api.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace brokit::api {

/// A file time as Unix-epoch milliseconds, truncated to the millisecond. A
/// pure conversion: the same file time always gives the same number, from
/// stat, fs.promises.stat and every other caller.
double fileTimeToUnixMs(std::filesystem::file_time_type t);

/// The path a script named, resolved on the calling (JS) thread: prefix
/// mounts (/lib, /system) and the fs base paths. `forCreate` resolves a path
/// that need not exist yet against the first base path.
std::string resolveFsPathOnThread(const std::string& path, bool forCreate = false);

/// The Node error code (ENOENT, EBADF, ...) for a filesystem error.
const char* fsErrorCode(const std::error_code& ec);

bronze::Value throwFsError(const char* syscall, const char* path, const std::error_code& ec);
bronze::Value throwErrno(const char* syscall, const char* path, const char* code, const char* message);

struct StatFields {
    double size = 0;
    double mtimeMs = 0;
    double atimeMs = 0;
    double ctimeMs = 0;
    double birthtimeMs = 0;
    double mode = 0;
    bool isFile = false;
    bool isDir = false;
    bool isLink = false;
};

/// Populate StatFields for `path` (following symlinks when `follow` is true).
bool statPathFields(const std::string& path, bool follow, StatFields& out, std::error_code& ec);
bronze::Value statFieldsToJs(const StatFields& s);

/// Update access and modification timestamps (in Unix epoch seconds, with
/// sub-second fractional precision) on `path`. When `follow` is false
/// (lutimes), operates on a symlink itself without following it.
std::error_code utimesPath(const std::string& path, double atimeSec, double mtimeSec, bool follow);

/// fsync's first half, on the JS thread: push what an open fd (openSync)
/// buffered to the OS and give the path it names and whether it was opened
/// for writing. False for a descriptor that is not open.
bool flushFdForSync(int fd, std::string& path, bool& writable);

/// fsync's second half, on any thread: flush the file's data (and, unless
/// `dataOnly`, its metadata) to the disk. fsync / fdatasync on POSIX,
/// FlushFileBuffers on Windows. A file gone since it was opened answers no
/// error: what the stream held reached the OS at flushFdForSync.
std::error_code syncPath(const std::string& path, bool dataOnly, bool writable);

/// Resize an open writable file descriptor to `len` bytes. Returns false if
/// `fd` is not an open writable descriptor (leaving `ec` clear -> EBADF).
bool truncateFd(int fd, int64_t len, std::string& path, std::error_code& ec);

/// mkdtemp: create `prefix` + six random characters as a directory (mode
/// 0700 on POSIX), retrying a name that exists. `resolvedPrefix` is the
/// prefix resolved to the real filesystem; the six characters are returned.
std::string makeTempDir(const std::string& resolvedPrefix, std::error_code& ec);

void installFSFd();
void installFSAsync();

}  // namespace brokit::api
