#include "api/api.h"
#include "runtime/runtime.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#else
#include <unistd.h>
#include <sys/utsname.h>
#include <pwd.h>
#endif

namespace brokit::api {

static JSValue js_os_platform(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
#ifdef _WIN32
    return JS_NewString(ctx, "win32");
#elif defined(__linux__)
    return JS_NewString(ctx, "linux");
#elif defined(__APPLE__)
    return JS_NewString(ctx, "darwin");
#else
    return JS_NewString(ctx, "unknown");
#endif
}

static JSValue js_os_type(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
#ifdef _WIN32
    return JS_NewString(ctx, "Windows_NT");
#elif defined(__linux__)
    return JS_NewString(ctx, "Linux");
#elif defined(__APPLE__)
    return JS_NewString(ctx, "Darwin");
#else
    return JS_NewString(ctx, "Unknown");
#endif
}

static JSValue js_os_arch(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
#if defined(_M_X64) || defined(__x86_64__)
    return JS_NewString(ctx, "x64");
#elif defined(_M_ARM64) || defined(__aarch64__)
    return JS_NewString(ctx, "arm64");
#elif defined(_M_IX86) || defined(__i386__)
    return JS_NewString(ctx, "ia32");
#elif defined(_M_ARM) || defined(__arm__)
    return JS_NewString(ctx, "arm");
#else
    return JS_NewString(ctx, "unknown");
#endif
}

static JSValue js_os_homedir(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
#ifdef _WIN32
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, path))) {
        return JS_NewString(ctx, path);
    }
    // Fallback
    const char* home = getenv("USERPROFILE");
    if (home) return JS_NewString(ctx, home);
    return JS_NewString(ctx, "");
#else
    const char* home = getenv("HOME");
    if (home) return JS_NewString(ctx, home);
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_dir) return JS_NewString(ctx, pw->pw_dir);
    return JS_NewString(ctx, "");
#endif
}

static JSValue js_os_tmpdir(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, path);
    if (len > 0) {
        // Remove trailing backslash
        if (len > 1 && path[len - 1] == '\\') path[len - 1] = '\0';
        return JS_NewString(ctx, path);
    }
    return JS_NewString(ctx, "C:\\Temp");
#else
    const char* tmp = getenv("TMPDIR");
    if (tmp) return JS_NewString(ctx, tmp);
    return JS_NewString(ctx, "/tmp");
#endif
}

static JSValue js_os_hostname(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
#ifdef _WIN32
    char buf[256];
    DWORD size = sizeof(buf);
    if (GetComputerNameA(buf, &size)) return JS_NewString(ctx, buf);
    return JS_NewString(ctx, "");
#else
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) return JS_NewString(ctx, buf);
    return JS_NewString(ctx, "");
#endif
}

void installOS(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue os = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, os, "platform",
                      JS_NewCFunction(ctx, js_os_platform, "platform", 0));
    JS_SetPropertyStr(ctx, os, "type",
                      JS_NewCFunction(ctx, js_os_type, "type", 0));
    JS_SetPropertyStr(ctx, os, "arch",
                      JS_NewCFunction(ctx, js_os_arch, "arch", 0));
    JS_SetPropertyStr(ctx, os, "homedir",
                      JS_NewCFunction(ctx, js_os_homedir, "homedir", 0));
    JS_SetPropertyStr(ctx, os, "tmpdir",
                      JS_NewCFunction(ctx, js_os_tmpdir, "tmpdir", 0));
    JS_SetPropertyStr(ctx, os, "hostname",
                      JS_NewCFunction(ctx, js_os_hostname, "hostname", 0));

#ifdef _WIN32
    JS_SetPropertyStr(ctx, os, "EOL", JS_NewString(ctx, "\r\n"));
#else
    JS_SetPropertyStr(ctx, os, "EOL", JS_NewString(ctx, "\n"));
#endif

    // Install as globalThis.__brokit_os for now; apps access via require('os') or direct
    JS_SetPropertyStr(ctx, global, "__brokit_os", os);
    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
