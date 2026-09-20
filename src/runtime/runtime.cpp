#include "runtime/runtime.h"

#include "cli/driver.h"
#include "embed/embed.h"
#include "api/api.h"

#include <optional>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <atomic>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace brokit {

namespace {

constexpr const char* kFingerprintSymbol = "bronze_object_abi_fingerprint";
constexpr const char* kEntrySymbol = "bronze_main";

using ModuleHandle = void*;

ModuleHandle openModule(const std::string& path, std::string& error) {
#ifdef _WIN32
    std::error_code ec;
    const fs::path abs = fs::absolute(path, ec);
    const std::wstring wide = (ec ? fs::path(path) : abs).wstring();
    HMODULE h = ::LoadLibraryExW(wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Windows error %lu",
                      static_cast<unsigned long>(::GetLastError()));
        error = buf;
        return nullptr;
    }
    return reinterpret_cast<ModuleHandle>(h);
#else
    void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* msg = ::dlerror();
        error = msg ? msg : "dlopen failed";
        return nullptr;
    }
    return h;
#endif
}

void closeModule(ModuleHandle handle) {
    if (!handle) return;
#ifdef _WIN32
    ::FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
}

void* moduleSymbol(ModuleHandle handle, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(
        ::GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
    return ::dlsym(handle, name);
#endif
}

static std::atomic<uint64_t> g_evalCounter{1};
static std::vector<ModuleHandle> g_loadedModules;
static std::vector<std::string> g_tempSos;
static std::atomic<int> g_activeRuntimes{0};
static std::mutex g_moduleMutex;

} // namespace

Runtime::Runtime()
{
    g_activeRuntimes.fetch_add(1, std::memory_order_relaxed);

    // Try to find default brokit.globals
    for (const auto& candidate : {
        fs::current_path() / "src/api/brokit.globals",
        fs::current_path() / "brokit.globals",
        fs::path(__FILE__).parent_path().parent_path() / "api/brokit.globals"
    }) {
        if (fs::exists(candidate)) {
            globalsPath_ = candidate.string();
            break;
        }
    }
}

Runtime::~Runtime()
{
    if (g_activeRuntimes.fetch_sub(1, std::memory_order_relaxed) == 1) {
        std::lock_guard<std::mutex> lock(g_moduleMutex);
        for (auto h : g_loadedModules) {
            closeModule(h);
        }
        g_loadedModules.clear();

        std::error_code ec;
        for (const auto& f : g_tempSos) {
            fs::remove(f, ec);
        }
        g_tempSos.clear();
    }
}

bool Runtime::loadModule(const std::string& modulePath)
{
    std::string error;
    ModuleHandle handle = openModule(modulePath, error);
    if (!handle) {
        log(LogLevel::Error, "Could not load module %s: %s", modulePath.c_str(), error.c_str());
        return false;
    }

    fs::path p(modulePath);
    std::error_code ec;
    fs::path scratchDir = fs::temp_directory_path() / "brokit_build";
    if (fs::equivalent(p.parent_path(), scratchDir, ec)) {
#ifndef _WIN32
        fs::remove(p, ec);
#else
        std::lock_guard<std::mutex> lock(g_moduleMutex);
        g_tempSos.push_back(p.string());
#endif
    }

    const auto* moduleAbi = static_cast<const uint32_t*>(moduleSymbol(handle, kFingerprintSymbol));
    if (!moduleAbi) {
        log(LogLevel::Error, "%s exports no %s (not a bronze module)", modulePath.c_str(), kFingerprintSymbol);
        closeModule(handle);
        return false;
    }

    const uint32_t kRuntimeAbi = bronze::embed::abiFingerprint();
    if (*moduleAbi != kRuntimeAbi) {
        log(LogLevel::Error, "%s compiled against bronze ABI %08x, but runtime speaks %08x",
            modulePath.c_str(), *moduleAbi, kRuntimeAbi);
        closeModule(handle);
        return false;
    }

    auto entry = reinterpret_cast<void (*)()>(moduleSymbol(handle, kEntrySymbol));
    if (!entry) {
        log(LogLevel::Error, "%s carries bronze ABI stamp but exports no %s", modulePath.c_str(), kEntrySymbol);
        closeModule(handle);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_moduleMutex);
        g_loadedModules.push_back(handle);
    }

    bronze::embed::runEntry(entry);
    return true;
}

bool Runtime::loadFile(const std::string& path)
{
    fs::path p(path);
    if (!fs::exists(p)) {
        log(LogLevel::Error, "loadFile: file not found: %s", path.c_str());
        return false;
    }

    std::string ext = p.extension().string();
    if (ext == ".so" || ext == ".dll" || ext == ".dylib") {
        return loadModule(path);
    }

    // Compile .js to a loadable shared library module
    fs::path scratchDir = fs::temp_directory_path() / "brokit_build";
    fs::create_directories(scratchDir);

    std::error_code ec;
    fs::path absPath = fs::absolute(p, ec);
    fs::path absScratch = fs::absolute(scratchDir, ec);

    std::optional<api::RequireDirGuard> guard;
    if (absPath.parent_path() != absScratch) {
        guard.emplace(absPath.parent_path());
    }

    uint64_t id = g_evalCounter.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    fs::path outSo = scratchDir / (p.stem().string() + "_" + std::to_string(id) + ".dll");
#elif defined(__APPLE__)
    fs::path outSo = scratchDir / (p.stem().string() + "_" + std::to_string(id) + ".dylib");
#else
    fs::path outSo = scratchDir / (p.stem().string() + "_" + std::to_string(id) + ".so");
#endif

    std::string err;
    int status = bronze::cli::runBuild(
        path,
        outSo.string(),
        &err,
        /*infer=*/true,
        /*timings=*/false,
        /*emitObj=*/false,
        /*hostGlobalsPath=*/globalsPath_,
        /*inferStats=*/false,
        /*statsOut=*/nullptr,
        /*moduleRoots=*/{},
        /*entrySymbol=*/{},
        /*emitShared=*/true
    );

    if (status != 0) {
        log(LogLevel::Error, "Compilation failed for %s:\n%s", path.c_str(), err.c_str());
        return false;
    }

    return loadModule(outSo.string());
}

bool Runtime::eval(const std::string& code, const std::string& filename)
{
    fs::path scratchDir = fs::temp_directory_path() / "brokit_build";
    fs::create_directories(scratchDir);

    uint64_t id = g_evalCounter.fetch_add(1, std::memory_order_relaxed);
    fs::path tempJs = scratchDir / ("eval_" + std::to_string(id) + ".js");

    std::ofstream out(tempJs, std::ios::out | std::ios::binary);
    if (!out) {
        log(LogLevel::Error, "eval: failed to create temporary script file: %s", tempJs.string().c_str());
        return false;
    }
    out << code;
    out.close();

    bool ok = loadFile(tempJs.string());

    std::error_code ec;
    fs::remove(tempJs, ec);

    return ok;
}

bool Runtime::evalModule(const std::string& code, const std::string& filename)
{
    return eval(code, filename);
}

void Runtime::executePendingJobs()
{
    bronze::embed::drainMicrotasks();
}

void Runtime::collectGarbage()
{
    bronze::embed::collectGarbage();
}

void Runtime::setLogCallback(LogCallback cb)
{
    logCallback_ = std::move(cb);
}

void Runtime::log(LogLevel level, const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (logCallback_) {
        logCallback_(level, buf);
    } else {
        FILE* out = (level == LogLevel::Error || level == LogLevel::Warn) ? stderr : stdout;
        const char* prefix = "";
        switch (level) {
            case LogLevel::Debug: prefix = "[DEBUG] "; break;
            case LogLevel::Info:  prefix = "[INFO]  "; break;
            case LogLevel::Warn:  prefix = "[WARN]  "; break;
            case LogLevel::Error: prefix = "[ERROR] "; break;
        }
        fprintf(out, "%s%s\n", prefix, buf);
    }
}

} // namespace brokit
