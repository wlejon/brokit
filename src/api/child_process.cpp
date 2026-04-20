#include "api/api.h"
#include "runtime/runtime.h"
#include "child_process.js.h"

#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#endif

namespace brokit::api {

// ---------------------------------------------------------------------------
// Async spawn registry
//
// `spawn()` returns immediately with a child handle; JS polls __brokit_cp_childPoll
// to learn when the child exits. We keep a small registry so we can hang on to
// OS handles (Windows HANDLE, Linux pid) without leaking them.
// ---------------------------------------------------------------------------
struct ChildHandle {
#ifdef _WIN32
    HANDLE process = nullptr;
    DWORD pid = 0;
#else
    pid_t pid = 0;
#endif
    std::atomic<bool> finished{false};
    int exitCode = -1;
    std::string signal;
};

static std::mutex g_childMutex;
static std::unordered_map<int, std::unique_ptr<ChildHandle>> g_children;
static std::atomic<int> g_nextChildId{1};

// ---------------------------------------------------------------------------
// Helper: run a command and capture stdout/stderr
// ---------------------------------------------------------------------------
struct ExecResult {
    std::string stdoutData;
    std::string stderrData;
    int exitCode = -1;
    bool timedOut = false;
    std::string error;
};

#ifdef _WIN32

static ExecResult runCommand(const std::string& command, const std::string& cwd,
                             const std::string& input, int timeoutMs, int maxBuffer)
{
    ExecResult result;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    // stdout pipe
    HANDLE hStdoutRead = nullptr, hStdoutWrite = nullptr;
    if (!CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0)) {
        result.error = "Failed to create stdout pipe";
        return result;
    }
    SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);

    // stderr pipe
    HANDLE hStderrRead = nullptr, hStderrWrite = nullptr;
    if (!CreatePipe(&hStderrRead, &hStderrWrite, &sa, 0)) {
        result.error = "Failed to create stderr pipe";
        CloseHandle(hStdoutRead);
        CloseHandle(hStdoutWrite);
        return result;
    }
    SetHandleInformation(hStderrRead, HANDLE_FLAG_INHERIT, 0);

    // stdin pipe (for writing input)
    HANDLE hStdinRead = nullptr, hStdinWrite = nullptr;
    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0)) {
        result.error = "Failed to create stdin pipe";
        CloseHandle(hStdoutRead);
        CloseHandle(hStdoutWrite);
        CloseHandle(hStderrRead);
        CloseHandle(hStderrWrite);
        return result;
    }
    SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.hStdOutput = hStdoutWrite;
    si.hStdError = hStderrWrite;
    si.hStdInput = hStdinRead;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};

    // Build command line: cmd /c "command"
    std::string cmdLine = "cmd /c " + command;

    BOOL ok = CreateProcessA(
        nullptr,
        cmdLine.data(),
        nullptr, nullptr,
        TRUE, // inherit handles
        CREATE_NO_WINDOW,
        nullptr,
        cwd.empty() ? nullptr : cwd.c_str(),
        &si, &pi
    );

    // Close write ends of pipes in parent
    CloseHandle(hStdoutWrite);
    CloseHandle(hStderrWrite);
    CloseHandle(hStdinRead);

    if (!ok) {
        result.error = "Failed to create process";
        result.exitCode = -1;
        CloseHandle(hStdoutRead);
        CloseHandle(hStderrRead);
        CloseHandle(hStdinWrite);
        return result;
    }

    // Write input if provided
    if (!input.empty()) {
        DWORD written;
        WriteFile(hStdinWrite, input.data(), static_cast<DWORD>(input.size()), &written, nullptr);
    }
    CloseHandle(hStdinWrite);

    // Wait for process
    DWORD waitTime = (timeoutMs > 0) ? static_cast<DWORD>(timeoutMs) : INFINITE;
    DWORD waitResult = WaitForSingleObject(pi.hProcess, waitTime);

    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
        result.timedOut = true;
        result.exitCode = -1;
    } else {
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        result.exitCode = static_cast<int>(exitCode);
    }

    // Read stdout
    {
        char buf[4096];
        DWORD bytesRead;
        while (ReadFile(hStdoutRead, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
            if (maxBuffer > 0 && result.stdoutData.size() + bytesRead > static_cast<size_t>(maxBuffer)) {
                result.stdoutData.append(buf, static_cast<size_t>(maxBuffer) - result.stdoutData.size());
                break;
            }
            result.stdoutData.append(buf, bytesRead);
        }
    }

    // Read stderr
    {
        char buf[4096];
        DWORD bytesRead;
        while (ReadFile(hStderrRead, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
            if (maxBuffer > 0 && result.stderrData.size() + bytesRead > static_cast<size_t>(maxBuffer)) {
                result.stderrData.append(buf, static_cast<size_t>(maxBuffer) - result.stderrData.size());
                break;
            }
            result.stderrData.append(buf, bytesRead);
        }
    }

    CloseHandle(hStdoutRead);
    CloseHandle(hStderrRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return result;
}

#else // Linux/macOS

static ExecResult runCommand(const std::string& command, const std::string& cwd,
                             const std::string& input, int timeoutMs, int maxBuffer)
{
    ExecResult result;

    int stdoutPipe[2], stderrPipe[2], stdinPipe[2];
    if (pipe(stdoutPipe) != 0 || pipe(stderrPipe) != 0 || pipe(stdinPipe) != 0) {
        result.error = "Failed to create pipes";
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.error = "Failed to fork";
        return result;
    }

    if (pid == 0) {
        // Child
        close(stdoutPipe[0]);
        close(stderrPipe[0]);
        close(stdinPipe[1]);

        dup2(stdinPipe[0], STDIN_FILENO);
        dup2(stdoutPipe[1], STDOUT_FILENO);
        dup2(stderrPipe[1], STDERR_FILENO);

        close(stdinPipe[0]);
        close(stdoutPipe[1]);
        close(stderrPipe[1]);

        if (!cwd.empty()) {
            if (chdir(cwd.c_str()) != 0) _exit(127);
        }

        execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
        _exit(127);
    }

    // Parent
    close(stdoutPipe[1]);
    close(stderrPipe[1]);
    close(stdinPipe[0]);

    // Write input
    if (!input.empty()) {
        write(stdinPipe[1], input.data(), input.size());
    }
    close(stdinPipe[1]);

    // Set non-blocking reads
    fcntl(stdoutPipe[0], F_SETFL, O_NONBLOCK);
    fcntl(stderrPipe[0], F_SETFL, O_NONBLOCK);

    // Read with optional timeout
    auto startTime = std::chrono::steady_clock::now();
    bool stdoutDone = false, stderrDone = false;

    while (!stdoutDone || !stderrDone) {
        if (timeoutMs > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count();
            if (elapsed >= timeoutMs) {
                kill(pid, SIGKILL);
                result.timedOut = true;
                break;
            }
        }

        char buf[4096];

        if (!stdoutDone) {
            ssize_t n = read(stdoutPipe[0], buf, sizeof(buf));
            if (n > 0) {
                if (maxBuffer > 0 && result.stdoutData.size() + n > static_cast<size_t>(maxBuffer)) {
                    result.stdoutData.append(buf, static_cast<size_t>(maxBuffer) - result.stdoutData.size());
                } else {
                    result.stdoutData.append(buf, n);
                }
            } else if (n == 0) {
                stdoutDone = true;
            }
        }

        if (!stderrDone) {
            ssize_t n = read(stderrPipe[0], buf, sizeof(buf));
            if (n > 0) {
                if (maxBuffer > 0 && result.stderrData.size() + n > static_cast<size_t>(maxBuffer)) {
                    result.stderrData.append(buf, static_cast<size_t>(maxBuffer) - result.stderrData.size());
                } else {
                    result.stderrData.append(buf, n);
                }
            } else if (n == 0) {
                stderrDone = true;
            }
        }

        usleep(1000); // 1ms poll
    }

    close(stdoutPipe[0]);
    close(stderrPipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else {
        result.exitCode = -1;
    }

    return result;
}

#endif

// ---------------------------------------------------------------------------
// Helper: parse options object
// ---------------------------------------------------------------------------
struct ExecOptions {
    std::string cwd;
    std::string encoding = "utf8";
    std::string input;
    int timeout = 0;        // 0 = no timeout
    int maxBuffer = 1024 * 1024; // 1MB default
    bool shell = true;
};

static ExecOptions parseOptions(JSContext* ctx, int argc, JSValueConst* argv, int optIdx)
{
    ExecOptions opts;
    if (optIdx >= argc || !JS_IsObject(argv[optIdx])) return opts;

    JSValue val;

    val = JS_GetPropertyStr(ctx, argv[optIdx], "cwd");
    if (JS_IsString(val)) {
        const char* s = JS_ToCString(ctx, val);
        if (s) { opts.cwd = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, val);

    val = JS_GetPropertyStr(ctx, argv[optIdx], "encoding");
    if (JS_IsString(val)) {
        const char* s = JS_ToCString(ctx, val);
        if (s) { opts.encoding = s; JS_FreeCString(ctx, s); }
    } else if (JS_IsNull(val)) {
        opts.encoding = "buffer";
    }
    JS_FreeValue(ctx, val);

    val = JS_GetPropertyStr(ctx, argv[optIdx], "timeout");
    if (JS_IsNumber(val)) {
        JS_ToInt32(ctx, &opts.timeout, val);
    }
    JS_FreeValue(ctx, val);

    val = JS_GetPropertyStr(ctx, argv[optIdx], "maxBuffer");
    if (JS_IsNumber(val)) {
        JS_ToInt32(ctx, &opts.maxBuffer, val);
    }
    JS_FreeValue(ctx, val);

    val = JS_GetPropertyStr(ctx, argv[optIdx], "input");
    if (JS_IsString(val)) {
        const char* s = JS_ToCString(ctx, val);
        if (s) { opts.input = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, val);

    return opts;
}

// Helper: convert string to JSValue based on encoding
static JSValue stringToOutput(JSContext* ctx, const std::string& data, const std::string& encoding)
{
    if (encoding == "buffer") {
        return JS_NewUint8ArrayCopy(ctx,
            reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }
    return JS_NewStringLen(ctx, data.data(), data.size());
}

// ---------------------------------------------------------------------------
// execSync(command[, options]) — blocking, returns stdout
// ---------------------------------------------------------------------------
static JSValue js_execSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "execSync: command required");

    const char* cmd = JS_ToCString(ctx, argv[0]);
    if (!cmd) return JS_EXCEPTION;

    std::string command(cmd);
    JS_FreeCString(ctx, cmd);

    auto opts = parseOptions(ctx, argc, argv, 1);

    ExecResult res = runCommand(command, opts.cwd, opts.input, opts.timeout, opts.maxBuffer);

    if (!res.error.empty()) {
        return JS_ThrowInternalError(ctx, "execSync: %s", res.error.c_str());
    }

    if (res.timedOut) {
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, "Command timed out"));
        JS_SetPropertyStr(ctx, err, "code", JS_NewString(ctx, "ETIMEDOUT"));
        JS_SetPropertyStr(ctx, err, "killed", JS_TRUE);
        JS_SetPropertyStr(ctx, err, "stdout", stringToOutput(ctx, res.stdoutData, opts.encoding));
        JS_SetPropertyStr(ctx, err, "stderr", stringToOutput(ctx, res.stderrData, opts.encoding));
        return JS_Throw(ctx, err);
    }

    if (res.exitCode != 0) {
        JSValue err = JS_NewError(ctx);
        std::string msg = "Command failed: " + command;
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg.c_str()));
        JS_SetPropertyStr(ctx, err, "status", JS_NewInt32(ctx, res.exitCode));
        JS_SetPropertyStr(ctx, err, "stdout", stringToOutput(ctx, res.stdoutData, opts.encoding));
        JS_SetPropertyStr(ctx, err, "stderr", stringToOutput(ctx, res.stderrData, opts.encoding));
        JS_SetPropertyStr(ctx, err, "code", JS_NewString(ctx, "ERR_CHILD_PROCESS"));
        return JS_Throw(ctx, err);
    }

    return stringToOutput(ctx, res.stdoutData, opts.encoding);
}

// ---------------------------------------------------------------------------
// __brokit_cp_exec(command, options) — blocking (used by JS layer to wrap in Promise)
// Returns { stdout, stderr, exitCode, error?, timedOut? }
// ---------------------------------------------------------------------------
static JSValue js_cp_exec(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "exec: command required");

    const char* cmd = JS_ToCString(ctx, argv[0]);
    if (!cmd) return JS_EXCEPTION;

    std::string command(cmd);
    JS_FreeCString(ctx, cmd);

    auto opts = parseOptions(ctx, argc, argv, 1);

    ExecResult res = runCommand(command, opts.cwd, opts.input, opts.timeout, opts.maxBuffer);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "stdout", stringToOutput(ctx, res.stdoutData, opts.encoding));
    JS_SetPropertyStr(ctx, obj, "stderr", stringToOutput(ctx, res.stderrData, opts.encoding));
    JS_SetPropertyStr(ctx, obj, "exitCode", JS_NewInt32(ctx, res.exitCode));
    if (!res.error.empty())
        JS_SetPropertyStr(ctx, obj, "error", JS_NewString(ctx, res.error.c_str()));
    JS_SetPropertyStr(ctx, obj, "timedOut", JS_NewBool(ctx, res.timedOut));

    return obj;
}

// ---------------------------------------------------------------------------
// spawnSync(command, args[, options]) — blocking, returns {stdout, stderr, status, signal, error}
// ---------------------------------------------------------------------------
static JSValue js_spawnSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "spawnSync: command required");

    const char* cmd = JS_ToCString(ctx, argv[0]);
    if (!cmd) return JS_EXCEPTION;

    std::string command(cmd);
    JS_FreeCString(ctx, cmd);

    // Build full command with args
    if (argc >= 2 && JS_IsArray(argv[1])) {
        uint32_t len = 0;
        JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
        JS_ToUint32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);

        for (uint32_t i = 0; i < len; i++) {
            JSValue elem = JS_GetPropertyUint32(ctx, argv[1], i);
            const char* arg = JS_ToCString(ctx, elem);
            if (arg) {
                command += " ";
                // Quote args containing spaces
                if (strchr(arg, ' ') || strchr(arg, '\t')) {
                    command += "\"";
                    command += arg;
                    command += "\"";
                } else {
                    command += arg;
                }
                JS_FreeCString(ctx, arg);
            }
            JS_FreeValue(ctx, elem);
        }
    }

    int optIdx = (argc >= 2 && JS_IsArray(argv[1])) ? 2 : 1;
    auto opts = parseOptions(ctx, argc, argv, optIdx);

    ExecResult res = runCommand(command, opts.cwd, opts.input, opts.timeout, opts.maxBuffer);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "stdout", stringToOutput(ctx, res.stdoutData, opts.encoding));
    JS_SetPropertyStr(ctx, obj, "stderr", stringToOutput(ctx, res.stderrData, opts.encoding));
    JS_SetPropertyStr(ctx, obj, "status", res.exitCode >= 0 ? JS_NewInt32(ctx, res.exitCode) : JS_NULL);
    JS_SetPropertyStr(ctx, obj, "signal", res.timedOut ? JS_NewString(ctx, "SIGKILL") : JS_NULL);

    if (!res.error.empty()) {
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, res.error.c_str()));
        JS_SetPropertyStr(ctx, obj, "error", err);
    }

    return obj;
}

// ---------------------------------------------------------------------------
// __brokit_cp_spawnAsync(file, args, options)
//
// Non-blocking spawn: starts a detached child process and returns
// { id, pid } immediately. Caller polls __brokit_cp_childPoll(id) to detect
// exit. Child inherits no stdio pipes (keeps its own terminal/window).
// ---------------------------------------------------------------------------
static JSValue js_spawnAsync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "spawnAsync: file required");

    const char* fileC = JS_ToCString(ctx, argv[0]);
    if (!fileC) return JS_EXCEPTION;
    std::string file(fileC);
    JS_FreeCString(ctx, fileC);

    std::vector<std::string> args;
    if (argc >= 2 && JS_IsArray(argv[1])) {
        uint32_t len = 0;
        JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
        JS_ToUint32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (uint32_t i = 0; i < len; i++) {
            JSValue elem = JS_GetPropertyUint32(ctx, argv[1], i);
            const char* a = JS_ToCString(ctx, elem);
            if (a) { args.emplace_back(a); JS_FreeCString(ctx, a); }
            JS_FreeValue(ctx, elem);
        }
    }

    int optIdx = (argc >= 2 && JS_IsArray(argv[1])) ? 2 : 1;
    auto opts = parseOptions(ctx, argc, argv, optIdx);

    auto handle = std::make_unique<ChildHandle>();

#ifdef _WIN32
    // Build quoted command line
    auto quote = [](const std::string& s) -> std::string {
        if (s.find_first_of(" \t\"") == std::string::npos) return s;
        std::string out = "\"";
        for (char c : s) {
            if (c == '"') out += "\\\"";
            else out += c;
        }
        out += "\"";
        return out;
    };
    std::string cmdLine = quote(file);
    for (auto& a : args) { cmdLine += " "; cmdLine += quote(a); }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    BOOL ok = CreateProcessA(
        nullptr,
        cmdLine.data(),
        nullptr, nullptr,
        FALSE,
        0, // no CREATE_NO_WINDOW — let GUI children show their window
        nullptr,
        opts.cwd.empty() ? nullptr : opts.cwd.c_str(),
        &si, &pi);

    if (!ok) {
        DWORD err = GetLastError();
        return JS_ThrowInternalError(ctx, "spawn failed: CreateProcess error %lu", err);
    }
    CloseHandle(pi.hThread);
    handle->process = pi.hProcess;
    handle->pid = pi.dwProcessId;
#else
    pid_t pid = fork();
    if (pid < 0) {
        return JS_ThrowInternalError(ctx, "spawn failed: fork");
    }
    if (pid == 0) {
        // Child
        if (!opts.cwd.empty()) {
            if (chdir(opts.cwd.c_str()) != 0) _exit(127);
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(file.c_str()));
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(file.c_str(), argv.data());
        _exit(127);
    }
    handle->pid = pid;
#endif

    int id = g_nextChildId.fetch_add(1);
    int pidVal = (int)handle->pid;
    {
        std::lock_guard<std::mutex> lock(g_childMutex);
        g_children[id] = std::move(handle);
    }

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "id", JS_NewInt32(ctx, id));
    JS_SetPropertyStr(ctx, obj, "pid", JS_NewInt32(ctx, pidVal));
    return obj;
}

// ---------------------------------------------------------------------------
// __brokit_cp_childPoll(id)
//
// Returns null if the child is still running, or { exitCode, signal } if it
// has exited. After a non-null return the handle is released from the
// registry — subsequent polls throw.
// ---------------------------------------------------------------------------
static JSValue js_childPoll(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "childPoll: id required");
    int id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]) < 0) return JS_EXCEPTION;

    ChildHandle* h = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_childMutex);
        auto it = g_children.find(id);
        if (it == g_children.end()) {
            return JS_ThrowRangeError(ctx, "childPoll: unknown child id %d", id);
        }
        h = it->second.get();
    }

#ifdef _WIN32
    DWORD status = WaitForSingleObject(h->process, 0);
    if (status == WAIT_TIMEOUT) {
        return JS_NULL;
    }
    DWORD code = 0;
    GetExitCodeProcess(h->process, &code);
    CloseHandle(h->process);
    int exitCode = (int)code;
#else
    int status = 0;
    pid_t r = waitpid(h->pid, &status, WNOHANG);
    if (r == 0) return JS_NULL;
    int exitCode = -1;
    std::string sig;
    if (r > 0) {
        if (WIFEXITED(status)) exitCode = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) { exitCode = 128 + WTERMSIG(status); sig = "SIG" + std::to_string(WTERMSIG(status)); }
    }
#endif

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "exitCode", JS_NewInt32(ctx, exitCode));
#ifdef _WIN32
    JS_SetPropertyStr(ctx, obj, "signal", JS_NULL);
#else
    if (sig.empty()) JS_SetPropertyStr(ctx, obj, "signal", JS_NULL);
    else JS_SetPropertyStr(ctx, obj, "signal", JS_NewString(ctx, sig.c_str()));
#endif

    {
        std::lock_guard<std::mutex> lock(g_childMutex);
        g_children.erase(id);
    }
    return obj;
}

// ---------------------------------------------------------------------------
// __brokit_cp_childKill(id, signal?)
// Returns true if a kill was issued. After kill the child still needs a
// subsequent poll to observe exit.
// ---------------------------------------------------------------------------
static JSValue js_childKill(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "childKill: id required");
    int id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]) < 0) return JS_EXCEPTION;

    std::lock_guard<std::mutex> lock(g_childMutex);
    auto it = g_children.find(id);
    if (it == g_children.end()) return JS_FALSE;
    ChildHandle* h = it->second.get();
#ifdef _WIN32
    TerminateProcess(h->process, 1);
#else
    int sig = SIGTERM;
    if (argc >= 2 && JS_IsString(argv[1])) {
        const char* s = JS_ToCString(ctx, argv[1]);
        if (s) {
            if (strcmp(s, "SIGKILL") == 0) sig = SIGKILL;
            else if (strcmp(s, "SIGINT") == 0) sig = SIGINT;
            JS_FreeCString(ctx, s);
        }
    }
    ::kill(h->pid, sig);
#endif
    return JS_TRUE;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------
void installChildProcess(JSContext* ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

    JS_SetPropertyStr(ctx, global, "__brokit_cp_execSync",
                      JS_NewCFunction(ctx, js_execSync, "execSync", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_cp_exec",
                      JS_NewCFunction(ctx, js_cp_exec, "__brokit_cp_exec", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_cp_spawnSync",
                      JS_NewCFunction(ctx, js_spawnSync, "spawnSync", 3));
    JS_SetPropertyStr(ctx, global, "__brokit_cp_spawnAsync",
                      JS_NewCFunction(ctx, js_spawnAsync, "__brokit_cp_spawnAsync", 3));
    JS_SetPropertyStr(ctx, global, "__brokit_cp_childPoll",
                      JS_NewCFunction(ctx, js_childPoll, "__brokit_cp_childPoll", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_cp_childKill",
                      JS_NewCFunction(ctx, js_childKill, "__brokit_cp_childKill", 2));

    JS_FreeValue(ctx, global);

    // Install JS polyfill
    JSValue r = JS_Eval(ctx, js_child_process, strlen(js_child_process),
                        "<child_process>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
