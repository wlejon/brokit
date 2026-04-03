#include "runtime/runtime.h"

#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdarg>
#include <cstdio>

extern "C" {
#include "quickjs.h"
}

namespace brokit {

// ---------------------------------------------------------------------------
// Module loader helpers (file-based)
// ---------------------------------------------------------------------------

static char* module_normalize(JSContext* ctx, const char* base_name,
                              const char* name, void* /*opaque*/)
{
    if (!name) return nullptr;

    std::string result;
    if (name[0] == '.' && base_name) {
        // Resolve relative to the directory of the base module.
        std::string base(base_name);
        auto slash = base.find_last_of("/\\");
        if (slash != std::string::npos) {
            result = base.substr(0, slash + 1) + name;
        } else {
            result = name;
        }
    } else {
        result = name;
    }

    char* buf = static_cast<char*>(js_malloc(ctx, result.size() + 1));
    if (buf) {
        std::memcpy(buf, result.c_str(), result.size() + 1);
    }
    return buf;
}

static JSModuleDef* module_loader(JSContext* ctx, const char* module_name,
                                  void* /*opaque*/)
{
    std::ifstream file(module_name, std::ios::in | std::ios::binary);
    if (!file) {
        JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
        return nullptr;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string source = ss.str();

    JSValue func = JS_Eval(ctx, source.c_str(), source.size(), module_name,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(func)) {
        Runtime::checkException(ctx, func);
        return nullptr;
    }

    JSModuleDef* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(func));
    JS_FreeValue(ctx, func);
    return m;
}

// ---------------------------------------------------------------------------
// Runtime implementation
// ---------------------------------------------------------------------------

Runtime::Runtime()
{
    rt_ = JS_NewRuntime();
    if (!rt_) {
        log(LogLevel::Error, "Failed to create QuickJS runtime");
        return;
    }

    JS_SetMemoryLimit(rt_, 256 * 1024 * 1024); // 256 MB
    JS_SetMaxStackSize(rt_, 8 * 1024 * 1024);  // 8 MB stack

    ctx_ = JS_NewContext(rt_);
    if (!ctx_) {
        log(LogLevel::Error, "Failed to create QuickJS context");
        JS_FreeRuntime(rt_);
        rt_ = nullptr;
        return;
    }
}

Runtime::~Runtime()
{
    if (ctx_) {
        JS_FreeContext(ctx_);
        ctx_ = nullptr;
    }
    if (rt_) {
        JS_FreeRuntime(rt_);
        rt_ = nullptr;
    }
}

bool Runtime::eval(const std::string& code, const std::string& filename)
{
    JSValue result = JS_Eval(ctx_, code.c_str(), code.size(),
                             filename.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (checkException(ctx_, result)) {
        return false;
    }
    JS_FreeValue(ctx_, result);
    return true;
}

bool Runtime::evalModule(const std::string& code, const std::string& filename)
{
    JSValue func = JS_Eval(ctx_, code.c_str(), code.size(),
                           filename.c_str(),
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (checkException(ctx_, func)) {
        return false;
    }

    JSValue result = JS_EvalFunction(ctx_, func);
    if (checkException(ctx_, result)) {
        return false;
    }
    JS_FreeValue(ctx_, result);
    return true;
}

bool Runtime::loadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file) {
        log(LogLevel::Error, "Failed to open file: %s", path.c_str());
        return false;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    return eval(ss.str(), path);
}

JSValue Runtime::globalObject() const
{
    return JS_GetGlobalObject(ctx_);
}

void Runtime::executePendingJobs()
{
    JSContext* pctx = nullptr;
    while (JS_ExecutePendingJob(rt_, &pctx) > 0) {
        // keep draining
    }
}

void Runtime::setModuleLoader(const std::string& basePath)
{
    moduleBasePath_ = basePath;
    JS_SetModuleLoaderFunc(rt_, module_normalize, module_loader, nullptr);
}

bool Runtime::checkException(JSContext* ctx, JSValue val)
{
    if (!JS_IsException(val))
        return false;

    JSValue exception = JS_GetException(ctx);
    const char* str = JS_ToCString(ctx, exception);
    if (str) {
        log(LogLevel::Error, "JS Exception: %s", str);
        JS_FreeCString(ctx, str);
    }

    if (JS_IsObject(exception)) {
        JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
        if (!JS_IsUndefined(stack)) {
            const char* stack_str = JS_ToCString(ctx, stack);
            if (stack_str) {
                log(LogLevel::Error, "Stack:\n%s", stack_str);
                JS_FreeCString(ctx, stack_str);
            }
        }
        JS_FreeValue(ctx, stack);
    }

    JS_FreeValue(ctx, exception);
    return true;
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
