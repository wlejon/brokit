#pragma once

#include <string>
#include <functional>
#include <cstdint>

#include "embed/embed.h"

namespace brokit {

/// Bronze AOT JavaScript runtime wrapper.
/// Manages script compilation, dynamic execution, module loading, and microtask draining.
class Runtime {
public:
    Runtime();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    /// Compile and evaluate a JavaScript string. Returns true on success.
    bool eval(const std::string& code, const std::string& filename = "<eval>");

    /// Evaluate code as an ES module. Returns true on success.
    bool evalModule(const std::string& code, const std::string& filename);

    /// Read a file from disk and evaluate it.
    /// If path is a .so/.dll, loads it directly. If .js, compiles to a shared module first.
    bool loadFile(const std::string& path);

    /// Load and run an already-compiled Bronze shared module (.so/.dll).
    bool loadModule(const std::string& modulePath);

    /// Drain the microtask / promise job queue.
    void executePendingJobs();

    /// Run full garbage collection.
    void collectGarbage();

    /// Set the path to the host-globals manifest for AOT compilation.
    void setGlobalsPath(const std::string& path) { globalsPath_ = path; }

    /// Set base directory for module resolution.
    void setModuleLoader(const std::string& basePath = "") { moduleBasePath_ = basePath; }

    /// Log callback type — consumers can redirect output.
    enum class LogLevel { Debug, Info, Warn, Error };
    using LogCallback = std::function<void(LogLevel level, const std::string& msg)>;

    /// Set a custom log callback. If not set, logs to stdout/stderr.
    static void setLogCallback(LogCallback cb);

    /// Internal log helper.
    static void log(LogLevel level, const char* fmt, ...);

private:
    std::string globalsPath_;
    std::string moduleBasePath_;

    static inline LogCallback logCallback_;
};

} // namespace brokit
