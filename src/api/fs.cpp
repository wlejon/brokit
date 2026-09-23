#include "api/api.h"
#include "api/arg_reader.h"
#include "api/object_builder.h"

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#endif

namespace fs = std::filesystem;

extern "C" void bronze_fs_main();

namespace brokit::api {

// Paths are UTF-8 strings on the JS side. A std::string handed straight to
// std::filesystem or an fstream is read in the ANSI code page on Windows,
// which mangles every non-ASCII name, so each crossing goes through these.
static fs::path u8p(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
static std::string u8s(const fs::path& p)
{
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

static thread_local std::map<std::string, std::string> g_pathMounts;

std::string resolveBrokitPrefixMount(const std::string& path)
{
    if (path.empty() || path[0] != '/') return {};

    std::string best, bestRewrite;

    auto g = ev::globalValue("globalThis");
    if (g.found && ev::isObject(g.value)) {
        // Every Value below outlives an allocating call, so each is rooted.
        ev::Persistent mounts{ev::getProperty(g.value, "__brokit_path_mounts")};
        if (ev::isObject(mounts.get())) {
            auto objCtor = ev::globalValue("Object");
            if (objCtor.found) {
                ev::Persistent ctor{objCtor.value};
                ev::Persistent keysFn{ev::getProperty(ctor.get(), "keys")};
                if (ev::isFunction(keysFn.get())) {
                    bronze::Value arg = mounts.get();
                    auto res = ev::call(keysFn.get(), ctor.get(), std::span(&arg, 1));
                    ev::Persistent keys{res.value};
                    if (!res.thrown && ev::isObject(keys.get())) {
                        auto lenVal = ev::getProperty(keys.get(), "length");
                        uint32_t len = saturateU32(ev::toDouble(lenVal));
                        for (uint32_t i = 0; i < len; ++i) {
                            auto k = ev::getElement(keys.get(), i);
                            std::string prefix = ev::toUtf8(k);
                            auto targetVal = ev::getProperty(mounts.get(), prefix);
                            if (ev::isString(targetVal)) {
                                std::string target = ev::toUtf8(targetVal);
                                if (path.size() >= prefix.size() &&
                                    path.compare(0, prefix.size(), prefix) == 0 &&
                                    (path.size() == prefix.size() || path[prefix.size()] == '/'))
                                {
                                    if (prefix.size() > best.size()) {
                                        best = prefix;
                                        bestRewrite = target + path.substr(prefix.size());
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    for (const auto& [prefix, target] : g_pathMounts) {
        if (path.size() >= prefix.size() &&
            path.compare(0, prefix.size(), prefix) == 0 &&
            (path.size() == prefix.size() || path[prefix.size()] == '/'))
        {
            if (prefix.size() > best.size()) {
                best = prefix;
                bestRewrite = target + path.substr(prefix.size());
            }
        }
    }
    return bestRewrite;
}

namespace {

thread_local std::vector<std::string> g_fsBasePaths;

std::string resolveFsPath(const char* path, bool forCreate = false)
{
    std::string mounted = resolveBrokitPrefixMount(std::string(path));
    if (!mounted.empty()) return mounted;

    const std::string pathStr(path);
    const fs::path rel = u8p(pathStr);
    if (rel.is_absolute()) return pathStr;

    std::string topBase;

    auto g = ev::globalValue("globalThis");
    if (g.found && ev::isObject(g.value)) {
        ev::Persistent arr{ev::getProperty(g.value, "__brokit_fs_base_paths")};
        if (ev::isObject(arr.get())) {
            auto lenVal = ev::getProperty(arr.get(), "length");
            // A script-writable global: bound the walk.
            int32_t len = ev::isNumber(lenVal)
                              ? static_cast<int32_t>((std::min)(saturateU32(ev::toDouble(lenVal)), kMaxScriptList))
                              : 0;
            for (int32_t i = len - 1; i >= 0; --i) {
                auto elem = ev::getElement(arr.get(), static_cast<uint32_t>(i));
                if (ev::isString(elem)) {
                    std::string base = ev::toUtf8(elem);
                    fs::path candidate = u8p(base) / rel;
                    if (topBase.empty()) topBase = base;

                    std::error_code ec;
                    if (fs::exists(candidate, ec)) {
                        return u8s(candidate);
                    }
                }
            }
        }
    }

    for (int i = static_cast<int>(g_fsBasePaths.size()) - 1; i >= 0; --i) {
        const auto& base = g_fsBasePaths[i];
        fs::path candidate = u8p(base) / rel;
        if (topBase.empty()) topBase = base;

        std::error_code ec;
        if (fs::exists(candidate, ec)) {
            return u8s(candidate);
        }
    }

    if (forCreate && !topBase.empty()) {
        return u8s(u8p(topBase) / rel);
    }

    return pathStr;
}

std::string getEncoding(std::span<const bronze::Value> a, size_t idx)
{
    if (idx >= a.size()) return "";
    bronze::Value v = a[idx];
    if (ev::isString(v)) return ev::toUtf8(v);
    if (ev::isObject(v)) {
        bronze::Value enc = ev::getProperty(v, "encoding");
        if (ev::isString(enc)) return ev::toUtf8(enc);
    }
    return "";
}

bronze::Value throwFsError(const char* syscall, const char* path, const std::error_code& ec)
{
    const char* code = "ERR_FS";
    if (ec == std::errc::no_such_file_or_directory) code = "ENOENT";
    else if (ec == std::errc::file_exists) code = "EEXIST";
    else if (ec == std::errc::permission_denied) code = "EACCES";
    else if (ec == std::errc::is_a_directory) code = "EISDIR";
    else if (ec == std::errc::not_a_directory) code = "ENOTDIR";
    else if (ec == std::errc::directory_not_empty) code = "ENOTEMPTY";
    else if (ec == std::errc::no_space_on_device) code = "ENOSPC";
    else if (ec == std::errc::too_many_files_open) code = "EMFILE";
    else if (ec == std::errc::cross_device_link) code = "EXDEV";

    std::string msg = std::string(code) + ": " + ec.message() + ", " + syscall;
    if (path) msg += " '" + std::string(path) + "'";

    ObjectBuilder err;
    err.set("message", ev::fromUtf8(msg));
    err.set("code", ev::fromUtf8(code));
    if (syscall) err.set("syscall", ev::fromUtf8(syscall));
    if (path) err.set("path", ev::fromUtf8(path));
    return ev::throwValue(err.get());
}

bronze::Value throwErrno(const char* syscall, const char* path, const char* code, const char* message)
{
    ObjectBuilder err;
    err.set("message", ev::fromUtf8(message));
    if (code) err.set("code", ev::fromUtf8(code));
    if (syscall) err.set("syscall", ev::fromUtf8(syscall));
    if (path) err.set("path", ev::fromUtf8(path));
    return ev::throwValue(err.get());
}

bronze::Value makeU8(std::span<const uint8_t> bytes)
{
    bronze::Value v = ev::createTypedArray(elements::Uint8, static_cast<uint32_t>(bytes.size()));
    ev::fillTypedArray(v, bytes);
    return v;
}

struct OpenFile {
    std::fstream stream;
    std::string path;
    bool writable = false;
};

std::mutex g_fdMutex;
std::map<int, std::unique_ptr<OpenFile>> g_fds;
int g_nextFd = 3;

OpenFile* lookupFd(int fd)
{
    auto it = g_fds.find(fd);
    return it == g_fds.end() ? nullptr : it->second.get();
}

} // namespace

std::string resolveAssetPath(const std::string& path)
{
    return resolveFsPath(path.c_str(), false);
}

void addFsBasePath(const std::string& path)
{
    g_fsBasePaths.push_back(path);
}

void addFsPrefixMount(const std::string& prefix, const std::string& absPath)
{
    if (!prefix.empty() && prefix[0] == '/') {
        g_pathMounts[prefix] = absPath;
    }
}

void addFetchPrefixMount(const std::string& prefix, const std::string& absPath)
{
    addFsPrefixMount(prefix, absPath);
}

// readFileSync(path[, encoding])
static bronze::Value js_readFileSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("readFileSync: path required");

    std::string rawPath = ev::toUtf8(a[0]);
    std::string resolved = resolveFsPath(rawPath.c_str());
    std::string encoding = getEncoding(a, 1);

    std::ifstream f(u8p(resolved), std::ios::in | std::ios::binary);
    if (!f) {
        return throwErrno("open", resolved.c_str(), "ENOENT",
                          ("ENOENT: no such file or directory, open '" + resolved + "'").c_str());
    }

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string data = ss.str();
    f.close();

    if (!encoding.empty()) {
        return ev::fromUtf8(data);
    }

    return makeU8(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
}

// openSync(path[, flags]) -> fd
static bronze::Value js_openSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("openSync: path required");

    std::string rawPath = ev::toUtf8(a[0]);
    std::string resolved = resolveFsPath(rawPath.c_str());

    std::string flags = "r";
    if (a.size() >= 2 && ev::isString(a[1])) {
        flags = ev::toUtf8(a[1]);
    }

    std::ios::openmode mode = std::ios::binary;
    bool writable = false;
    if (flags == "r") {
        mode |= std::ios::in;
    } else if (flags == "r+") {
        mode |= std::ios::in | std::ios::out;
        writable = true;
    } else if (flags == "w") {
        mode |= std::ios::out | std::ios::trunc;
        writable = true;
    } else if (flags == "w+") {
        mode |= std::ios::in | std::ios::out | std::ios::trunc;
        writable = true;
    } else if (flags == "a" || flags == "a+") {
        mode |= std::ios::out | std::ios::app;
        if (flags == "a+") mode |= std::ios::in;
        writable = true;
    } else {
        return ev::throwTypeError(("openSync: unsupported flags '" + flags + "'").c_str());
    }

    std::error_code existsEc;
    if (flags == "r+" && !fs::exists(u8p(resolved), existsEc)) {
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
    file->writable = writable;

    std::lock_guard<std::mutex> lock(g_fdMutex);
    int fd = g_nextFd++;
    g_fds[fd] = std::move(file);
    return ev::fromDouble(fd);
}

static std::optional<bronze::embed::TypedArrayInfo> getBufferOrTypedArrayInfo(bronze::Value val)
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
static bool windowFits(int64_t offset, int64_t length, size_t byteLength)
{
    if (offset < 0 || length < 0) return false;
    const uint64_t cap = static_cast<uint64_t>(byteLength);
    return static_cast<uint64_t>(offset) <= cap &&
           static_cast<uint64_t>(length) <= cap - static_cast<uint64_t>(offset);
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

    // The option reads above can run user code (valueOf) and allocate, so
    // the buffer's address is taken again, and its bounds rechecked, here.
    info = getBufferOrTypedArrayInfo(a[1]);
    if (!info || !windowFits(offset, length, info->byteLength))
        return ev::throwRangeError("readSync: buffer changed size during the call");
    base = info->data;

    std::lock_guard<std::mutex> lock(g_fdMutex);
    OpenFile* file = lookupFd(fd);
    if (!file) return throwErrno("read", nullptr, "EBADF", "EBADF: bad file descriptor, read");

    file->stream.clear();
    if (seek) file->stream.seekg(static_cast<std::streamoff>(position), std::ios::beg);
    if (!file->stream) {
        return throwErrno("read", file->path.c_str(), "EINVAL", "EINVAL: invalid position, read");
    }

    file->stream.read(reinterpret_cast<char*>(base + offset),
                      static_cast<std::streamsize>(length));
    std::streamsize got = file->stream.gcount();
    file->stream.clear();
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
    if (seek) file->stream.seekp(static_cast<std::streamoff>(position), std::ios::beg);
    file->stream.write(reinterpret_cast<const char*>(base + offset),
                       static_cast<std::streamsize>(length));
    if (!file->stream) {
        return throwErrno("write", file->path.c_str(), "EIO", "EIO: write failed");
    }
    return ev::fromDouble(static_cast<double>(length));
}

// fstatSync(fd) -> { size, ... }
static bronze::Value js_fstatSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("fstatSync: fd required");
    int32_t fd = i32At(a, 0);

    std::string path;
    {
        std::lock_guard<std::mutex> lock(g_fdMutex);
        OpenFile* file = lookupFd(fd);
        if (!file) return throwErrno("fstat", nullptr, "EBADF", "EBADF: bad file descriptor, fstat");
        path = file->path;
    }

    std::error_code ec;
    auto size = fs::file_size(u8p(path), ec);
    if (ec) return throwFsError("fstat", path.c_str(), ec);

    ObjectBuilder obj;
    obj.set("size", ev::fromDouble(static_cast<double>(size)));
    obj.set("_isFile", ev::fromBool(true));
    obj.set("_isDirectory", ev::fromBool(false));
    obj.set("_isSymbolicLink", ev::fromBool(false));
    return obj.get();
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

// writeFileSync(path, data[, encoding])
static bronze::Value js_writeFileSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.size() < 2) return ev::throwTypeError("writeFileSync: path and data required");

    std::string rawPath = ev::toUtf8(a[0]);
    std::string pathStr = resolveFsPath(rawPath.c_str(), /*forCreate=*/true);

    std::string data;
    if (ev::isString(a[1])) {
        data = ev::toUtf8(a[1]);
    } else if (auto info = getBufferOrTypedArrayInfo(a[1])) {
        data.assign(reinterpret_cast<const char*>(info->data), info->byteLength);
    } else if (auto infoAb = ev::arrayBufferInfo(a[1])) {
        data.assign(reinterpret_cast<const char*>(infoAb.data), infoAb.byteLength);
    } else {
        data = ev::toUtf8(a[1]);
    }

    std::ofstream f(u8p(pathStr), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!f) {
        return throwErrno("open", pathStr.c_str(), "ENOENT",
                          ("ENOENT: no such file or directory, open '" + pathStr + "'").c_str());
    }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!f) {
        return throwErrno("write", pathStr.c_str(), "ERR_FS",
                          ("ERR_FS: write failed '" + pathStr + "'").c_str());
    }
    f.close();

    return ev::undefined();
}

// appendFileSync(path, data[, encoding])
static bronze::Value js_appendFileSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.size() < 2) return ev::throwTypeError("appendFileSync: path and data required");

    std::string rawPath = ev::toUtf8(a[0]);
    std::string pathStr = resolveFsPath(rawPath.c_str(), /*forCreate=*/true);

    std::string data;
    if (ev::isString(a[1])) {
        data = ev::toUtf8(a[1]);
    } else if (auto info = getBufferOrTypedArrayInfo(a[1])) {
        data.assign(reinterpret_cast<const char*>(info->data), info->byteLength);
    } else if (auto infoAb = ev::arrayBufferInfo(a[1])) {
        data.assign(reinterpret_cast<const char*>(infoAb.data), infoAb.byteLength);
    } else {
        data = ev::toUtf8(a[1]);
    }

    std::ofstream f(u8p(pathStr), std::ios::out | std::ios::binary | std::ios::app);
    if (!f) {
        return throwErrno("open", pathStr.c_str(), "ENOENT",
                          ("ENOENT: no such file or directory, open '" + pathStr + "'").c_str());
    }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.close();

    return ev::undefined();
}

// statSync(path)
static bronze::Value js_statSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("statSync: path required");

    std::string rawPath = ev::toUtf8(a[0]);
    std::string resolved = resolveFsPath(rawPath.c_str());

    const fs::path p = u8p(resolved);
    std::error_code ec;
    auto status = fs::status(p, ec);
    if (ec) {
        return throwFsError("stat", resolved.c_str(), ec);
    }

    auto fileSize = fs::file_size(p, ec);
    if (ec) fileSize = 0;

    auto mtime = fs::last_write_time(p, ec);
    double mtimeMs = 0;
    if (!ec) {
        auto sctp = std::chrono::time_point_cast<std::chrono::milliseconds>(
            mtime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        mtimeMs = static_cast<double>(sctp.time_since_epoch().count());
    }

    bool isFile = fs::is_regular_file(status);
    bool isDir = fs::is_directory(status);
    bool isSymlink = false;
    {
        auto lstatus = fs::symlink_status(p, ec);
        if (!ec) isSymlink = fs::is_symlink(lstatus);
    }

    int mode = 0;
#ifdef _WIN32
    DWORD attrs = GetFileAttributesW(p.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        mode = 0444;
        if (!(attrs & FILE_ATTRIBUTE_READONLY)) mode |= 0222;
        if (isDir) mode |= 0111;
    }
#else
    struct stat st;
    if (::stat(resolved.c_str(), &st) == 0) {
        mode = st.st_mode & 07777;
    }
#endif

    ObjectBuilder obj;
    obj.set("size", ev::fromDouble(static_cast<double>(fileSize)));
    obj.set("mtimeMs", ev::fromDouble(mtimeMs));
    obj.set("mode", ev::fromDouble(mode));
    obj.set("_isFile", ev::fromBool(isFile));
    obj.set("_isDirectory", ev::fromBool(isDir));
    obj.set("_isSymbolicLink", ev::fromBool(isSymlink));

    return obj.get();
}

// lstatSync(path)
static bronze::Value js_lstatSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("lstatSync: path required");

    std::string rawPath = ev::toUtf8(a[0]);
    std::string resolved = resolveFsPath(rawPath.c_str());

    const fs::path p = u8p(resolved);
    std::error_code ec;
    auto status = fs::symlink_status(p, ec);
    if (ec) {
        return throwFsError("lstat", resolved.c_str(), ec);
    }

    auto fileSize = fs::file_size(p, ec);
    if (ec) fileSize = 0;

    bool isFile = fs::is_regular_file(status);
    bool isDir = fs::is_directory(status);
    bool isSymlink = fs::is_symlink(status);

    ObjectBuilder obj;
    obj.set("size", ev::fromDouble(static_cast<double>(fileSize)));
    obj.set("mode", ev::fromDouble(0));
    obj.set("_isFile", ev::fromBool(isFile));
    obj.set("_isDirectory", ev::fromBool(isDir));
    obj.set("_isSymbolicLink", ev::fromBool(isSymlink));

    return obj.get();
}

// readdirSync(path[, options])
static bronze::Value js_readdirSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("readdirSync: path required");

    std::string rawPath = ev::toUtf8(a[0]);
    std::string resolved = resolveFsPath(rawPath.c_str());

    bool withFileTypes = false;
    if (a.size() >= 2 && ev::isObject(a[1])) {
        bronze::Value wft = ev::getProperty(a[1], "withFileTypes");
        withFileTypes = ev::isBool(wft) && ev::toBool(wft);
    }

    std::error_code ec;
    auto iter = fs::directory_iterator(u8p(resolved), ec);
    if (ec) {
        return throwFsError("scandir", resolved.c_str(), ec);
    }

    ArrayBuilder items;
    for (auto& entry : iter) {
        std::string name = u8s(entry.path().filename());
        if (withFileTypes) {
            ObjectBuilder dirent;
            dirent.set("name", ev::fromUtf8(name));
            dirent.set("_isFile", ev::fromBool(entry.is_regular_file()));
            dirent.set("_isDirectory", ev::fromBool(entry.is_directory()));
            dirent.set("_isSymbolicLink", ev::fromBool(entry.is_symlink()));
            items.push(dirent.get());
        } else {
            items.push(ev::fromUtf8(name));
        }
    }

    return items.get();
}

// existsSync(path)
static bronze::Value js_existsSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::fromBool(false);
    std::string rawPath = ev::toUtf8(a[0]);
    std::string resolved = resolveFsPath(rawPath.c_str());
    std::error_code ec;
    return ev::fromBool(fs::exists(u8p(resolved), ec));
}

// mkdirSync(path[, options])
static bronze::Value js_mkdirSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("mkdirSync: path required");

    std::string rawPath = ev::toUtf8(a[0]);
    std::string resolved = resolveFsPath(rawPath.c_str(), /*forCreate=*/true);

    bool recursive = false;
    if (a.size() >= 2 && ev::isObject(a[1])) {
        bronze::Value rec = ev::getProperty(a[1], "recursive");
        recursive = ev::isBool(rec) && ev::toBool(rec);
    }

    std::error_code ec;
    if (recursive) {
        fs::create_directories(u8p(resolved), ec);
    } else {
        fs::create_directory(u8p(resolved), ec);
    }

    if (ec) {
        return throwFsError("mkdir", resolved.c_str(), ec);
    }

    return ev::undefined();
}

// rmdirSync(path)
static bronze::Value js_rmdirSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("rmdirSync: path required");

    std::string rawPath = ev::toUtf8(a[0]);
    std::string resolved = resolveFsPath(rawPath.c_str());

    std::error_code ec;
    fs::remove(u8p(resolved), ec);
    if (ec) {
        return throwFsError("rmdir", resolved.c_str(), ec);
    }
    return ev::undefined();
}

// rmSync(path[, options])
static bronze::Value js_rmSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("rmSync: path required");

    std::string rawPath = ev::toUtf8(a[0]);
    std::string resolved = resolveFsPath(rawPath.c_str());

    bool recursive = false;
    bool force = false;
    if (a.size() >= 2 && ev::isObject(a[1])) {
        bronze::Value rec = ev::getProperty(a[1], "recursive");
        recursive = ev::isBool(rec) && ev::toBool(rec);
        bronze::Value f = ev::getProperty(a[1], "force");
        force = ev::isBool(f) && ev::toBool(f);
    }

    std::error_code ec;
    const fs::path p = u8p(resolved);
    if (!fs::exists(p, ec) && force) {
        return ev::undefined();
    }

    if (recursive) {
        fs::remove_all(p, ec);
    } else {
        fs::remove(p, ec);
    }

    if (ec && !force) {
        return throwFsError("rm", resolved.c_str(), ec);
    }
    return ev::undefined();
}

// unlinkSync(path)
static bronze::Value js_unlinkSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("unlinkSync: path required");

    std::string rawPath = ev::toUtf8(a[0]);
    std::string resolved = resolveFsPath(rawPath.c_str());

    std::error_code ec;
    fs::remove(u8p(resolved), ec);
    if (ec) {
        return throwFsError("unlink", resolved.c_str(), ec);
    }
    return ev::undefined();
}

// renameSync(oldPath, newPath)
static bronze::Value js_renameSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.size() < 2) return ev::throwTypeError("renameSync: oldPath and newPath required");

    std::string oldResolved = resolveFsPath(ev::toUtf8(a[0]).c_str());
    std::string newResolved = resolveFsPath(ev::toUtf8(a[1]).c_str(), /*forCreate=*/true);

    std::error_code ec;
    fs::rename(u8p(oldResolved), u8p(newResolved), ec);
    if (ec) {
        return throwFsError("rename", oldResolved.c_str(), ec);
    }
    return ev::undefined();
}

// copyFileSync(src, dest)
static bronze::Value js_copyFileSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.size() < 2) return ev::throwTypeError("copyFileSync: src and dest required");

    std::string srcResolved = resolveFsPath(ev::toUtf8(a[0]).c_str());
    std::string destResolved = resolveFsPath(ev::toUtf8(a[1]).c_str(), /*forCreate=*/true);

    std::error_code ec;
    fs::copy_file(u8p(srcResolved), u8p(destResolved), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return throwFsError("copyfile", srcResolved.c_str(), ec);
    }
    return ev::undefined();
}

// chmodSync(path, mode)
static bronze::Value js_chmodSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.size() < 2) return ev::throwTypeError("chmodSync: path and mode required");

    std::string resolved = resolveFsPath(ev::toUtf8(a[0]).c_str());
    int mode = i32At(a, 1);

#ifdef _WIN32
    int wmode = 0;
    if (mode & 0444) wmode |= 0x100;
    if (mode & 0222) wmode |= 0x080;
    int result = _wchmod(u8p(resolved).c_str(), wmode);
#else
    int result = chmod(resolved.c_str(), static_cast<mode_t>(mode));
#endif

    if (result != 0) {
        return throwErrno("chmod", resolved.c_str(), "ENOENT",
                          ("ENOENT: no such file or directory, chmod '" + resolved + "'").c_str());
    }
    return ev::undefined();
}

// realpathSync(path)
static bronze::Value js_realpathSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("realpathSync: path required");

    std::string resolved = resolveFsPath(ev::toUtf8(a[0]).c_str());
    std::error_code ec;
    auto canonical = fs::canonical(u8p(resolved), ec);
    if (ec) {
        return throwFsError("realpath", resolved.c_str(), ec);
    }

    return ev::fromUtf8(u8s(canonical));
}

void installFS()
{
    ev::setGlobalFunction("__brokit_fs_readFileSync", 2, js_readFileSync);
    ev::setGlobalFunction("__brokit_fs_writeFileSync", 3, js_writeFileSync);
    ev::setGlobalFunction("__brokit_fs_appendFileSync", 3, js_appendFileSync);
    ev::setGlobalFunction("__brokit_fs_statSync", 2, js_statSync);
    ev::setGlobalFunction("__brokit_fs_lstatSync", 2, js_lstatSync);
    ev::setGlobalFunction("__brokit_fs_readdirSync", 2, js_readdirSync);
    ev::setGlobalFunction("__brokit_fs_existsSync", 1, js_existsSync);
    ev::setGlobalFunction("__brokit_fs_mkdirSync", 2, js_mkdirSync);
    ev::setGlobalFunction("__brokit_fs_rmdirSync", 1, js_rmdirSync);
    ev::setGlobalFunction("__brokit_fs_rmSync", 2, js_rmSync);
    ev::setGlobalFunction("__brokit_fs_unlinkSync", 1, js_unlinkSync);
    ev::setGlobalFunction("__brokit_fs_renameSync", 2, js_renameSync);
    ev::setGlobalFunction("__brokit_fs_copyFileSync", 2, js_copyFileSync);
    ev::setGlobalFunction("__brokit_fs_chmodSync", 2, js_chmodSync);
    ev::setGlobalFunction("__brokit_fs_realpathSync", 1, js_realpathSync);
    ev::setGlobalFunction("__brokit_fs_openSync", 2, js_openSync);
    ev::setGlobalFunction("__brokit_fs_readSync", 5, js_readSync);
    ev::setGlobalFunction("__brokit_fs_writeSync", 5, js_writeSync);
    ev::setGlobalFunction("__brokit_fs_fstatSync", 1, js_fstatSync);
    ev::setGlobalFunction("__brokit_fs_closeSync", 1, js_closeSync);

    bronze::embed::runEntry(bronze_fs_main);
}

} // namespace brokit::api
