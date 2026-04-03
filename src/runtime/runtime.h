#pragma once

#include <string>
#include <functional>

extern "C" {
#include "quickjs.h"
}

namespace brokit {

/// QuickJS runtime wrapper.
/// Owns the JSRuntime and primary JSContext. Non-copyable, non-movable.
class Runtime {
public:
    Runtime();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    /// Evaluate a script. Returns true on success.
    bool eval(const std::string& code, const std::string& filename = "<eval>");

    /// Evaluate code as an ES module. Returns true on success.
    bool evalModule(const std::string& code, const std::string& filename);

    /// Read a file from disk and evaluate it as a script. Returns true on success.
    bool loadFile(const std::string& path);

    /// Get the underlying JSContext.
    JSContext* context() const { return ctx_; }

    /// Get the underlying JSRuntime.
    JSRuntime* runtime() const { return rt_; }

    /// Get the global object (caller must JS_FreeValue when done).
    JSValue globalObject() const;

    /// Drain the microtask / promise job queue.
    void executePendingJobs();

    /// Install file-based ES module loader.
    /// basePath is used to resolve relative module specifiers.
    void setModuleLoader(const std::string& basePath = "");

    /// Check a JSValue for exceptions. Logs the error and frees the exception.
    /// Returns true if val *is* an exception.
    static bool checkException(JSContext* ctx, JSValue val);

    /// Log callback type — consumers can redirect output.
    enum class LogLevel { Debug, Info, Warn, Error };
    using LogCallback = std::function<void(LogLevel level, const std::string& msg)>;

    /// Set a custom log callback. If not set, logs to stderr.
    static void setLogCallback(LogCallback cb);

    /// Internal log helper.
    static void log(LogLevel level, const char* fmt, ...);

private:
    JSRuntime* rt_ = nullptr;
    JSContext* ctx_ = nullptr;
    std::string moduleBasePath_;

    static inline LogCallback logCallback_;
};

} // namespace brokit
