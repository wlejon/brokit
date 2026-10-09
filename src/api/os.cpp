#include "api/api.h"
#include "api/object_builder.h"
#include "api/arg_reader.h"

#include <cstring>
#include <string>

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

namespace {

Value js_os_platform(Value, std::span<const Value>) {
#ifdef _WIN32
    return ev::fromUtf8("win32");
#elif defined(__linux__)
    return ev::fromUtf8("linux");
#elif defined(__APPLE__)
    return ev::fromUtf8("darwin");
#else
    return ev::fromUtf8("unknown");
#endif
}

Value js_os_type(Value, std::span<const Value>) {
#ifdef _WIN32
    return ev::fromUtf8("Windows_NT");
#elif defined(__linux__)
    return ev::fromUtf8("Linux");
#elif defined(__APPLE__)
    return ev::fromUtf8("Darwin");
#else
    return ev::fromUtf8("Unknown");
#endif
}

Value js_os_arch(Value, std::span<const Value>) {
#if defined(_M_X64) || defined(__x86_64__)
    return ev::fromUtf8("x64");
#elif defined(_M_ARM64) || defined(__aarch64__)
    return ev::fromUtf8("arm64");
#elif defined(_M_IX86) || defined(__i386__)
    return ev::fromUtf8("ia32");
#elif defined(_M_ARM) || defined(__arm__)
    return ev::fromUtf8("arm");
#else
    return ev::fromUtf8("unknown");
#endif
}

#ifdef _WIN32
std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size_t(len > 0 ? len - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}
#endif

// The user's home directory, UTF-8: $HOME first (as Node), else the profile /
// passwd entry. On Windows the wide API, since the profile path holds the
// user's name in whatever script it is written in.
std::string homeDir() {
#ifdef _WIN32
    if (const wchar_t* home = _wgetenv(L"USERPROFILE"); home && *home) return narrow(home);
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, path))) return narrow(path);
    return {};
#else
    const char* home = getenv("HOME");
    if (home && *home) return home;
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_dir) return pw->pw_dir;
    return {};
#endif
}

Value js_os_homedir(Value, std::span<const Value>) {
    return ev::fromUtf8(homeDir());
}

// os.userInfo(): Node's shape, { uid, gid, username, homedir, shell }. On
// Windows uid and gid are -1 and shell is null, as in Node; elsewhere shell is
// the passwd entry's login shell (null when it names none).
Value js_os_userInfo(Value, std::span<const Value>) {
    ObjectBuilder o;
#ifdef _WIN32
    wchar_t name[256];
    DWORD n = 256;
    std::string user = GetUserNameW(name, &n) ? narrow(name) : std::string();
    o.set("uid", ev::fromDouble(-1));
    o.set("gid", ev::fromDouble(-1));
    o.set("username", ev::fromUtf8(user));
    o.set("homedir", ev::fromUtf8(homeDir()));
    o.set("shell", ev::null());
#else
    struct passwd* pw = getpwuid(getuid());
    o.set("uid", ev::fromDouble(double(getuid())));
    o.set("gid", ev::fromDouble(double(getgid())));
    o.set("username", ev::fromUtf8(pw && pw->pw_name ? pw->pw_name : ""));
    o.set("homedir", ev::fromUtf8(pw && pw->pw_dir ? std::string(pw->pw_dir) : homeDir()));
    o.set("shell", pw && pw->pw_shell && *pw->pw_shell ? ev::fromUtf8(pw->pw_shell) : ev::null());
#endif
    return o.get();
}

Value js_os_tmpdir(Value, std::span<const Value>) {
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, path);
    if (len > 0) {
        if (len > 1 && path[len - 1] == '\\') path[len - 1] = '\0';
        return ev::fromUtf8(path);
    }
    return ev::fromUtf8("C:\\Temp");
#else
    const char* tmp = getenv("TMPDIR");
    if (tmp) return ev::fromUtf8(tmp);
    return ev::fromUtf8("/tmp");
#endif
}

Value js_os_hostname(Value, std::span<const Value>) {
#ifdef _WIN32
    char buf[256];
    DWORD size = sizeof(buf);
    if (GetComputerNameA(buf, &size)) return ev::fromUtf8(buf);
    return ev::fromUtf8("");
#else
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) return ev::fromUtf8(buf);
    return ev::fromUtf8("");
#endif
}

} // namespace

void installOS() {
    ObjectBuilder os;

    os.def("platform", 0, js_os_platform);
    os.def("type", 0, js_os_type);
    os.def("arch", 0, js_os_arch);
    os.def("homedir", 0, js_os_homedir);
    os.def("userInfo", 1, js_os_userInfo);
    os.def("tmpdir", 0, js_os_tmpdir);
    os.def("hostname", 0, js_os_hostname);

#ifdef _WIN32
    os.set("EOL", ev::fromUtf8("\r\n"));
#else
    os.set("EOL", ev::fromUtf8("\n"));
#endif

    ev::setGlobalValue("__brokit_os", os.get());
    ev::setGlobalValue("os", os.get());
}

} // namespace brokit::api
