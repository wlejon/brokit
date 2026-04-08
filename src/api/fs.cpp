#include "api/api.h"
#include "runtime/runtime.h"
#include "fs.js.h"

#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#endif

namespace fs = std::filesystem;

namespace brokit::api {

// Key for the base-paths array stored on globalThis
static const char* kFsBasePathsKey = "__brokit_fs_base_paths";

// Resolve a relative path against registered base paths.
// Returns the first path that exists, or the original path if none match.
static std::string resolveFsPath(JSContext* ctx, const char* path)
{
    // Absolute paths are returned as-is
    fs::path p(path);
    if (p.is_absolute()) return path;

    // Check base paths (last added = checked first, like fetch)
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue arr = JS_GetPropertyStr(ctx, global, kFsBasePathsKey);
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
            if (fs::exists(candidate, ec)) {
                JS_FreeValue(ctx, arr);
                JS_FreeValue(ctx, global);
                return candidate.string();
            }
        }
    }
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, global);

    // No match — return original (will fail naturally)
    return path;
}

// Helper: read encoding arg (default nullptr = buffer mode)
static const char* getEncoding(JSContext* ctx, int argc, JSValueConst* argv, int idx)
{
    if (idx >= argc) return nullptr;
    // Could be string or options object with 'encoding' field
    if (JS_IsString(argv[idx])) {
        return JS_ToCString(ctx, argv[idx]);
    }
    if (JS_IsObject(argv[idx])) {
        JSValue enc = JS_GetPropertyStr(ctx, argv[idx], "encoding");
        if (JS_IsString(enc)) {
            const char* s = JS_ToCString(ctx, enc);
            JS_FreeValue(ctx, enc);
            return s;
        }
        JS_FreeValue(ctx, enc);
    }
    return nullptr;
}

// Helper: throw a Node.js-style error with code and syscall
static JSValue throwErrno(JSContext* ctx, const char* syscall, const char* path,
                          const char* code, const char* message)
{
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, message));
    JS_SetPropertyStr(ctx, err, "code", JS_NewString(ctx, code));
    JS_SetPropertyStr(ctx, err, "syscall", JS_NewString(ctx, syscall));
    if (path) JS_SetPropertyStr(ctx, err, "path", JS_NewString(ctx, path));
    return JS_Throw(ctx, err);
}

static JSValue throwFsError(JSContext* ctx, const char* syscall, const char* path,
                            const std::error_code& ec)
{
    // Map common error codes to Node.js codes
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
    return throwErrno(ctx, syscall, path, code, msg.c_str());
}

// ---------------------------------------------------------------------------
// readFileSync(path[, encoding])
// ---------------------------------------------------------------------------
static JSValue js_readFileSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "readFileSync: path required");

    const char* rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) return JS_EXCEPTION;

    std::string resolved = resolveFsPath(ctx, rawPath);
    JS_FreeCString(ctx, rawPath);

    const char* encoding = getEncoding(ctx, argc, argv, 1);

    std::ifstream f(resolved, std::ios::in | std::ios::binary);
    if (!f) {
        if (encoding) JS_FreeCString(ctx, encoding);
        return throwErrno(ctx, "open", resolved.c_str(), "ENOENT",
                          ("ENOENT: no such file or directory, open '" + resolved + "'").c_str());
    }

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string data = ss.str();
    f.close();

    JSValue result;
    if (encoding && (strcmp(encoding, "utf8") == 0 || strcmp(encoding, "utf-8") == 0)) {
        result = JS_NewStringLen(ctx, data.data(), data.size());
    } else if (encoding) {
        // For any other encoding, return as string too (best effort)
        result = JS_NewStringLen(ctx, data.data(), data.size());
    } else {
        // No encoding: return Uint8Array (Buffer-like)
        result = JS_NewUint8ArrayCopy(ctx, reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    if (encoding) JS_FreeCString(ctx, encoding);
    return result;
}

// ---------------------------------------------------------------------------
// writeFileSync(path, data[, encoding])
// ---------------------------------------------------------------------------
static JSValue js_writeFileSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "writeFileSync: path and data required");

    const char* rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) return JS_EXCEPTION;

    std::string pathStr = resolveFsPath(ctx, rawPath);
    JS_FreeCString(ctx, rawPath);

    // Get data as bytes
    std::string data;
    if (JS_IsString(argv[1])) {
        const char* str = JS_ToCString(ctx, argv[1]);
        if (!str) return JS_EXCEPTION;
        data = str;
        JS_FreeCString(ctx, str);
    } else {
        // TypedArray or ArrayBuffer
        size_t len = 0;
        uint8_t* buf = JS_GetUint8Array(ctx, &len, argv[1]);
        if (buf) {
            data.assign(reinterpret_cast<char*>(buf), len);
        } else {
            // Try ArrayBuffer
            size_t abLen = 0;
            uint8_t* abBuf = JS_GetArrayBuffer(ctx, &abLen, argv[1]);
            if (abBuf) {
                data.assign(reinterpret_cast<char*>(abBuf), abLen);
            } else {
                // Convert to string as fallback
                const char* str = JS_ToCString(ctx, argv[1]);
                if (!str) return JS_EXCEPTION;
                data = str;
                JS_FreeCString(ctx, str);
            }
        }
    }

    std::ofstream f(pathStr, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!f) {
        return throwErrno(ctx, "open", pathStr.c_str(), "ENOENT",
                          ("ENOENT: no such file or directory, open '" + pathStr + "'").c_str());
    }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!f) {
        return throwErrno(ctx, "write", pathStr.c_str(), "ERR_FS",
                          ("ERR_FS: write failed '" + pathStr + "'").c_str());
    }
    f.close();

    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// appendFileSync(path, data[, encoding])
// ---------------------------------------------------------------------------
static JSValue js_appendFileSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "appendFileSync: path and data required");

    const char* rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) return JS_EXCEPTION;

    std::string pathStr = resolveFsPath(ctx, rawPath);
    JS_FreeCString(ctx, rawPath);

    std::string data;
    if (JS_IsString(argv[1])) {
        const char* str = JS_ToCString(ctx, argv[1]);
        if (!str) return JS_EXCEPTION;
        data = str;
        JS_FreeCString(ctx, str);
    } else {
        size_t len = 0;
        uint8_t* buf = JS_GetUint8Array(ctx, &len, argv[1]);
        if (buf) {
            data.assign(reinterpret_cast<char*>(buf), len);
        } else {
            const char* str = JS_ToCString(ctx, argv[1]);
            if (!str) return JS_EXCEPTION;
            data = str;
            JS_FreeCString(ctx, str);
        }
    }

    std::ofstream f(pathStr, std::ios::out | std::ios::binary | std::ios::app);
    if (!f) {
        return throwErrno(ctx, "open", pathStr.c_str(), "ENOENT",
                          ("ENOENT: no such file or directory, open '" + pathStr + "'").c_str());
    }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.close();

    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// statSync(path) — returns {size, isFile(), isDirectory(), isSymbolicLink(), mtimeMs, atimeMs, ctimeMs, birthtimeMs, mode}
// ---------------------------------------------------------------------------
static JSValue js_statSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "statSync: path required");

    const char* rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) return JS_EXCEPTION;

    std::string resolved = resolveFsPath(ctx, rawPath);
    JS_FreeCString(ctx, rawPath);

    std::error_code ec;
    auto status = fs::status(resolved, ec);
    if (ec) {
        return throwFsError(ctx, "stat", resolved.c_str(), ec);
    }

    auto fileSize = fs::file_size(resolved, ec);
    if (ec) fileSize = 0; // directories etc.

    auto mtime = fs::last_write_time(resolved, ec);
    // Convert to ms since epoch
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
        auto lstatus = fs::symlink_status(resolved, ec);
        if (!ec) isSymlink = fs::is_symlink(lstatus);
    }

#ifdef _WIN32
    // Get file attributes for mode approximation
    DWORD attrs = GetFileAttributesA(resolved.c_str());
    int mode = 0;
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        mode = 0444; // readable
        if (!(attrs & FILE_ATTRIBUTE_READONLY)) mode |= 0222; // writable
        if (isDir) mode |= 0111; // executable for dirs
    }
#else
    struct stat st;
    int mode = 0;
    if (::stat(resolved.c_str(), &st) == 0) {
        mode = st.st_mode & 07777;
    }
#endif

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "size", JS_NewFloat64(ctx, static_cast<double>(fileSize)));
    JS_SetPropertyStr(ctx, obj, "mtimeMs", JS_NewFloat64(ctx, mtimeMs));
    JS_SetPropertyStr(ctx, obj, "mode", JS_NewInt32(ctx, mode));
    JS_SetPropertyStr(ctx, obj, "_isFile", JS_NewBool(ctx, isFile));
    JS_SetPropertyStr(ctx, obj, "_isDirectory", JS_NewBool(ctx, isDir));
    JS_SetPropertyStr(ctx, obj, "_isSymbolicLink", JS_NewBool(ctx, isSymlink));

    return obj;
}

// ---------------------------------------------------------------------------
// lstatSync(path) — like statSync but doesn't follow symlinks
// ---------------------------------------------------------------------------
static JSValue js_lstatSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "lstatSync: path required");

    const char* rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) return JS_EXCEPTION;

    std::string resolved = resolveFsPath(ctx, rawPath);
    JS_FreeCString(ctx, rawPath);

    std::error_code ec;
    auto status = fs::symlink_status(resolved, ec);
    if (ec) {
        return throwFsError(ctx, "lstat", resolved.c_str(), ec);
    }

    auto fileSize = fs::file_size(resolved, ec);
    if (ec) fileSize = 0;

    bool isFile = fs::is_regular_file(status);
    bool isDir = fs::is_directory(status);
    bool isSymlink = fs::is_symlink(status);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "size", JS_NewFloat64(ctx, static_cast<double>(fileSize)));
    JS_SetPropertyStr(ctx, obj, "mode", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, obj, "_isFile", JS_NewBool(ctx, isFile));
    JS_SetPropertyStr(ctx, obj, "_isDirectory", JS_NewBool(ctx, isDir));
    JS_SetPropertyStr(ctx, obj, "_isSymbolicLink", JS_NewBool(ctx, isSymlink));

    return obj;
}

// ---------------------------------------------------------------------------
// readdirSync(path[, options])
// ---------------------------------------------------------------------------
static JSValue js_readdirSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "readdirSync: path required");

    const char* rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) return JS_EXCEPTION;

    std::string resolved = resolveFsPath(ctx, rawPath);
    JS_FreeCString(ctx, rawPath);

    // Check for withFileTypes option
    bool withFileTypes = false;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue wft = JS_GetPropertyStr(ctx, argv[1], "withFileTypes");
        withFileTypes = JS_ToBool(ctx, wft);
        JS_FreeValue(ctx, wft);
    }

    std::error_code ec;
    auto iter = fs::directory_iterator(resolved, ec);
    if (ec) {
        return throwFsError(ctx, "scandir", resolved.c_str(), ec);
    }

    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;

    for (auto& entry : iter) {
        if (withFileTypes) {
            JSValue dirent = JS_NewObject(ctx);
            std::string name = entry.path().filename().string();
            JS_SetPropertyStr(ctx, dirent, "name", JS_NewString(ctx, name.c_str()));
            JS_SetPropertyStr(ctx, dirent, "_isFile", JS_NewBool(ctx, entry.is_regular_file()));
            JS_SetPropertyStr(ctx, dirent, "_isDirectory", JS_NewBool(ctx, entry.is_directory()));
            JS_SetPropertyStr(ctx, dirent, "_isSymbolicLink", JS_NewBool(ctx, entry.is_symlink()));
            JS_SetPropertyUint32(ctx, arr, i++, dirent);
        } else {
            std::string name = entry.path().filename().string();
            JS_SetPropertyUint32(ctx, arr, i++, JS_NewString(ctx, name.c_str()));
        }
    }

    return arr;
}

// ---------------------------------------------------------------------------
// existsSync(path)
// ---------------------------------------------------------------------------
static JSValue js_existsSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_NewBool(ctx, false);

    const char* rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) return JS_NewBool(ctx, false);

    std::string resolved = resolveFsPath(ctx, rawPath);
    JS_FreeCString(ctx, rawPath);

    std::error_code ec;
    bool exists = fs::exists(resolved, ec);

    return JS_NewBool(ctx, exists);
}

// ---------------------------------------------------------------------------
// mkdirSync(path[, options])
// ---------------------------------------------------------------------------
static JSValue js_mkdirSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "mkdirSync: path required");

    const char* rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) return JS_EXCEPTION;

    std::string resolved = resolveFsPath(ctx, rawPath);
    JS_FreeCString(ctx, rawPath);

    bool recursive = false;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue rec = JS_GetPropertyStr(ctx, argv[1], "recursive");
        recursive = JS_ToBool(ctx, rec);
        JS_FreeValue(ctx, rec);
    }

    std::error_code ec;
    if (recursive) {
        fs::create_directories(resolved, ec);
    } else {
        fs::create_directory(resolved, ec);
    }

    if (ec) {
        return throwFsError(ctx, "mkdir", resolved.c_str(), ec);
    }

    // Node returns the first directory created when recursive, or undefined
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// rmdirSync(path)
// ---------------------------------------------------------------------------
static JSValue js_rmdirSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "rmdirSync: path required");

    const char* rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) return JS_EXCEPTION;

    std::string resolved = resolveFsPath(ctx, rawPath);
    JS_FreeCString(ctx, rawPath);

    std::error_code ec;
    fs::remove(resolved, ec);
    if (ec) {
        return throwFsError(ctx, "rmdir", resolved.c_str(), ec);
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// rmSync(path[, options]) — supports {recursive, force}
// ---------------------------------------------------------------------------
static JSValue js_rmSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "rmSync: path required");

    const char* rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) return JS_EXCEPTION;

    std::string resolved = resolveFsPath(ctx, rawPath);
    JS_FreeCString(ctx, rawPath);

    bool recursive = false;
    bool force = false;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue rec = JS_GetPropertyStr(ctx, argv[1], "recursive");
        recursive = JS_ToBool(ctx, rec);
        JS_FreeValue(ctx, rec);
        JSValue f = JS_GetPropertyStr(ctx, argv[1], "force");
        force = JS_ToBool(ctx, f);
        JS_FreeValue(ctx, f);
    }

    std::error_code ec;

    // Check if path exists first for force mode
    if (!fs::exists(resolved, ec) && force) {
        return JS_UNDEFINED;
    }

    if (recursive) {
        fs::remove_all(resolved, ec);
    } else {
        fs::remove(resolved, ec);
    }

    if (ec && !force) {
        return throwFsError(ctx, "rm", resolved.c_str(), ec);
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// unlinkSync(path)
// ---------------------------------------------------------------------------
static JSValue js_unlinkSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "unlinkSync: path required");

    const char* rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) return JS_EXCEPTION;

    std::string resolved = resolveFsPath(ctx, rawPath);
    JS_FreeCString(ctx, rawPath);

    std::error_code ec;
    fs::remove(resolved, ec);
    if (ec) {
        return throwFsError(ctx, "unlink", resolved.c_str(), ec);
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// renameSync(oldPath, newPath)
// ---------------------------------------------------------------------------
static JSValue js_renameSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "renameSync: oldPath and newPath required");

    const char* rawOld = JS_ToCString(ctx, argv[0]);
    if (!rawOld) return JS_EXCEPTION;
    const char* rawNew = JS_ToCString(ctx, argv[1]);
    if (!rawNew) { JS_FreeCString(ctx, rawOld); return JS_EXCEPTION; }

    std::string resolvedOld = resolveFsPath(ctx, rawOld);
    std::string resolvedNew = resolveFsPath(ctx, rawNew);
    JS_FreeCString(ctx, rawOld);
    JS_FreeCString(ctx, rawNew);

    std::error_code ec;
    fs::rename(resolvedOld, resolvedNew, ec);

    if (ec) {
        return throwFsError(ctx, "rename", resolvedOld.c_str(), ec);
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// copyFileSync(src, dest)
// ---------------------------------------------------------------------------
static JSValue js_copyFileSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "copyFileSync: src and dest required");

    const char* rawSrc = JS_ToCString(ctx, argv[0]);
    if (!rawSrc) return JS_EXCEPTION;
    const char* rawDest = JS_ToCString(ctx, argv[1]);
    if (!rawDest) { JS_FreeCString(ctx, rawSrc); return JS_EXCEPTION; }

    std::string resolvedSrc = resolveFsPath(ctx, rawSrc);
    std::string resolvedDest = resolveFsPath(ctx, rawDest);
    JS_FreeCString(ctx, rawSrc);
    JS_FreeCString(ctx, rawDest);

    std::error_code ec;
    fs::copy_file(resolvedSrc, resolvedDest, fs::copy_options::overwrite_existing, ec);

    if (ec) {
        return throwFsError(ctx, "copyfile", resolvedSrc.c_str(), ec);
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// chmodSync(path, mode)
// ---------------------------------------------------------------------------
static JSValue js_chmodSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "chmodSync: path and mode required");

    const char* rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) return JS_EXCEPTION;

    std::string resolved = resolveFsPath(ctx, rawPath);
    JS_FreeCString(ctx, rawPath);

    int mode = 0;
    JS_ToInt32(ctx, &mode, argv[1]);

#ifdef _WIN32
    // Windows: approximate with _chmod (only supports _S_IREAD / _S_IWRITE)
    int wmode = 0;
    if (mode & 0444) wmode |= 0x100; // _S_IREAD
    if (mode & 0222) wmode |= 0x080; // _S_IWRITE
    int result = _chmod(resolved.c_str(), wmode);
#else
    int result = chmod(resolved.c_str(), static_cast<mode_t>(mode));
#endif

    if (result != 0) {
        return throwErrno(ctx, "chmod", resolved.c_str(), "ENOENT",
                          ("ENOENT: no such file or directory, chmod '" + resolved + "'").c_str());
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// realpathSync(path)
// ---------------------------------------------------------------------------
static JSValue js_realpathSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "realpathSync: path required");

    const char* rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) return JS_EXCEPTION;

    std::string fsResolved = resolveFsPath(ctx, rawPath);
    JS_FreeCString(ctx, rawPath);

    std::error_code ec;
    auto canonical = fs::canonical(fsResolved, ec);
    if (ec) {
        return throwFsError(ctx, "realpath", fsResolved.c_str(), ec);
    }

    std::string result = canonical.string();
    return JS_NewString(ctx, result.c_str());
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------
void installFS(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

    // Initialize base-paths array for relative path resolution
    JS_SetPropertyStr(ctx, global, kFsBasePathsKey, JS_NewArray(ctx));

    // Native sync functions as __brokit_fs_* globals
    JS_SetPropertyStr(ctx, global, "__brokit_fs_readFileSync",
                      JS_NewCFunction(ctx, js_readFileSync, "readFileSync", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_fs_writeFileSync",
                      JS_NewCFunction(ctx, js_writeFileSync, "writeFileSync", 3));
    JS_SetPropertyStr(ctx, global, "__brokit_fs_appendFileSync",
                      JS_NewCFunction(ctx, js_appendFileSync, "appendFileSync", 3));
    JS_SetPropertyStr(ctx, global, "__brokit_fs_statSync",
                      JS_NewCFunction(ctx, js_statSync, "statSync", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_fs_lstatSync",
                      JS_NewCFunction(ctx, js_lstatSync, "lstatSync", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_fs_readdirSync",
                      JS_NewCFunction(ctx, js_readdirSync, "readdirSync", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_fs_existsSync",
                      JS_NewCFunction(ctx, js_existsSync, "existsSync", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_fs_mkdirSync",
                      JS_NewCFunction(ctx, js_mkdirSync, "mkdirSync", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_fs_rmdirSync",
                      JS_NewCFunction(ctx, js_rmdirSync, "rmdirSync", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_fs_rmSync",
                      JS_NewCFunction(ctx, js_rmSync, "rmSync", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_fs_unlinkSync",
                      JS_NewCFunction(ctx, js_unlinkSync, "unlinkSync", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_fs_renameSync",
                      JS_NewCFunction(ctx, js_renameSync, "renameSync", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_fs_copyFileSync",
                      JS_NewCFunction(ctx, js_copyFileSync, "copyFileSync", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_fs_chmodSync",
                      JS_NewCFunction(ctx, js_chmodSync, "chmodSync", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_fs_realpathSync",
                      JS_NewCFunction(ctx, js_realpathSync, "realpathSync", 1));

    JS_FreeValue(ctx, global);

    // Install JS polyfill (async wrappers + fs object)
    JSValue r = JS_Eval(ctx, js_fs, strlen(js_fs), "<fs>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

void addFsBasePath(JSContext* ctx, const std::string& path)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue arr = JS_GetPropertyStr(ctx, global, kFsBasePathsKey);
    if (!JS_IsArray(arr)) {
        JS_FreeValue(ctx, arr);
        arr = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, kFsBasePathsKey, JS_DupValue(ctx, arr));
    }
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);
    JS_SetPropertyUint32(ctx, arr, len, JS_NewString(ctx, path.c_str()));
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
