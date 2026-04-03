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

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    const char* encoding = getEncoding(ctx, argc, argv, 1);

    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f) {
        std::string p(path);
        JS_FreeCString(ctx, path);
        if (encoding) JS_FreeCString(ctx, encoding);
        return throwErrno(ctx, "open", p.c_str(), "ENOENT",
                          ("ENOENT: no such file or directory, open '" + p + "'").c_str());
    }

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string data = ss.str();
    f.close();
    JS_FreeCString(ctx, path);

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

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    std::string pathStr(path);
    JS_FreeCString(ctx, path);

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

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    std::string pathStr(path);
    JS_FreeCString(ctx, path);

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

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    std::error_code ec;
    auto status = fs::status(path, ec);
    if (ec) {
        std::string p(path);
        JS_FreeCString(ctx, path);
        return throwFsError(ctx, "stat", p.c_str(), ec);
    }

    auto fileSize = fs::file_size(path, ec);
    if (ec) fileSize = 0; // directories etc.

    auto mtime = fs::last_write_time(path, ec);
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
        auto lstatus = fs::symlink_status(path, ec);
        if (!ec) isSymlink = fs::is_symlink(lstatus);
    }

#ifdef _WIN32
    // Get file attributes for mode approximation
    DWORD attrs = GetFileAttributesA(path);
    int mode = 0;
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        mode = 0444; // readable
        if (!(attrs & FILE_ATTRIBUTE_READONLY)) mode |= 0222; // writable
        if (isDir) mode |= 0111; // executable for dirs
    }
#else
    struct stat st;
    int mode = 0;
    if (::stat(path, &st) == 0) {
        mode = st.st_mode & 07777;
    }
#endif

    JS_FreeCString(ctx, path);

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

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    std::error_code ec;
    auto status = fs::symlink_status(path, ec);
    if (ec) {
        std::string p(path);
        JS_FreeCString(ctx, path);
        return throwFsError(ctx, "lstat", p.c_str(), ec);
    }

    auto fileSize = fs::file_size(path, ec);
    if (ec) fileSize = 0;

    bool isFile = fs::is_regular_file(status);
    bool isDir = fs::is_directory(status);
    bool isSymlink = fs::is_symlink(status);

    JS_FreeCString(ctx, path);

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

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    // Check for withFileTypes option
    bool withFileTypes = false;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue wft = JS_GetPropertyStr(ctx, argv[1], "withFileTypes");
        withFileTypes = JS_ToBool(ctx, wft);
        JS_FreeValue(ctx, wft);
    }

    std::error_code ec;
    auto iter = fs::directory_iterator(path, ec);
    if (ec) {
        std::string p(path);
        JS_FreeCString(ctx, path);
        return throwFsError(ctx, "scandir", p.c_str(), ec);
    }

    JS_FreeCString(ctx, path);

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

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_NewBool(ctx, false);

    std::error_code ec;
    bool exists = fs::exists(path, ec);
    JS_FreeCString(ctx, path);

    return JS_NewBool(ctx, exists);
}

// ---------------------------------------------------------------------------
// mkdirSync(path[, options])
// ---------------------------------------------------------------------------
static JSValue js_mkdirSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "mkdirSync: path required");

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    bool recursive = false;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue rec = JS_GetPropertyStr(ctx, argv[1], "recursive");
        recursive = JS_ToBool(ctx, rec);
        JS_FreeValue(ctx, rec);
    }

    std::error_code ec;
    if (recursive) {
        fs::create_directories(path, ec);
    } else {
        fs::create_directory(path, ec);
    }

    if (ec) {
        std::string p(path);
        JS_FreeCString(ctx, path);
        return throwFsError(ctx, "mkdir", p.c_str(), ec);
    }

    JS_FreeCString(ctx, path);

    // Node returns the first directory created when recursive, or undefined
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// rmdirSync(path)
// ---------------------------------------------------------------------------
static JSValue js_rmdirSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "rmdirSync: path required");

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        std::string p(path);
        JS_FreeCString(ctx, path);
        return throwFsError(ctx, "rmdir", p.c_str(), ec);
    }

    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// rmSync(path[, options]) — supports {recursive, force}
// ---------------------------------------------------------------------------
static JSValue js_rmSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "rmSync: path required");

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

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
    if (!fs::exists(path, ec) && force) {
        JS_FreeCString(ctx, path);
        return JS_UNDEFINED;
    }

    if (recursive) {
        fs::remove_all(path, ec);
    } else {
        fs::remove(path, ec);
    }

    if (ec && !force) {
        std::string p(path);
        JS_FreeCString(ctx, path);
        return throwFsError(ctx, "rm", p.c_str(), ec);
    }

    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// unlinkSync(path)
// ---------------------------------------------------------------------------
static JSValue js_unlinkSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "unlinkSync: path required");

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        std::string p(path);
        JS_FreeCString(ctx, path);
        return throwFsError(ctx, "unlink", p.c_str(), ec);
    }

    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// renameSync(oldPath, newPath)
// ---------------------------------------------------------------------------
static JSValue js_renameSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "renameSync: oldPath and newPath required");

    const char* oldPath = JS_ToCString(ctx, argv[0]);
    if (!oldPath) return JS_EXCEPTION;
    const char* newPath = JS_ToCString(ctx, argv[1]);
    if (!newPath) { JS_FreeCString(ctx, oldPath); return JS_EXCEPTION; }

    std::error_code ec;
    fs::rename(oldPath, newPath, ec);

    if (ec) {
        std::string op(oldPath);
        JS_FreeCString(ctx, oldPath);
        JS_FreeCString(ctx, newPath);
        return throwFsError(ctx, "rename", op.c_str(), ec);
    }

    JS_FreeCString(ctx, oldPath);
    JS_FreeCString(ctx, newPath);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// copyFileSync(src, dest)
// ---------------------------------------------------------------------------
static JSValue js_copyFileSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "copyFileSync: src and dest required");

    const char* src = JS_ToCString(ctx, argv[0]);
    if (!src) return JS_EXCEPTION;
    const char* dest = JS_ToCString(ctx, argv[1]);
    if (!dest) { JS_FreeCString(ctx, src); return JS_EXCEPTION; }

    std::error_code ec;
    fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);

    if (ec) {
        std::string s(src);
        JS_FreeCString(ctx, src);
        JS_FreeCString(ctx, dest);
        return throwFsError(ctx, "copyfile", s.c_str(), ec);
    }

    JS_FreeCString(ctx, src);
    JS_FreeCString(ctx, dest);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// chmodSync(path, mode)
// ---------------------------------------------------------------------------
static JSValue js_chmodSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "chmodSync: path and mode required");

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    int mode = 0;
    JS_ToInt32(ctx, &mode, argv[1]);

#ifdef _WIN32
    // Windows: approximate with _chmod (only supports _S_IREAD / _S_IWRITE)
    int wmode = 0;
    if (mode & 0444) wmode |= 0x100; // _S_IREAD
    if (mode & 0222) wmode |= 0x080; // _S_IWRITE
    int result = _chmod(path, wmode);
#else
    int result = chmod(path, static_cast<mode_t>(mode));
#endif

    if (result != 0) {
        std::string p(path);
        JS_FreeCString(ctx, path);
        return throwErrno(ctx, "chmod", p.c_str(), "ENOENT",
                          ("ENOENT: no such file or directory, chmod '" + p + "'").c_str());
    }

    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// realpathSync(path)
// ---------------------------------------------------------------------------
static JSValue js_realpathSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "realpathSync: path required");

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    std::error_code ec;
    auto resolved = fs::canonical(path, ec);
    if (ec) {
        std::string p(path);
        JS_FreeCString(ctx, path);
        return throwFsError(ctx, "realpath", p.c_str(), ec);
    }

    JS_FreeCString(ctx, path);

    std::string result = resolved.string();
    return JS_NewString(ctx, result.c_str());
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------
void installFS(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

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

} // namespace brokit::api
