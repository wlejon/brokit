#include "api/fs_async.h"
#include "api/access_policy.h"
#include "api/arg_reader.h"
#include "api/object_builder.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace brokit::api {

namespace {

fs::path u8p(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

struct OpenFile {
    enum class Op { None, Read, Write };
    std::fstream stream;
    std::string path;
    bool readable = true;
    bool writable = false;
    bool append = false;
    Op lastOp = Op::None;
};

std::mutex g_fdMutex;
std::map<int, std::unique_ptr<OpenFile>> g_fds;
int g_nextFd = 3;

OpenFile* lookupFd(int fd)
{
    auto it = g_fds.find(fd);
    return it == g_fds.end() ? nullptr : it->second.get();
}

std::optional<bronze::embed::TypedArrayInfo> getBufferOrTypedArrayInfo(bronze::Value val)
{
    if (auto info = ev::typedArrayInfo(val)) return info;
    if (ev::isObject(val)) {
        bronze::Value u8 = ev::getProperty(val, "_u8");
        if (auto info = ev::typedArrayInfo(u8)) return info;
    }
    return std::nullopt;
}

// [offset, offset + length) inside a buffer of byteLength bytes, checked
// without forming offset + length (two script-sized int64s can overflow it
// back into range).
bool windowFits(int64_t offset, int64_t length, size_t byteLength)
{
    if (offset < 0 || length < 0) return false;
    const uint64_t cap = static_cast<uint64_t>(byteLength);
    return static_cast<uint64_t>(offset) <= cap &&
           static_cast<uint64_t>(length) <= cap - static_cast<uint64_t>(offset);
}

#ifdef _WIN32
FILETIME unixSecToFileTime(double sec)
{
    const int64_t ticks = static_cast<int64_t>(std::llround(sec * 1e7)) + 116444736000000000LL;
    const uint64_t u = ticks > 0 ? static_cast<uint64_t>(ticks) : 0ULL;
    FILETIME ft{};
    ft.dwLowDateTime = static_cast<DWORD>(u & 0xFFFFFFFFULL);
    ft.dwHighDateTime = static_cast<DWORD>(u >> 32);
    return ft;
}
#else
timespec unixSecToTimespec(double sec)
{
    double whole = std::floor(sec);
    const double frac = sec - whole;
    long nsec = static_cast<long>(std::llround(frac * 1e9));
    if (nsec >= 1000000000L) {
        whole += 1.0;
        nsec -= 1000000000L;
    } else if (nsec < 0) {
        nsec = 0;
    }
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(whole);
    ts.tv_nsec = nsec;
    return ts;
}
#endif

} // namespace

bool statPathFields(const std::string& path, bool follow, StatFields& out, std::error_code& ec)
{
    out = StatFields{};
#ifdef _WIN32
    const fs::path p = u8p(path);
    const auto status = follow ? fs::status(p, ec) : fs::symlink_status(p, ec);
    if (ec) return false;
    out.isFile = fs::is_regular_file(status);
    out.isDir = fs::is_directory(status);
    if (follow) {
        auto ls = fs::symlink_status(p, ec);
        out.isLink = !ec && fs::is_symlink(ls);
        ec.clear();
    } else {
        out.isLink = fs::is_symlink(status);
    }
    if (out.isFile) {
        auto sz = fs::file_size(p, ec);
        out.size = ec ? 0.0 : static_cast<double>(sz);
        ec.clear();
    }
    const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
    HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, flags, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileInformationByHandle(h, &info)) {
            auto ftToMs = [](const FILETIME& ft) -> double {
                const int64_t ticks = (static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
                const auto d = std::chrono::duration<int64_t, std::ratio<1, 10000000>>(ticks - 116444736000000000LL);
                return static_cast<double>(std::chrono::floor<std::chrono::milliseconds>(d).count());
            };
            out.atimeMs = ftToMs(info.ftLastAccessTime);
            out.mtimeMs = ftToMs(info.ftLastWriteTime);
            out.birthtimeMs = ftToMs(info.ftCreationTime);
            out.ctimeMs = out.mtimeMs;
            int mode = 0444;
            if (!(info.dwFileAttributes & FILE_ATTRIBUTE_READONLY)) mode |= 0222;
            if (out.isDir) mode |= 0111;
            out.mode = mode;
        }
        CloseHandle(h);
    } else {
        auto mtime = fs::last_write_time(p, ec);
        if (!ec) out.mtimeMs = fileTimeToUnixMs(mtime);
        ec.clear();
        out.atimeMs = out.mtimeMs;
        out.ctimeMs = out.mtimeMs;
        out.birthtimeMs = out.mtimeMs;
    }
    return true;
#else
    struct stat st{};
    const int r = follow ? ::stat(path.c_str(), &st) : ::lstat(path.c_str(), &st);
    if (r != 0) {
        ec = std::error_code(errno, std::generic_category());
        return false;
    }
    ec.clear();
    out.isFile = S_ISREG(st.st_mode);
    out.isDir = S_ISDIR(st.st_mode);
    if (follow) {
        struct stat lst{};
        out.isLink = (::lstat(path.c_str(), &lst) == 0) && S_ISLNK(lst.st_mode);
    } else {
        out.isLink = S_ISLNK(st.st_mode);
    }
    out.size = (out.isFile || out.isLink) ? static_cast<double>(st.st_size) : 0.0;
    out.mode = static_cast<double>(st.st_mode & 07777);
    auto tsToMs = [](const timespec& ts) -> double {
        const auto d = std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
        return static_cast<double>(std::chrono::floor<std::chrono::milliseconds>(d).count());
    };
#if defined(__APPLE__)
    out.atimeMs = tsToMs(st.st_atimespec);
    out.mtimeMs = tsToMs(st.st_mtimespec);
    out.ctimeMs = tsToMs(st.st_ctimespec);
    out.birthtimeMs = tsToMs(st.st_birthtimespec);
#else
    out.atimeMs = tsToMs(st.st_atim);
    out.mtimeMs = tsToMs(st.st_mtim);
    out.ctimeMs = tsToMs(st.st_ctim);
    out.birthtimeMs = out.ctimeMs;
#endif
    return true;
#endif
}

bronze::Value statFieldsToJs(const StatFields& s)
{
    ObjectBuilder obj;
    obj.set("size", ev::fromDouble(s.size));
    obj.set("mtimeMs", ev::fromDouble(s.mtimeMs));
    obj.set("atimeMs", ev::fromDouble(s.atimeMs));
    obj.set("ctimeMs", ev::fromDouble(s.ctimeMs));
    obj.set("birthtimeMs", ev::fromDouble(s.birthtimeMs));
    obj.set("mode", ev::fromDouble(s.mode));
    obj.set("_isFile", ev::fromBool(s.isFile));
    obj.set("_isDirectory", ev::fromBool(s.isDir));
    obj.set("_isSymbolicLink", ev::fromBool(s.isLink));
    return obj.get();
}

std::error_code utimesPath(const std::string& path, double atimeSec, double mtimeSec, bool follow)
{
#ifdef _WIN32
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    if (!follow) flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE h = CreateFileW(u8p(path).c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return std::error_code(static_cast<int>(GetLastError()), std::system_category());
    }
    const FILETIME at = unixSecToFileTime(atimeSec);
    const FILETIME mt = unixSecToFileTime(mtimeSec);
    std::error_code ec;
    if (!SetFileTime(h, nullptr, &at, &mt)) {
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    }
    CloseHandle(h);
    return ec;
#else
    const timespec times[2] = { unixSecToTimespec(atimeSec), unixSecToTimespec(mtimeSec) };
    if (::utimensat(AT_FDCWD, path.c_str(), times, follow ? 0 : AT_SYMLINK_NOFOLLOW) != 0) {
        return std::error_code(errno, std::generic_category());
    }
    return {};
#endif
}

bool flushFdForSync(int fd, std::string& path, bool& writable)
{
    std::lock_guard<std::mutex> lock(g_fdMutex);
    OpenFile* file = lookupFd(fd);
    if (!file) return false;
    file->stream.clear();
    if (file->writable) file->stream.flush();
    file->stream.clear();
    path = file->path;
    writable = file->writable;
    return true;
}

std::error_code syncPath(const std::string& path, bool dataOnly, bool writable)
{
#ifdef _WIN32
    (void)dataOnly;
    if (!writable) return {};
    HANDLE h = CreateFileW(u8p(path).c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::error_code ec(static_cast<int>(GetLastError()), std::system_category());
        if (ec == std::errc::no_such_file_or_directory) return {};
        return ec;
    }
    std::error_code ec;
    if (!FlushFileBuffers(h)) ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    CloseHandle(h);
    return ec;
#else
    (void)writable;
    int h = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (h < 0) {
        if (errno == ENOENT) return {};
        return std::error_code(errno, std::generic_category());
    }
    int r;
#if defined(__APPLE__)
    (void)dataOnly;
    r = ::fsync(h);
#else
    r = dataOnly ? ::fdatasync(h) : ::fsync(h);
#endif
    std::error_code ec;
    if (r != 0) ec = std::error_code(errno, std::generic_category());
    ::close(h);
    return ec;
#endif
}

bool truncateFd(int fd, int64_t len, std::string& path, std::error_code& ec)
{
    std::lock_guard<std::mutex> lock(g_fdMutex);
    OpenFile* file = lookupFd(fd);
    if (!file || !file->writable) return false;
    path = file->path;
    if (len < 0) {
        ec = std::make_error_code(std::errc::invalid_argument);
        return true;
    }
    file->stream.clear();
    file->stream.flush();
    file->stream.clear();
    fs::resize_file(u8p(file->path), static_cast<uintmax_t>(len), ec);
    if (!ec) {
        file->stream.clear();
        file->stream.seekg(0, std::ios::beg);
        file->stream.seekp(0, std::ios::beg);
        file->stream.clear();
        file->lastOp = OpenFile::Op::None;
    }
    return true;
}

// openSync(path[, flags]) -> fd
static bronze::Value js_openSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("openSync: path required");

    std::string rawPath = ev::toUtf8(a[0]);
    std::string flags = "r";
    if (a.size() >= 2 && ev::isString(a[1])) {
        flags = ev::toUtf8(a[1]);
    }

    std::ios::openmode mode = std::ios::binary;
    bool readable = false;
    bool writable = false;
    bool append = false;
    bool forCreate = false;
    bool excl = false;

    if (flags == "r" || flags == "rs") {
        mode |= std::ios::in;
        readable = true;
    } else if (flags == "r+" || flags == "rs+") {
        mode |= std::ios::in | std::ios::out;
        readable = true;
        writable = true;
    } else if (flags == "w" || flags == "wx") {
        mode |= std::ios::out | std::ios::trunc;
        writable = true;
        forCreate = true;
        excl = (flags == "wx");
    } else if (flags == "w+" || flags == "wx+") {
        mode |= std::ios::in | std::ios::out | std::ios::trunc;
        readable = true;
        writable = true;
        forCreate = true;
        excl = (flags == "wx+");
    } else if (flags == "a" || flags == "ax" || flags == "a+" || flags == "ax+") {
        mode |= std::ios::out | std::ios::app;
        if (flags == "a+" || flags == "ax+") {
            mode |= std::ios::in;
            readable = true;
        }
        writable = true;
        append = true;
        forCreate = true;
        excl = (flags == "ax" || flags == "ax+");
    } else {
        return ev::throwTypeError(("openSync: unsupported flags '" + flags + "'").c_str());
    }

    std::string resolved = resolveFsPathOnThread(rawPath, forCreate);
    // Asked for what the descriptor can do, so every later read or write
    // through it was granted here.
    if (readable) {
        if (auto r = fsRefusal(resolved, FsAccess::Read); !r.empty()) return throwFsDenied(r, resolved);
    }
    if (writable) {
        if (auto r = fsRefusal(resolved, FsAccess::Write); !r.empty()) return throwFsDenied(r, resolved);
    }

    std::error_code existsEc;
    if (excl && fs::exists(u8p(resolved), existsEc)) {
        return throwErrno("open", resolved.c_str(), "EEXIST",
                          ("EEXIST: file already exists, open '" + resolved + "'").c_str());
    }
    if ((flags == "r+" || flags == "rs+") && !fs::exists(u8p(resolved), existsEc)) {
        return throwErrno("open", resolved.c_str(), "ENOENT",
                          ("ENOENT: no such file or directory, open '" + resolved + "'").c_str());
    }

    auto file = std::make_unique<OpenFile>();
    file->stream.open(u8p(resolved), mode);
    if (!file->stream.is_open()) {
        return throwErrno("open", resolved.c_str(), "ENOENT",
                          ("ENOENT: no such file or directory, open '" + resolved + "'").c_str());
    }
    file->path = resolved;
    file->readable = readable;
    file->writable = writable;
    file->append = append;

    std::lock_guard<std::mutex> lock(g_fdMutex);
    int fd = g_nextFd++;
    g_fds[fd] = std::move(file);
    return ev::fromDouble(fd);
}

// readSync(fd, buffer, offset, length[, position]) -> bytes read
static bronze::Value js_readSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.size() < 2) return ev::throwTypeError("readSync: fd and buffer required");

    int32_t fd = i32At(a, 0);
    auto info = getBufferOrTypedArrayInfo(a[1]);
    if (!info) return ev::throwTypeError("readSync: buffer must be a typed array");

    uint8_t* base = info->data;
    size_t byteLength = info->byteLength;

    int64_t offset = 0, length = static_cast<int64_t>(byteLength);
    if (a.size() >= 3 && !ev::isUndefined(a[2]) && !ev::isNull(a[2])) {
        offset = saturateI64(ev::toDouble(a[2]));
    }
    if (a.size() >= 4 && !ev::isUndefined(a[3]) && !ev::isNull(a[3])) {
        length = saturateI64(ev::toDouble(a[3]));
    }
    if (!windowFits(offset, length, byteLength)) {
        return ev::throwRangeError("readSync: offset/length out of buffer bounds");
    }

    bool seek = false;
    int64_t position = 0;
    if (a.size() >= 5 && !ev::isUndefined(a[4]) && !ev::isNull(a[4])) {
        position = saturateI64(ev::toDouble(a[4]));
        if (position >= 0) seek = true;
    }

    info = getBufferOrTypedArrayInfo(a[1]);
    if (!info || !windowFits(offset, length, info->byteLength))
        return ev::throwRangeError("readSync: buffer changed size during the call");
    base = info->data;

    std::lock_guard<std::mutex> lock(g_fdMutex);
    OpenFile* file = lookupFd(fd);
    if (!file) return throwErrno("read", nullptr, "EBADF", "EBADF: bad file descriptor, read");
    if (!file->readable) {
        return throwErrno("read", file->path.c_str(), "EBADF",
                          "EBADF: file descriptor is not open for reading, read");
    }

    file->stream.clear();
    if (seek) {
        file->stream.seekg(static_cast<std::streamoff>(position), std::ios::beg);
    } else if (file->lastOp == OpenFile::Op::Write) {
        file->stream.seekg(file->stream.tellp());
    }
    if (!file->stream) {
        return throwErrno("read", file->path.c_str(), "EINVAL", "EINVAL: invalid position, read");
    }

    file->stream.read(reinterpret_cast<char*>(base + offset),
                      static_cast<std::streamsize>(length));
    std::streamsize got = file->stream.gcount();
    file->stream.clear();
    file->lastOp = OpenFile::Op::Read;
    return ev::fromDouble(static_cast<double>(got));
}

// writeSync(fd, buffer, offset, length[, position]) -> bytes written
static bronze::Value js_writeSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.size() < 2) return ev::throwTypeError("writeSync: fd and buffer required");

    int32_t fd = i32At(a, 0);
    auto info = getBufferOrTypedArrayInfo(a[1]);
    if (!info) return ev::throwTypeError("writeSync: buffer must be a typed array");

    uint8_t* base = info->data;
    size_t byteLength = info->byteLength;

    int64_t offset = 0, length = static_cast<int64_t>(byteLength);
    if (a.size() >= 3 && !ev::isUndefined(a[2]) && !ev::isNull(a[2])) {
        offset = saturateI64(ev::toDouble(a[2]));
    }
    if (a.size() >= 4 && !ev::isUndefined(a[3]) && !ev::isNull(a[3])) {
        length = saturateI64(ev::toDouble(a[3]));
    }
    if (!windowFits(offset, length, byteLength)) {
        return ev::throwRangeError("writeSync: offset/length out of buffer bounds");
    }

    bool seek = false;
    int64_t position = 0;
    if (a.size() >= 5 && !ev::isUndefined(a[4]) && !ev::isNull(a[4])) {
        position = saturateI64(ev::toDouble(a[4]));
        if (position >= 0) seek = true;
    }

    info = getBufferOrTypedArrayInfo(a[1]);
    if (!info || !windowFits(offset, length, info->byteLength))
        return ev::throwRangeError("writeSync: buffer changed size during the call");
    base = info->data;

    std::lock_guard<std::mutex> lock(g_fdMutex);
    OpenFile* file = lookupFd(fd);
    if (!file) return throwErrno("write", nullptr, "EBADF", "EBADF: bad file descriptor, write");
    if (!file->writable) {
        return throwErrno("write", file->path.c_str(), "EBADF",
                          "EBADF: file descriptor is not open for writing, write");
    }

    file->stream.clear();
    if (seek && !file->append) {
        file->stream.seekp(static_cast<std::streamoff>(position), std::ios::beg);
    } else if (!file->append && file->lastOp == OpenFile::Op::Read) {
        file->stream.seekp(file->stream.tellg());
    }
    file->stream.write(reinterpret_cast<const char*>(base + offset),
                       static_cast<std::streamsize>(length));
    if (!file->stream) {
        return throwErrno("write", file->path.c_str(), "EIO", "EIO: write failed");
    }
    file->lastOp = OpenFile::Op::Write;
    return ev::fromDouble(static_cast<double>(length));
}

// fstatSync(fd) -> { size, mtimeMs, atimeMs, ... }
static bronze::Value js_fstatSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("fstatSync: fd required");
    int32_t fd = i32At(a, 0);

    std::string path;
    bool writable = false;
    if (!flushFdForSync(fd, path, writable)) {
        return throwErrno("fstat", nullptr, "EBADF", "EBADF: bad file descriptor, fstat");
    }

    StatFields sf;
    std::error_code ec;
    if (!statPathFields(path, /*follow=*/true, sf, ec)) {
        return throwFsError("fstat", path.c_str(), ec);
    }
    return statFieldsToJs(sf);
}

// closeSync(fd)
static bronze::Value js_closeSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::undefined();
    int32_t fd = i32At(a, 0);

    std::lock_guard<std::mutex> lock(g_fdMutex);
    auto it = g_fds.find(fd);
    if (it == g_fds.end()) {
        return throwErrno("close", nullptr, "EBADF", "EBADF: bad file descriptor, close");
    }
    it->second->stream.close();
    g_fds.erase(it);
    return ev::undefined();
}

// fsyncSync(fd[, dataOnly])
static bronze::Value js_fsyncSync(bronze::Value, std::span<const bronze::Value> a)
{
    int32_t fd = a.empty() ? -1 : i32At(a, 0);
    const bool dataOnly = a.size() >= 2 && ev::isBool(a[1]) && ev::toBool(a[1]);
    const char* syscall = dataOnly ? "fdatasync" : "fsync";
    std::string path;
    bool writable = false;
    if (!flushFdForSync(fd, path, writable)) {
        return throwErrno(syscall, nullptr, "EBADF", (std::string("EBADF: bad file descriptor, ") + syscall).c_str());
    }
    std::error_code ec = syncPath(path, dataOnly, writable);
    if (ec) return throwFsError(syscall, nullptr, ec);
    return ev::undefined();
}

// ftruncateSync(fd[, len])
static bronze::Value js_ftruncateSync(bronze::Value, std::span<const bronze::Value> a)
{
    int32_t fd = a.empty() ? -1 : i32At(a, 0);
    int64_t len = (a.size() >= 2 && !ev::isUndefined(a[1]) && !ev::isNull(a[1]))
                      ? saturateI64(ev::toDouble(a[1]))
                      : 0;
    std::string path;
    std::error_code ec;
    if (!truncateFd(fd, len, path, ec)) {
        return throwErrno("ftruncate", nullptr, "EBADF", "EBADF: bad file descriptor, ftruncate");
    }
    if (ec) return throwFsError("ftruncate", path.c_str(), ec);
    return ev::undefined();
}

// futimesSync(fd, atimeSec, mtimeSec)
static bronze::Value js_futimesSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.size() < 3) return ev::throwTypeError("futimesSync: fd, atime, and mtime required");
    int32_t fd = i32At(a, 0);
    const double atimeSec = ev::toDouble(a[1]);
    const double mtimeSec = ev::toDouble(a[2]);
    if (!std::isfinite(atimeSec) || !std::isfinite(mtimeSec)) {
        return ev::throwTypeError("futimesSync: atime and mtime must be finite numbers");
    }
    std::string path;
    bool writable = false;
    if (!flushFdForSync(fd, path, writable)) {
        return throwErrno("futime", nullptr, "EBADF", "EBADF: bad file descriptor, futime");
    }
    std::error_code ec = utimesPath(path, atimeSec, mtimeSec, /*follow=*/true);
    if (ec) return throwFsError("futime", nullptr, ec);
    return ev::undefined();
}

void installFSFd()
{
    ev::setGlobalFunction("__brokit_fs_openSync", 2, js_openSync);
    ev::setGlobalFunction("__brokit_fs_readSync", 5, js_readSync);
    ev::setGlobalFunction("__brokit_fs_writeSync", 5, js_writeSync);
    ev::setGlobalFunction("__brokit_fs_fstatSync", 1, js_fstatSync);
    ev::setGlobalFunction("__brokit_fs_closeSync", 1, js_closeSync);
    ev::setGlobalFunction("__brokit_fs_fsyncSync", 2, js_fsyncSync);
    ev::setGlobalFunction("__brokit_fs_ftruncateSync", 2, js_ftruncateSync);
    ev::setGlobalFunction("__brokit_fs_futimesSync", 3, js_futimesSync);
}

} // namespace brokit::api
