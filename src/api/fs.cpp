#include "api/api.h"
#include "api/arg_reader.h"
#include "api/fs_async.h"
#include "api/object_builder.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <cerrno>
#include <fcntl.h>
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

const char* fsErrorCode(const std::error_code& ec)
{
    if (ec == std::errc::no_such_file_or_directory) return "ENOENT";
    if (ec == std::errc::file_exists) return "EEXIST";
    if (ec == std::errc::permission_denied) return "EACCES";
    if (ec == std::errc::operation_not_permitted) return "EPERM";
    if (ec == std::errc::is_a_directory) return "EISDIR";
    if (ec == std::errc::not_a_directory) return "ENOTDIR";
    if (ec == std::errc::directory_not_empty) return "ENOTEMPTY";
    if (ec == std::errc::no_space_on_device) return "ENOSPC";
    if (ec == std::errc::too_many_files_open) return "EMFILE";
    if (ec == std::errc::cross_device_link) return "EXDEV";
    if (ec == std::errc::bad_file_descriptor) return "EBADF";
    if (ec == std::errc::io_error) return "EIO";
    if (ec == std::errc::read_only_file_system) return "EROFS";
    if (ec == std::errc::invalid_argument) return "EINVAL";
    return "ERR_FS";
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

bronze::Value makeU8(std::span<const uint8_t> bytes)
{
    bronze::Value v = ev::createTypedArray(elements::Uint8, static_cast<uint32_t>(bytes.size()));
    ev::fillTypedArray(v, bytes);
    return v;
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

} // namespace

bronze::Value throwFsError(const char* syscall, const char* path, const std::error_code& ec)
{
    const char* code = fsErrorCode(ec);

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

std::string resolveAssetPath(const std::string& path)
{
    return resolveFsPath(path.c_str(), false);
}

std::string resolveFsPathOnThread(const std::string& path, bool forCreate)
{
    return resolveFsPath(path.c_str(), forCreate);
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

std::string makeTempDir(const std::string& resolvedPrefix, std::error_code& ec)
{
    static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<int> pick(0, static_cast<int>(sizeof(kChars)) - 2);
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::string suffix(6, ' ');
        for (char& c : suffix) c = kChars[pick(rng)];
        const std::string full = resolvedPrefix + suffix;
#ifdef _WIN32
        ec.clear();
        bool created = fs::create_directory(u8p(full), ec);
        if (ec) return {};
        if (created) return suffix;
#else
        if (::mkdir(full.c_str(), 0700) == 0) {
            ec.clear();
            return suffix;
        }
        if (errno != EEXIST) {
            ec = std::error_code(errno, std::generic_category());
            return {};
        }
#endif
    }
    ec = std::make_error_code(std::errc::file_exists);
    return {};
}

// mkdtempSync(prefix) -> the directory made
static bronze::Value js_mkdtempSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty() || !ev::isString(a[0])) return ev::throwTypeError("mkdtempSync: prefix must be a string");
    const std::string prefix = ev::toUtf8(a[0]);
    const std::string resolved = resolveFsPath(prefix.c_str(), /*forCreate=*/true);
    std::error_code ec;
    const std::string suffix = makeTempDir(resolved, ec);
    if (ec) return throwFsError("mkdtemp", (prefix + "XXXXXX").c_str(), ec);
    return ev::fromUtf8(prefix + suffix);
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

    StatFields sf;
    std::error_code ec;
    if (!statPathFields(resolved, /*follow=*/true, sf, ec)) {
        return throwFsError("stat", resolved.c_str(), ec);
    }
    return statFieldsToJs(sf);
}

// lstatSync(path)
static bronze::Value js_lstatSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("lstatSync: path required");

    std::string rawPath = ev::toUtf8(a[0]);
    std::string resolved = resolveFsPath(rawPath.c_str());

    StatFields sf;
    std::error_code ec;
    if (!statPathFields(resolved, /*follow=*/false, sf, ec)) {
        return throwFsError("lstat", resolved.c_str(), ec);
    }
    return statFieldsToJs(sf);
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
    if (!fs::exists(fs::symlink_status(p, ec)) && force) {
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

// utimesSync(path, atimeSec, mtimeSec[, follow])
static bronze::Value js_utimesSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.size() < 3) return ev::throwTypeError("utimesSync: path, atime, and mtime required");

    std::string resolved = resolveFsPath(ev::toUtf8(a[0]).c_str());
    const double atimeSec = ev::toDouble(a[1]);
    const double mtimeSec = ev::toDouble(a[2]);
    const bool follow = !(a.size() >= 4 && ev::isBool(a[3]) && !ev::toBool(a[3]));
    const char* syscall = follow ? "utime" : "lutime";
    if (!std::isfinite(atimeSec) || !std::isfinite(mtimeSec)) {
        return ev::throwTypeError("utimesSync: atime and mtime must be finite numbers");
    }

    std::error_code ec = utimesPath(resolved, atimeSec, mtimeSec, follow);
    if (ec) return throwFsError(syscall, resolved.c_str(), ec);
    return ev::undefined();
}

// truncateSync(path[, len])
static bronze::Value js_truncateSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("truncateSync: path required");

    std::string resolved = resolveFsPath(ev::toUtf8(a[0]).c_str());
    int64_t len = (a.size() >= 2 && !ev::isUndefined(a[1]) && !ev::isNull(a[1]))
                      ? saturateI64(ev::toDouble(a[1]))
                      : 0;
    if (len < 0) {
        return throwFsError("truncate", resolved.c_str(), std::make_error_code(std::errc::invalid_argument));
    }

    std::error_code ec;
    fs::resize_file(u8p(resolved), static_cast<uintmax_t>(len), ec);
    if (ec) return throwFsError("truncate", resolved.c_str(), ec);
    return ev::undefined();
}

// symlinkSync(target, path[, type])
static bronze::Value js_symlinkSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.size() < 2) return ev::throwTypeError("symlinkSync: target and path required");

    std::string target = ev::toUtf8(a[0]);
    std::string resolvedPath = resolveFsPath(ev::toUtf8(a[1]).c_str(), /*forCreate=*/true);
    std::string type = (a.size() >= 3 && ev::isString(a[2])) ? ev::toUtf8(a[2]) : "";

    bool isDir = (type == "dir" || type == "junction");
    if (!isDir && type != "file") {
        std::error_code targetEc;
        isDir = fs::is_directory(u8p(resolveFsPath(target.c_str())), targetEc);
    }

    std::error_code ec;
    if (isDir) {
        fs::create_directory_symlink(u8p(target), u8p(resolvedPath), ec);
    } else {
        fs::create_symlink(u8p(target), u8p(resolvedPath), ec);
    }
    if (ec) return throwFsError("symlink", resolvedPath.c_str(), ec);
    return ev::undefined();
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
    ev::setGlobalFunction("__brokit_fs_mkdtempSync", 1, js_mkdtempSync);
    ev::setGlobalFunction("__brokit_fs_utimesSync", 4, js_utimesSync);
    ev::setGlobalFunction("__brokit_fs_truncateSync", 2, js_truncateSync);
    ev::setGlobalFunction("__brokit_fs_symlinkSync", 3, js_symlinkSync);
    installFSFd();
    installFSAsync();

    bronze::embed::runEntry(bronze_fs_main);
}

} // namespace brokit::api
