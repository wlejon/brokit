#include "api/api.h"
#include "runtime/runtime.h"
#include "child_process.js.h"

#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <memory>

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
