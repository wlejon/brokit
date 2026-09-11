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

Value js_os_homedir(Value, std::span<const Value>) {
#ifdef _WIN32
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, path))) {
        return ev::fromUtf8(path);
    }
    const char* home = getenv("USERPROFILE");
    if (home) return ev::fromUtf8(home);
    return ev::fromUtf8("");
#else
    const char* home = getenv("HOME");
    if (home) return ev::fromUtf8(home);
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_dir) return ev::fromUtf8(pw->pw_dir);
    return ev::fromUtf8("");
#endif
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
