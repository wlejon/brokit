// path: the string work of normalize / join / resolve / dirname / basename /
// extname in C++, handed to js/path.js as __brokit_path_native. The script
// versions cost several microseconds a call (a regex split, an array, a
// join); a folder listing joins hundreds of names a frame. The rules are the
// ones js/path.js had: both separators are read on every platform, the
// platform's own is written, and on Windows a leading "X:" is the drive.

#include "api/api.h"
#include "api/arg_reader.h"
#include "api/object_builder.h"
#include "embed/embed.h"

#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#endif

extern "C" void bronze_path_main();

namespace brokit::api {

namespace {

#ifdef _WIN32
constexpr bool kWindows = true;
constexpr char kSep = '\\';
#else
constexpr bool kWindows = false;
constexpr char kSep = '/';
#endif

inline bool isSep(char c) { return c == '/' || c == '\\'; }

inline bool hasDrive(std::string_view p) { return kWindows && p.size() >= 2 && p[1] == ':'; }

// A script value as path text: strings as they are, anything else through
// String(), as the script version did (a Symbol is a TypeError there too).
bool textOf(Value v, std::string& out) {
    if (ev::isSymbol(v)) return false;
    out = ev::toUtf8(v);
    return true;
}

// The segments after the root, with "." and empty ones dropped and ".."
// taking its parent (or kept, above a relative root).
void normalizeInto(std::string_view p, std::string& out) {
    if (p.empty()) { out = "."; return; }
    out.clear();
    out.reserve(p.size());
    size_t i = 0;
    if (hasDrive(p)) { out.append(p.substr(0, 2)); i = 2; }
    bool absolute = false;
    if (i < p.size() && isSep(p[i])) { absolute = true; out += kSep; ++i; }
    const size_t rootLen = out.size();
    size_t kept = 0;   // segments written after the root
    size_t dotdots = 0; // of them, the leading ".."s (a relative path's)
    while (i < p.size()) {
        size_t j = i;
        while (j < p.size() && !isSep(p[j])) ++j;
        std::string_view seg = p.substr(i, j - i);
        i = j + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (kept > dotdots) {
                // Drop the last segment and the separator before it.
                size_t cut = out.find_last_of(kSep);
                if (cut == std::string::npos || cut < rootLen) cut = rootLen;
                out.resize(cut);
                --kept;
            } else if (!absolute) {
                if (kept) out += kSep;
                out += "..";
                ++kept;
                ++dotdots;
            }
            continue;
        }
        if (kept) out += kSep;
        out.append(seg);
        ++kept;
    }
    if (out.empty()) out = absolute ? std::string(1, kSep) : std::string(".");
}

Value js_normalize(Value, std::span<const Value> a) {
    std::string p;
    if (!textOf(hasArg(a, 0) ? a[0] : ev::undefined(), p))
        return ev::throwTypeError("path.normalize: a Symbol is not a path");
    std::string out;
    normalizeInto(p, out);
    return ev::fromUtf8(out);
}

Value js_join(Value, std::span<const Value> a) {
    std::string joined;
    for (const Value& v : a) {
        if (!ev::isString(v)) continue;
        std::string s = ev::toUtf8(v);
        if (s.empty()) continue;
        if (!joined.empty()) joined += kSep;
        joined += s;
    }
    if (joined.empty()) return ev::fromUtf8(".");
    std::string out;
    normalizeInto(joined, out);
    return ev::fromUtf8(out);
}

std::string currentDirectory() {
#ifdef _WIN32
    DWORD n = GetCurrentDirectoryW(0, nullptr);
    if (n == 0) return {};
    std::wstring w(n, L'\0');
    n = GetCurrentDirectoryW(n, w.data());
    w.resize(n);
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), len, nullptr, nullptr);
    return out;
#else
    char buf[PATH_MAX];
    return getcwd(buf, sizeof(buf)) ? std::string(buf) : std::string();
#endif
}

bool resolvesAbsolute(std::string_view p) {
    if (kWindows) return p.size() >= 3 && p[1] == ':' && isSep(p[2]);
    return !p.empty() && p[0] == '/';
}

Value js_resolve(Value, std::span<const Value> a) {
    // Right to left until a part is absolute; the working directory under
    // the rest when none is.
    std::string resolved;
    bool absolute = false;
    for (size_t k = a.size(); k-- > 0 && !absolute;) {
        if (!ev::isString(a[k])) continue;
        std::string p = ev::toUtf8(a[k]);
        if (p.empty()) continue;
        resolved = p + kSep + resolved;
        absolute = resolvesAbsolute(p);
    }
    if (!absolute) resolved = currentDirectory() + kSep + resolved;
    std::string out;
    normalizeInto(resolved, out);
    return ev::fromUtf8(out);
}

Value js_dirname(Value, std::span<const Value> a) {
    if (!hasArg(a, 0) || !ev::isString(a[0])) return ev::fromUtf8(".");
    std::string p = ev::toUtf8(a[0]);
    if (p.empty()) return ev::fromUtf8(".");
    size_t idx = hasDrive(p) ? 2 : 0;
    // The last separator, a trailing one aside.
    size_t lastSep = std::string::npos;
    for (size_t i = p.size(); i-- > idx;) {
        if (isSep(p[i]) && i != p.size() - 1) { lastSep = i; break; }
    }
    if (lastSep == std::string::npos) {
        // A root alone ("/", "C:\") is its own dirname, as in Node.
        if (idx < p.size() && isSep(p[idx])) return ev::fromUtf8(p.substr(0, idx + 1));
        return ev::fromUtf8(idx ? p.substr(0, 2) : std::string("."));
    }
    if (lastSep == idx) return ev::fromUtf8(p.substr(0, idx + 1));
    return ev::fromUtf8(p.substr(0, lastSep));
}

std::string baseOf(std::string p) {
    while (!p.empty() && isSep(p.back())) p.pop_back();
    if (kWindows && p.size() == 2 && p[1] == ':' &&
        ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')))
        return {};   // a drive root has no name, as in Node
    size_t cut = p.find_last_of("/\\");
    return cut == std::string::npos ? p : p.substr(cut + 1);
}

Value js_basename(Value, std::span<const Value> a) {
    if (!hasArg(a, 0) || !ev::isString(a[0])) return ev::fromUtf8("");
    std::string base = baseOf(ev::toUtf8(a[0]));
    if (hasArg(a, 1) && ev::isString(a[1])) {
        std::string ext = ev::toUtf8(a[1]);
        if (!ext.empty() && base.size() >= ext.size() &&
            base.compare(base.size() - ext.size(), ext.size(), ext) == 0)
            base.resize(base.size() - ext.size());
    }
    return ev::fromUtf8(base);
}

Value js_extname(Value, std::span<const Value> a) {
    if (!hasArg(a, 0) || !ev::isString(a[0])) return ev::fromUtf8("");
    std::string base = baseOf(ev::toUtf8(a[0]));
    size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return ev::fromUtf8("");
    return ev::fromUtf8(base.substr(dot));
}

} // namespace

void installPath() {
    ObjectBuilder n;
    n.def("normalize", 1, js_normalize);
    n.def("join", 0, js_join);
    n.def("resolve", 0, js_resolve);
    n.def("dirname", 1, js_dirname);
    n.def("basename", 2, js_basename);
    n.def("extname", 1, js_extname);
    ev::setGlobalValue("__brokit_path_native", n.get());
    bronze::embed::runEntry(bronze_path_main);
}

} // namespace brokit::api
