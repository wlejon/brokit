#include "api/api.h"
#include "api/object_builder.h"
#include "api/arg_reader.h"
#include "api/signals.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <fstream>
#include <map>
#include <sstream>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <net/if_dl.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <ctime>
#else
#include <netpacket/packet.h>
#include <sys/sysinfo.h>
#endif
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
std::string narrow(const wchar_t* w, int n = -1) {
    if (!w || !*w || n == 0) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(size_t(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, out.data(), len, nullptr, nullptr);
    if (n < 0 && !out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

// A REG_SZ / REG_DWORD under HKLM, or empty / 0.
std::string regString(const wchar_t* key, const wchar_t* name) {
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return narrow(buf);
}

DWORD regDword(const wchar_t* key, const wchar_t* name) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return 0;
    return v;
}

// The real version (GetVersionEx lies to an unmanifested process).
bool windowsVersion(DWORD& major, DWORD& minor, DWORD& build) {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    if (!fn) return false;
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) return false;
    major = vi.dwMajorVersion;
    minor = vi.dwMinorVersion;
    build = vi.dwBuildNumber;
    return true;
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
    wchar_t path[MAX_PATH + 1];
    DWORD len = GetTempPathW(MAX_PATH + 1, path);
    if (len > 0 && len <= MAX_PATH) {
        if (len > 3 && path[len - 1] == L'\\') path[len - 1] = L'\0';
        return ev::fromUtf8(narrow(path));
    }
    return ev::fromUtf8("C:\\Temp");
#else
    const char* tmp = getenv("TMPDIR");
    if (tmp && *tmp) {
        std::string t = tmp;
        if (t.size() > 1 && t.back() == '/') t.pop_back();
        return ev::fromUtf8(t);
    }
    return ev::fromUtf8("/tmp");
#endif
}

Value js_os_hostname(Value, std::span<const Value>) {
#ifdef _WIN32
    wchar_t buf[256];
    DWORD size = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, buf, &size)) return ev::fromUtf8(narrow(buf));
    return ev::fromUtf8("");
#else
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) {
        buf[sizeof(buf) - 1] = '\0';
        return ev::fromUtf8(buf);
    }
    return ev::fromUtf8("");
#endif
}

// ---- memory, uptime, load ----------------------------------------------------

#if defined(__linux__)
// A /proc/meminfo field, in bytes (0 when absent).
double meminfo(const char* field) {
    std::ifstream f("/proc/meminfo");
    std::string line;
    const size_t n = std::strlen(field);
    while (std::getline(f, line)) {
        if (line.compare(0, n, field) == 0 && line.size() > n && line[n] == ':') {
            return std::strtod(line.c_str() + n + 1, nullptr) * 1024.0;
        }
    }
    return 0;
}
#endif

Value js_os_totalmem(Value, std::span<const Value>) {
#ifdef _WIN32
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    return ev::fromDouble(GlobalMemoryStatusEx(&ms) ? double(ms.ullTotalPhys) : 0.0);
#elif defined(__APPLE__)
    uint64_t mem = 0;
    size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) != 0) mem = 0;
    return ev::fromDouble(double(mem));
#else
    double total = meminfo("MemTotal");
    if (total <= 0) {
        struct sysinfo si{};
        if (sysinfo(&si) == 0) total = double(si.totalram) * si.mem_unit;
    }
    return ev::fromDouble(total);
#endif
}

Value js_os_freemem(Value, std::span<const Value>) {
#ifdef _WIN32
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    return ev::fromDouble(GlobalMemoryStatusEx(&ms) ? double(ms.ullAvailPhys) : 0.0);
#elif defined(__APPLE__)
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) !=
        KERN_SUCCESS)
        return ev::fromDouble(0);
    return ev::fromDouble(double(vm.free_count + vm.inactive_count) * double(sysconf(_SC_PAGESIZE)));
#else
    // What a new allocation can have without swapping (libuv's answer).
    double avail = meminfo("MemAvailable");
    if (avail <= 0) {
        struct sysinfo si{};
        if (sysinfo(&si) == 0) avail = double(si.freeram) * si.mem_unit;
    }
    return ev::fromDouble(avail);
#endif
}

// Seconds since boot.
Value js_os_uptime(Value, std::span<const Value>) {
#ifdef _WIN32
    return ev::fromDouble(double(GetTickCount64() / 1000));
#elif defined(__APPLE__)
    struct timeval boot{};
    size_t len = sizeof(boot);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boot, &len, nullptr, 0) != 0) return ev::fromDouble(0);
    return ev::fromDouble(double(std::time(nullptr) - boot.tv_sec));
#else
    std::ifstream f("/proc/uptime");
    double up = 0;
    if (f >> up) return ev::fromDouble(up);
    struct sysinfo si{};
    return ev::fromDouble(sysinfo(&si) == 0 ? double(si.uptime) : 0.0);
#endif
}

// [1, 5, 15]-minute load averages; [0, 0, 0] on Windows, as Node.
Value js_os_loadavg(Value, std::span<const Value>) {
    double avg[3] = {0, 0, 0};
#ifndef _WIN32
    if (getloadavg(avg, 3) != 3) avg[0] = avg[1] = avg[2] = 0;
#endif
    return hostArrayOf(3, [&avg](size_t i) { return ev::fromDouble(avg[i]); });
}

// ---- CPUs --------------------------------------------------------------------

struct CpuInfo {
    std::string model;
    double speedMHz = 0;
    double user = 0, nice = 0, sys = 0, idle = 0, irq = 0;  // ms
};

std::vector<CpuInfo> readCpus() {
    std::vector<CpuInfo> cpus;
#ifdef _WIN32
    // Per-processor times from the kernel (in 100 ns units), the model and
    // clock from the registry, as libuv reads them.
    struct ProcessorPerformance {
        LARGE_INTEGER idle, kernel, user, dpc, interrupt;
        ULONG interruptCount;
    };
    using NtQueryFn = LONG(WINAPI*)(int, void*, ULONG, ULONG*);
    auto query = reinterpret_cast<NtQueryFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const DWORD count = si.dwNumberOfProcessors;
    std::vector<ProcessorPerformance> perf(count);
    ULONG got = 0;
    const bool haveTimes =
        query && query(8 /* SystemProcessorPerformanceInformation */, perf.data(),
                       ULONG(perf.size() * sizeof(ProcessorPerformance)), &got) == 0;
    const size_t timed = haveTimes ? got / sizeof(ProcessorPerformance) : 0;
    for (DWORD i = 0; i < count; ++i) {
        CpuInfo c;
        wchar_t key[128];
        swprintf(key, 128, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\%lu", static_cast<unsigned long>(i));
        c.model = regString(key, L"ProcessorNameString");
        while (!c.model.empty() && c.model.back() == ' ') c.model.pop_back();
        c.speedMHz = double(regDword(key, L"~MHz"));
        if (i < timed) {
            const ProcessorPerformance& p = perf[i];
            c.idle = double(p.idle.QuadPart) / 10000.0;
            c.sys = double(p.kernel.QuadPart - p.idle.QuadPart) / 10000.0;  // kernel time counts idle
            c.user = double(p.user.QuadPart) / 10000.0;
            c.irq = double(p.interrupt.QuadPart) / 10000.0;
        }
        cpus.push_back(std::move(c));
    }
#elif defined(__APPLE__)
    char brand[256] = {};
    size_t len = sizeof(brand);
    if (sysctlbyname("machdep.cpu.brand_string", brand, &len, nullptr, 0) != 0) brand[0] = '\0';
    uint64_t freq = 0;
    len = sizeof(freq);
    if (sysctlbyname("hw.cpufrequency", &freq, &len, nullptr, 0) != 0) freq = 0;
    natural_t n = 0;
    processor_info_array_t info = nullptr;
    mach_msg_type_number_t infoCount = 0;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &n, &info, &infoCount) == KERN_SUCCESS) {
        const double tick = 1000.0 / double(sysconf(_SC_CLK_TCK));
        for (natural_t i = 0; i < n; ++i) {
            const integer_t* t = info + i * CPU_STATE_MAX;
            CpuInfo c;
            c.model = brand;
            c.speedMHz = double(freq / 1000000);
            c.user = double(t[CPU_STATE_USER]) * tick;
            c.nice = double(t[CPU_STATE_NICE]) * tick;
            c.sys = double(t[CPU_STATE_SYSTEM]) * tick;
            c.idle = double(t[CPU_STATE_IDLE]) * tick;
            cpus.push_back(std::move(c));
        }
        vm_deallocate(mach_task_self(), vm_address_t(info), vm_size_t(infoCount) * sizeof(integer_t));
    }
#else
    // Times from /proc/stat (clock ticks), the model and clock from
    // /proc/cpuinfo, the current clock from cpufreq where the kernel has it.
    const double tick = 1000.0 / double(sysconf(_SC_CLK_TCK));
    std::ifstream stat("/proc/stat");
    std::string line;
    while (std::getline(stat, line)) {
        if (line.compare(0, 3, "cpu") != 0 || line.size() < 4 || line[3] < '0' || line[3] > '9') continue;
        std::istringstream ss(line);
        std::string name;
        unsigned long long user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0;
        ss >> name >> user >> nice >> sys >> idle >> iowait >> irq;
        CpuInfo c;
        c.user = double(user) * tick;
        c.nice = double(nice) * tick;
        c.sys = double(sys) * tick;
        c.idle = double(idle) * tick;
        c.irq = double(irq) * tick;
        cpus.push_back(std::move(c));
    }
    std::vector<std::string> models;
    std::vector<double> mhz;
    std::ifstream info("/proc/cpuinfo");
    while (std::getline(info, line)) {
        auto value = [&line]() {
            size_t colon = line.find(':');
            std::string v = colon == std::string::npos ? std::string() : line.substr(colon + 1);
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
            return v;
        };
        if (line.compare(0, 10, "model name") == 0) models.push_back(value());
        else if (line.compare(0, 7, "cpu MHz") == 0) mhz.push_back(std::strtod(value().c_str(), nullptr));
    }
    for (size_t i = 0; i < cpus.size(); ++i) {
        cpus[i].model = i < models.size() ? models[i] : (models.empty() ? std::string("unknown") : models[0]);
        char path[96];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%zu/cpufreq/scaling_cur_freq", i);
        std::ifstream freq(path);
        double khz = 0;
        if (freq >> khz && khz > 0) cpus[i].speedMHz = std::floor(khz / 1000.0);
        else if (i < mhz.size()) cpus[i].speedMHz = std::floor(mhz[i]);
    }
#endif
    return cpus;
}

Value js_os_cpus(Value, std::span<const Value>) {
    std::vector<CpuInfo> cpus = readCpus();
    return hostArrayOf(cpus.size(), [&cpus](size_t i) -> Value {
        const CpuInfo& c = cpus[i];
        ObjectBuilder times;
        times.set("user", ev::fromDouble(std::floor(c.user)));
        times.set("nice", ev::fromDouble(std::floor(c.nice)));
        times.set("sys", ev::fromDouble(std::floor(c.sys)));
        times.set("idle", ev::fromDouble(std::floor(c.idle)));
        times.set("irq", ev::fromDouble(std::floor(c.irq)));
        ObjectBuilder o;
        o.set("model", ev::fromUtf8(c.model));
        o.set("speed", ev::fromDouble(c.speedMHz));
        o.set("times", times.get());
        return o.get();
    });
}

Value js_os_availableParallelism(Value, std::span<const Value>) {
    unsigned n = std::thread::hardware_concurrency();
    return ev::fromDouble(double(n ? n : 1));
}

// ---- release, version, machine -------------------------------------------------

Value js_os_release(Value, std::span<const Value>) {
#ifdef _WIN32
    DWORD major = 0, minor = 0, build = 0;
    if (!windowsVersion(major, minor, build)) return ev::fromUtf8("");
    return ev::fromUtf8(std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(build));
#else
    struct utsname u{};
    return ev::fromUtf8(uname(&u) == 0 ? u.release : "");
#endif
}

// The kernel's version string; on Windows the product ("Windows 11 Pro").
Value js_os_version(Value, std::span<const Value>) {
#ifdef _WIN32
    std::string product =
        regString(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
    DWORD major = 0, minor = 0, build = 0;
    // Windows 11 still names itself "Windows 10" there (libuv corrects it too).
    if (windowsVersion(major, minor, build) && major == 10 && build >= 22000) {
        size_t at = product.find("Windows 10");
        if (at != std::string::npos) product.replace(at, 10, "Windows 11");
    }
    return ev::fromUtf8(product);
#else
    struct utsname u{};
    return ev::fromUtf8(uname(&u) == 0 ? u.version : "");
#endif
}

// The machine type, as `uname -m` prints it (x86_64, arm64, aarch64, ...).
Value js_os_machine(Value, std::span<const Value>) {
#ifdef _WIN32
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return ev::fromUtf8("x86_64");
        case PROCESSOR_ARCHITECTURE_ARM64: return ev::fromUtf8("arm64");
        case PROCESSOR_ARCHITECTURE_INTEL: return ev::fromUtf8("i686");
        case PROCESSOR_ARCHITECTURE_ARM: return ev::fromUtf8("arm");
        default: return ev::fromUtf8("unknown");
    }
#else
    struct utsname u{};
    return ev::fromUtf8(uname(&u) == 0 ? u.machine : "");
#endif
}

// ---- network interfaces --------------------------------------------------------

struct IfAddr {
    std::string name;
    std::string address;
    std::string netmask;
    bool v6 = false;
    std::string mac = "00:00:00:00:00:00";
    bool internal = false;
    int prefix = 0;
    uint32_t scopeid = 0;
};

std::string macString(const unsigned char* b, size_t n) {
    if (n < 6) return "00:00:00:00:00:00";
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
    return buf;
}

std::string maskString(bool v6, int prefix) {
    if (!v6) {
        uint32_t m = prefix <= 0 ? 0 : (prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1));
        in_addr a{};
        a.s_addr = htonl(m);
        char buf[INET_ADDRSTRLEN];
        return inet_ntop(AF_INET, &a, buf, sizeof(buf)) ? buf : "";
    }
    unsigned char bytes[16] = {};
    for (int i = 0; i < 16; ++i) {
        int bits = prefix - i * 8;
        bytes[i] = bits >= 8 ? 0xFF : (bits <= 0 ? 0 : static_cast<unsigned char>(0xFF << (8 - bits)));
    }
    char buf[INET6_ADDRSTRLEN];
    return inet_ntop(AF_INET6, bytes, buf, sizeof(buf)) ? buf : "";
}

int prefixOf(const unsigned char* mask, size_t n) {
    int bits = 0;
    for (size_t i = 0; i < n; ++i)
        for (int b = 7; b >= 0; --b)
            if (mask[i] & (1 << b)) ++bits;
    return bits;
}

std::vector<IfAddr> readInterfaces() {
    std::vector<IfAddr> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 4 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_UNSPEC,
                                  GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        const std::string name = narrow(a->FriendlyName);
        const bool loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const sockaddr* sa = u->Address.lpSockaddr;
            if (!sa || (sa->sa_family != AF_INET && sa->sa_family != AF_INET6)) continue;
            IfAddr e;
            e.name = name;
            e.v6 = sa->sa_family == AF_INET6;
            char text[INET6_ADDRSTRLEN] = {};
            if (e.v6) {
                auto* s6 = reinterpret_cast<const sockaddr_in6*>(sa);
                inet_ntop(AF_INET6, &s6->sin6_addr, text, sizeof(text));
                e.scopeid = s6->sin6_scope_id;
            } else {
                inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, text, sizeof(text));
            }
            e.address = text;
            e.prefix = u->OnLinkPrefixLength;
            e.netmask = maskString(e.v6, e.prefix);
            e.mac = macString(a->PhysicalAddress, a->PhysicalAddressLength);
            e.internal = loopback;
            out.push_back(std::move(e));
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    std::map<std::string, std::string> macs;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr) continue;
#if defined(__APPLE__)
        if (i->ifa_addr->sa_family == AF_LINK) {
            auto* dl = reinterpret_cast<const sockaddr_dl*>(i->ifa_addr);
            macs[i->ifa_name] =
                macString(reinterpret_cast<const unsigned char*>(LLADDR(dl)), size_t(dl->sdl_alen));
        }
#else
        if (i->ifa_addr->sa_family == AF_PACKET) {
            auto* ll = reinterpret_cast<const sockaddr_ll*>(i->ifa_addr);
            macs[i->ifa_name] = macString(ll->sll_addr, size_t(ll->sll_halen));
        }
#endif
    }
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || !(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_RUNNING)) continue;
        const int fam = i->ifa_addr->sa_family;
        if (fam != AF_INET && fam != AF_INET6) continue;
        IfAddr e;
        e.name = i->ifa_name;
        e.v6 = fam == AF_INET6;
        char text[INET6_ADDRSTRLEN] = {};
        if (e.v6) {
            auto* s6 = reinterpret_cast<const sockaddr_in6*>(i->ifa_addr);
            inet_ntop(AF_INET6, &s6->sin6_addr, text, sizeof(text));
            e.scopeid = s6->sin6_scope_id;
            if (i->ifa_netmask)
                e.prefix = prefixOf(reinterpret_cast<const sockaddr_in6*>(i->ifa_netmask)->sin6_addr.s6_addr, 16);
        } else {
            inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr, text, sizeof(text));
            if (i->ifa_netmask) {
                const auto* m = reinterpret_cast<const sockaddr_in*>(i->ifa_netmask);
                e.prefix = prefixOf(reinterpret_cast<const unsigned char*>(&m->sin_addr), 4);
            }
        }
        e.address = text;
        e.netmask = maskString(e.v6, e.prefix);
        e.internal = (i->ifa_flags & IFF_LOOPBACK) != 0;
        auto mac = macs.find(e.name);
        if (mac != macs.end()) e.mac = mac->second;
        out.push_back(std::move(e));
    }
    freeifaddrs(list);
#endif
    return out;
}

// os.networkInterfaces(): { name: [{ address, netmask, family, mac,
// internal, cidr, scopeid? }] } for the interfaces that are up, as Node.
Value js_os_networkInterfaces(Value, std::span<const Value>) {
    std::vector<IfAddr> all = readInterfaces();
    ObjectBuilder result;
    std::vector<std::string> names;
    for (const IfAddr& a : all) {
        bool seen = false;
        for (const auto& n : names) seen = seen || n == a.name;
        if (!seen) names.push_back(a.name);
    }
    for (const std::string& name : names) {
        std::vector<const IfAddr*> mine;
        for (const IfAddr& a : all)
            if (a.name == name) mine.push_back(&a);
        ev::Persistent list(hostArrayOf(mine.size(), [&mine](size_t i) -> Value {
            const IfAddr& a = *mine[i];
            ObjectBuilder o;
            o.set("address", ev::fromUtf8(a.address));
            o.set("netmask", ev::fromUtf8(a.netmask));
            o.set("family", ev::fromUtf8(a.v6 ? "IPv6" : "IPv4"));
            o.set("mac", ev::fromUtf8(a.mac));
            o.set("internal", ev::fromBool(a.internal));
            o.set("cidr", ev::fromUtf8(a.address + "/" + std::to_string(a.prefix)));
            if (a.v6) o.set("scopeid", ev::fromDouble(double(a.scopeid)));
            return o.get();
        }));
        result.set(name, list.get());
    }
    return result.get();
}

Value makeConstants() {
    ObjectBuilder signals;
    for (const SignalName& s : signalNames()) signals.set(s.name, ev::fromDouble(double(s.number)));
    ObjectBuilder constants;
    constants.set("signals", signals.get());
    return constants.get();
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
    os.def("cpus", 0, js_os_cpus);
    os.def("availableParallelism", 0, js_os_availableParallelism);
    os.def("totalmem", 0, js_os_totalmem);
    os.def("freemem", 0, js_os_freemem);
    os.def("uptime", 0, js_os_uptime);
    os.def("loadavg", 0, js_os_loadavg);
    os.def("networkInterfaces", 0, js_os_networkInterfaces);
    os.def("release", 0, js_os_release);
    os.def("version", 0, js_os_version);
    os.def("machine", 0, js_os_machine);
    os.def("endianness", 0, [](Value, std::span<const Value>) -> Value { return ev::fromUtf8("LE"); });
    {
        ev::Persistent constants(makeConstants());
        os.set("constants", constants.get());
    }

#ifdef _WIN32
    os.set("EOL", ev::fromUtf8("\r\n"));
    os.set("devNull", ev::fromUtf8("\\\\.\\nul"));
#else
    os.set("EOL", ev::fromUtf8("\n"));
    os.set("devNull", ev::fromUtf8("/dev/null"));
#endif

    ev::setGlobalValue("__brokit_os", os.get());
    ev::setGlobalValue("os", os.get());
}

} // namespace brokit::api
