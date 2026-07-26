#include "api/api.h"
#include "runtime/runtime.h"
#include "child_process.js.h"

#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
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
#include <poll.h>
#endif

namespace brokit::api {

// ---------------------------------------------------------------------------
// Async spawn registry
//
// `spawn()` returns immediately with a child handle; JS polls __brokit_cp_childPoll
// to learn when the child exits. We keep a small registry so we can hang on to
// OS handles (Windows HANDLE, Linux pid) without leaking them.
// ---------------------------------------------------------------------------
// One direction of a piped child's stdio. A reader thread appends here; JS
// drains it via __brokit_cp_childRead. `highWater` back-pressures the reader
// — and through the pipe's own kernel buffer, the child — so a producer that
// outruns the JS poll (rawvideo at tens of MB/frame) blocks instead of
// buffering without bound in the parent.
struct PipeBuf {
    std::mutex m;
    std::condition_variable cv;   // reader waits here while full
    std::vector<uint8_t> data;
    bool eof = false;
    size_t highWater = 8u << 20;  // 8 MB
};

struct ChildHandle {
#ifdef _WIN32
    HANDLE process = nullptr;
    DWORD pid = 0;
    HANDLE outRead = nullptr, errRead = nullptr, inWrite = nullptr;
#else
    pid_t pid = 0;
    int outRead = -1, errRead = -1, inWrite = -1;
#endif
    std::atomic<bool> finished{false};
    int exitCode = -1;
    std::string signal;

    // --- stdio: 'pipe' state (all unused when the child was spawned with the
    // default stdio: 'ignore') ---
    bool piped = false;
    PipeBuf out, err;
    std::thread outThread, errThread;
    std::atomic<bool> closing{false};
    std::mutex stdinMutex;         // serializes childWrite / closeStdin
    bool exitReported = false;     // childPoll already handed the code to JS

    // Readers poll with a short timeout and re-check `closing` each pass, so
    // teardown is deterministic even when a surviving grandchild holds a write
    // end open (the classic reason a blocking-read drain never sees EOF).
    void stopReaders() {
        closing.store(true, std::memory_order_release);
        out.cv.notify_all();
        err.cv.notify_all();
        if (outThread.joinable()) outThread.join();
        if (errThread.joinable()) errThread.join();
    }

    void closeStdin() {
        std::lock_guard<std::mutex> lock(stdinMutex);
#ifdef _WIN32
        if (inWrite) { CloseHandle(inWrite); inWrite = nullptr; }
#else
        if (inWrite >= 0) { ::close(inWrite); inWrite = -1; }
#endif
    }

    ~ChildHandle() {
        stopReaders();
        closeStdin();
#ifdef _WIN32
        if (outRead) CloseHandle(outRead);
        if (errRead) CloseHandle(errRead);
        // childPoll nulls this after closing on exit; a handle released while
        // the child is still live (or killed and never polled) lands here.
        if (process) CloseHandle(process);
#else
        if (outRead >= 0) ::close(outRead);
        if (errRead >= 0) ::close(errRead);
#endif
    }
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

// options.env, when present, REPLACES the child environment (Node semantics).
using EnvList = std::vector<std::pair<std::string, std::string>>;

#ifdef _WIN32
// Double-NUL-terminated "KEY=VALUE\0" block for CreateProcessA.
static std::string buildEnvBlock(const EnvList& env)
{
    std::string block;
    for (const auto& [k, v] : env) {
        block += k;
        block += '=';
        block += v;
        block += '\0';
    }
    block += '\0';
    return block;
}
#else
extern "C" char** environ;

// "KEY=VALUE" strings + char* view for execve-family calls.
static std::vector<std::string> buildEnvStrings(const EnvList& env)
{
    std::vector<std::string> out;
    out.reserve(env.size());
    for (const auto& [k, v] : env) out.push_back(k + "=" + v);
    return out;
}
#endif

#ifdef _WIN32

// Build a PROC_THREAD_ATTRIBUTE_HANDLE_LIST restricting inheritance to exactly
// the given handles. bInheritHandles=TRUE alone leaks EVERY inheritable handle
// in the process into the child — a concurrently spawned child then inherits
// another child's pipe write end, and that pipe's ReadFile never sees EOF
// until the unrelated child exits (the classic cross-spawn EOF hang).
// Returns false (with attrBuf left empty) if the attribute list cannot be
// built; callers then fall back to plain inheritance.
static bool buildHandleList(std::vector<uint8_t>& attrBuf,
                            const HANDLE* handles, size_t count)
{
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    if (attrSize == 0) return false;
    attrBuf.resize(attrSize);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &attrSize)) {
        attrBuf.clear();
        return false;
    }
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   const_cast<HANDLE*>(handles),
                                   count * sizeof(HANDLE), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(list);
        attrBuf.clear();
        return false;
    }
    return true;
}

static ExecResult runCommand(const std::string& command, const std::string& cwd,
                             const std::string& input, int timeoutMs, int maxBuffer,
                             const EnvList* env)
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

    STARTUPINFOEXA six = {};
    six.StartupInfo.cb = sizeof(six);
    six.StartupInfo.hStdOutput = hStdoutWrite;
    six.StartupInfo.hStdError = hStderrWrite;
    six.StartupInfo.hStdInput = hStdinRead;
    six.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;

    // Restrict inheritance to exactly this child's three pipe ends so
    // concurrent spawns can't cross-inherit each other's write ends.
    HANDLE inheritList[3] = { hStdoutWrite, hStderrWrite, hStdinRead };
    std::vector<uint8_t> attrBuf;
    bool haveAttrList = buildHandleList(attrBuf, inheritList, 3);
    if (haveAttrList)
        six.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());

    PROCESS_INFORMATION pi = {};

    // Build command line: cmd /c "command"
    std::string cmdLine = "cmd /c " + command;

    std::string envBlock;
    if (env) envBlock = buildEnvBlock(*env);

    BOOL ok = CreateProcessA(
        nullptr,
        cmdLine.data(),
        nullptr, nullptr,
        TRUE, // inherit handles (limited by the attribute list when present)
        CREATE_NO_WINDOW | (haveAttrList ? EXTENDED_STARTUPINFO_PRESENT : 0),
        env ? const_cast<char*>(envBlock.data()) : nullptr,
        cwd.empty() ? nullptr : cwd.c_str(),
        &six.StartupInfo, &pi
    );

    if (haveAttrList)
        DeleteProcThreadAttributeList(
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data()));

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

    // Drain stdout/stderr on their own threads BEFORE waiting on the process.
    // Waiting first deadlocks when the child fills a pipe (~4 KB kernel
    // buffer) and blocks in write() while we block in WaitForSingleObject.
    // Past maxBuffer we keep reading and discard, so a chatty child is never
    // back-pressured into the same deadlock either.
    std::atomic<bool> outDone{false}, errDone{false};
    auto drain = [maxBuffer](HANDLE h, std::string* out, std::atomic<bool>* done) {
        char buf[4096];
        DWORD bytesRead = 0;
        while (ReadFile(h, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
            if (maxBuffer > 0 && out->size() >= static_cast<size_t>(maxBuffer))
                continue;  // cap reached: keep draining, discard
            size_t take = bytesRead;
            if (maxBuffer > 0 && out->size() + take > static_cast<size_t>(maxBuffer))
                take = static_cast<size_t>(maxBuffer) - out->size();
            out->append(buf, take);
        }
        done->store(true, std::memory_order_release);
    };
    std::thread outThread(drain, hStdoutRead, &result.stdoutData, &outDone);
    std::thread errThread(drain, hStderrRead, &result.stderrData, &errDone);

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

    // The readers exit at EOF, which arrives once every write-end copy is
    // closed. After a timeout kill, a surviving grandchild (cmd /c children
    // inherit the std handles) can hold a write end open indefinitely —
    // cancel the blocked reads instead of joining forever. CancelSynchronousIo
    // only lands while the thread is inside ReadFile (ERROR_NOT_FOUND
    // otherwise), so retry briefly until the reader reports done.
    if (result.timedOut) {
        auto cancelReader = [](std::thread& t, std::atomic<bool>& done) {
            for (int i = 0; i < 200 && !done.load(std::memory_order_acquire); ++i) {
                CancelSynchronousIo(t.native_handle());
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };
        cancelReader(outThread, outDone);
        cancelReader(errThread, errDone);
    }
    outThread.join();
    errThread.join();

    CloseHandle(hStdoutRead);
    CloseHandle(hStderrRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return result;
}

#else // Linux/macOS

static ExecResult runCommand(const std::string& command, const std::string& cwd,
                             const std::string& input, int timeoutMs, int maxBuffer,
                             const EnvList* env)
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

        if (env) {
            auto envStrs = buildEnvStrings(*env);
            std::vector<char*> envp;
            for (auto& s : envStrs) envp.push_back(const_cast<char*>(s.c_str()));
            envp.push_back(nullptr);
            char* const shArgv[] = { const_cast<char*>("sh"), const_cast<char*>("-c"),
                                     const_cast<char*>(command.c_str()), nullptr };
            execve("/bin/sh", shArgv, envp.data());
            _exit(127);
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
// Pipe reader thread (stdio: 'pipe')
//
// Poll-then-read rather than a blocking read: teardown must not depend on the
// child — or on a grandchild holding an inherited write end — ever closing the
// pipe, so every pass re-checks `closing`. That is the same hazard runCommand
// works around after a timeout kill with CancelSynchronousIo; polling sidesteps
// it entirely. Costs one 2 ms wakeup while a piped child is alive.
// ---------------------------------------------------------------------------
#ifdef _WIN32
static void pipeReader(HANDLE h, PipeBuf* buf, std::atomic<bool>* closing)
{
    std::vector<uint8_t> chunk;
    for (;;) {
        if (closing->load(std::memory_order_acquire)) break;

        DWORD avail = 0;
        // FALSE here is a broken pipe: every write end is closed == EOF.
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // Wait for room BEFORE reading, so unread bytes stay in the kernel
        // pipe buffer and the child blocks in write() — real backpressure
        // rather than an unbounded parent-side queue.
        {
            std::unique_lock<std::mutex> lock(buf->m);
            buf->cv.wait(lock, [&] {
                return buf->data.size() < buf->highWater ||
                       closing->load(std::memory_order_acquire);
            });
            if (closing->load(std::memory_order_acquire)) break;
        }

        if (avail > 64u * 1024u) avail = 64u * 1024u;
        chunk.resize(avail);
        DWORD got = 0;
        if (!ReadFile(h, chunk.data(), avail, &got, nullptr) || got == 0) break;

        std::lock_guard<std::mutex> lock(buf->m);
        buf->data.insert(buf->data.end(), chunk.begin(), chunk.begin() + got);
    }
    std::lock_guard<std::mutex> lock(buf->m);
    buf->eof = true;
}
#else
static void pipeReader(int fd, PipeBuf* buf, std::atomic<bool>* closing)
{
    uint8_t chunk[64 * 1024];
    for (;;) {
        if (closing->load(std::memory_order_acquire)) break;

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = ::poll(&pfd, 1, 2);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        {
            std::unique_lock<std::mutex> lock(buf->m);
            buf->cv.wait(lock, [&] {
                return buf->data.size() < buf->highWater ||
                       closing->load(std::memory_order_acquire);
            });
            if (closing->load(std::memory_order_acquire)) break;
        }

        ssize_t got = ::read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        if (got == 0) break;   // EOF: all write ends closed

        std::lock_guard<std::mutex> lock(buf->m);
        buf->data.insert(buf->data.end(), chunk, chunk + got);
    }
    std::lock_guard<std::mutex> lock(buf->m);
    buf->eof = true;
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
    bool hasEnv = false;
    EnvList env;            // replaces the child environment when hasEnv
    std::string stdoutFile; // spawn only: redirect child stdout to this file
    std::string stderrFile; // spawn only: redirect child stderr (may equal stdoutFile)
    bool pipeStdio = false; // spawn only: stdio:'pipe' — stream stdout/stderr, writable stdin
    int highWaterMark = 8 * 1024 * 1024; // spawn only: per-stream backpressure threshold
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

    val = JS_GetPropertyStr(ctx, argv[optIdx], "env");
    if (JS_IsObject(val)) {
        opts.hasEnv = true;
        JSPropertyEnum* props = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &count, val,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < count; i++) {
                const char* key = JS_AtomToCString(ctx, props[i].atom);
                if (!key) continue;
                JSValue pv = JS_GetProperty(ctx, val, props[i].atom);
                if (!JS_IsUndefined(pv) && !JS_IsNull(pv)) {
                    const char* pvs = JS_ToCString(ctx, pv);
                    if (pvs) {
                        opts.env.emplace_back(key, pvs);
                        JS_FreeCString(ctx, pvs);
                    }
                }
                JS_FreeValue(ctx, pv);
                JS_FreeCString(ctx, key);
            }
            for (uint32_t i = 0; i < count; i++) JS_FreeAtom(ctx, props[i].atom);
            js_free(ctx, props);
        }
    }
    JS_FreeValue(ctx, val);

    val = JS_GetPropertyStr(ctx, argv[optIdx], "stdoutFile");
    if (JS_IsString(val)) {
        const char* s = JS_ToCString(ctx, val);
        if (s) { opts.stdoutFile = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, val);

    val = JS_GetPropertyStr(ctx, argv[optIdx], "stderrFile");
    if (JS_IsString(val)) {
        const char* s = JS_ToCString(ctx, val);
        if (s) { opts.stderrFile = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, val);

    // stdio: 'pipe' opts spawn into streaming mode. Anything else (including
    // the default) keeps the historical behaviour — no pipes at all — so an
    // existing caller that never reads can't start silently buffering.
    val = JS_GetPropertyStr(ctx, argv[optIdx], "stdio");
    if (JS_IsString(val)) {
        const char* s = JS_ToCString(ctx, val);
        if (s) { opts.pipeStdio = (strcmp(s, "pipe") == 0); JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, val);

    val = JS_GetPropertyStr(ctx, argv[optIdx], "highWaterMark");
    if (JS_IsNumber(val)) {
        JS_ToInt32(ctx, &opts.highWaterMark, val);
        if (opts.highWaterMark < 4096) opts.highWaterMark = 4096;
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

    ExecResult res = runCommand(command, opts.cwd, opts.input, opts.timeout, opts.maxBuffer,
                                opts.hasEnv ? &opts.env : nullptr);

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

    ExecResult res = runCommand(command, opts.cwd, opts.input, opts.timeout, opts.maxBuffer,
                                opts.hasEnv ? &opts.env : nullptr);

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

    ExecResult res = runCommand(command, opts.cwd, opts.input, opts.timeout, opts.maxBuffer,
                                opts.hasEnv ? &opts.env : nullptr);

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

    std::string envBlock;
    if (opts.hasEnv) envBlock = buildEnvBlock(opts.env);

    STARTUPINFOEXA six = {};
    six.StartupInfo.cb = sizeof(six);
    PROCESS_INFORMATION pi = {};

    // hOut/hErr/hIn are the CHILD ends — inheritable, and closed in the parent
    // right after CreateProcess in every path below.
    HANDLE hOut = nullptr, hErr = nullptr, hIn = nullptr;
    BOOL inheritHandles = FALSE;
    std::vector<uint8_t> attrBuf;
    bool haveAttrList = false;
    if (opts.pipeStdio) {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
        // Parent ends go straight into `handle` so its destructor closes them
        // on any early-return below.
        if (!CreatePipe(&handle->outRead, &hOut, &sa, 0))
            return JS_ThrowInternalError(ctx, "spawn: cannot create stdout pipe");
        SetHandleInformation(handle->outRead, HANDLE_FLAG_INHERIT, 0);
        if (!CreatePipe(&handle->errRead, &hErr, &sa, 0)) {
            CloseHandle(hOut);
            return JS_ThrowInternalError(ctx, "spawn: cannot create stderr pipe");
        }
        SetHandleInformation(handle->errRead, HANDLE_FLAG_INHERIT, 0);
        if (!CreatePipe(&hIn, &handle->inWrite, &sa, 0)) {
            CloseHandle(hOut);
            CloseHandle(hErr);
            return JS_ThrowInternalError(ctx, "spawn: cannot create stdin pipe");
        }
        SetHandleInformation(handle->inWrite, HANDLE_FLAG_INHERIT, 0);

        six.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        six.StartupInfo.hStdOutput = hOut;
        six.StartupInfo.hStdError  = hErr;
        six.StartupInfo.hStdInput  = hIn;
        inheritHandles = TRUE;

        // Same cross-spawn hazard as runCommand: bInheritHandles=TRUE alone
        // leaks every inheritable handle in the process into the child, so a
        // concurrent spawn inherits this child's write ends and its pipe never
        // sees EOF.
        HANDLE inheritList[3] = { hOut, hErr, hIn };
        haveAttrList = buildHandleList(attrBuf, inheritList, 3);
        if (haveAttrList)
            six.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    } else if (!opts.stdoutFile.empty() || !opts.stderrFile.empty()) {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
        if (!opts.stdoutFile.empty()) {
            hOut = CreateFileA(opts.stdoutFile.c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hOut == INVALID_HANDLE_VALUE)
                return JS_ThrowInternalError(ctx, "spawn: cannot open stdoutFile '%s'",
                                             opts.stdoutFile.c_str());
        }
        if (!opts.stderrFile.empty()) {
            if (opts.stderrFile == opts.stdoutFile) {
                hErr = hOut;
            } else {
                hErr = CreateFileA(opts.stderrFile.c_str(), GENERIC_WRITE,
                                   FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hErr == INVALID_HANDLE_VALUE) {
                    if (hOut) CloseHandle(hOut);
                    return JS_ThrowInternalError(ctx, "spawn: cannot open stderrFile '%s'",
                                                 opts.stderrFile.c_str());
                }
            }
        }
        hIn = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        six.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        six.StartupInfo.hStdOutput = hOut ? hOut : GetStdHandle(STD_OUTPUT_HANDLE);
        six.StartupInfo.hStdError = hErr ? hErr : GetStdHandle(STD_ERROR_HANDLE);
        six.StartupInfo.hStdInput = hIn;
        inheritHandles = TRUE;

        // Restrict inheritance to this child's own std handles — same
        // cross-spawn hazard as runCommand: without a handle list a child
        // spawned here also inherits every concurrently live pipe end.
        // Only when all three std handles are ones we created: a GetStdHandle
        // fallback handle may not be inheritable, and putting it in the list
        // (or omitting it) would break the child's stdio.
        if (hOut && hErr) {
            HANDLE inheritList[3];
            size_t n = 0;
            inheritList[n++] = hOut;
            if (hErr != hOut) inheritList[n++] = hErr;
            if (hIn && hIn != INVALID_HANDLE_VALUE) inheritList[n++] = hIn;
            haveAttrList = buildHandleList(attrBuf, inheritList, n);
            if (haveAttrList)
                six.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
        }
    }

    BOOL ok = CreateProcessA(
        nullptr,
        cmdLine.data(),
        nullptr, nullptr,
        inheritHandles,
        // No CREATE_NO_WINDOW by default — let GUI children show their window.
        // A piped child is by definition being driven programmatically, so
        // suppress the console window that would otherwise flash up.
        (opts.pipeStdio ? CREATE_NO_WINDOW : 0) |
        (haveAttrList ? EXTENDED_STARTUPINFO_PRESENT : 0),
        opts.hasEnv ? const_cast<char*>(envBlock.data()) : nullptr,
        opts.cwd.empty() ? nullptr : opts.cwd.c_str(),
        &six.StartupInfo, &pi);

    DWORD createErr = ok ? 0 : GetLastError();
    if (haveAttrList)
        DeleteProcThreadAttributeList(
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data()));
    if (hOut) CloseHandle(hOut);
    if (hErr && hErr != hOut) CloseHandle(hErr);
    if (hIn && hIn != INVALID_HANDLE_VALUE) CloseHandle(hIn);

    if (!ok) {
        return JS_ThrowInternalError(ctx, "spawn failed: CreateProcess error %lu", createErr);
    }
    CloseHandle(pi.hThread);
    handle->process = pi.hProcess;
    handle->pid = pi.dwProcessId;
#else
    // stdio:'pipe' — create the three stdio pipes before forking. The child
    // dups its ends over 0/1/2; the parent keeps outRead/errRead/inWrite (in
    // `handle`, so its destructor closes them on any early return below).
    int outPipe[2] = {-1, -1}, errPipe[2] = {-1, -1}, inPipe[2] = {-1, -1};
    if (opts.pipeStdio) {
        if (pipe(outPipe) != 0 || pipe(errPipe) != 0 || pipe(inPipe) != 0) {
            for (int fd : { outPipe[0], outPipe[1], errPipe[0], errPipe[1],
                            inPipe[0], inPipe[1] })
                if (fd >= 0) ::close(fd);
            return JS_ThrowInternalError(ctx, "spawn: cannot create stdio pipes");
        }
        handle->outRead = outPipe[0];
        handle->errRead = errPipe[0];
        handle->inWrite = inPipe[1];
    }
    auto closeChildEnds = [&]() {
        if (!opts.pipeStdio) return;
        if (outPipe[1] >= 0) { ::close(outPipe[1]); outPipe[1] = -1; }
        if (errPipe[1] >= 0) { ::close(errPipe[1]); errPipe[1] = -1; }
        if (inPipe[0]  >= 0) { ::close(inPipe[0]);  inPipe[0]  = -1; }
    };

    // Self-pipe so an execvp failure in the child (e.g. a missing binary)
    // surfaces synchronously here, matching Windows' CreateProcess failure. The
    // write end is close-on-exec: a successful exec closes it (parent reads EOF);
    // a failed exec writes errno through it before _exit.
    int execPipe[2] = {-1, -1};
    if (pipe(execPipe) != 0) {
        closeChildEnds();
        return JS_ThrowInternalError(ctx, "spawn failed: pipe");
    }
    fcntl(execPipe[1], F_SETFD, fcntl(execPipe[1], F_GETFD) | FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close(execPipe[0]);
        close(execPipe[1]);
        closeChildEnds();
        return JS_ThrowInternalError(ctx, "spawn failed: fork");
    }
    if (pid == 0) {
        // Child
        close(execPipe[0]);
        if (opts.pipeStdio) {
            // Drop the parent ends, then move our ends onto 0/1/2.
            ::close(outPipe[0]);
            ::close(errPipe[0]);
            ::close(inPipe[1]);
            dup2(outPipe[1], STDOUT_FILENO);
            dup2(errPipe[1], STDERR_FILENO);
            dup2(inPipe[0],  STDIN_FILENO);
            ::close(outPipe[1]);
            ::close(errPipe[1]);
            ::close(inPipe[0]);
        }
        if (!opts.cwd.empty()) {
            if (chdir(opts.cwd.c_str()) != 0) _exit(127);
        }
        if (!opts.stdoutFile.empty()) {
            int fd = open(opts.stdoutFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) _exit(127);
            dup2(fd, STDOUT_FILENO);
            if (opts.stderrFile == opts.stdoutFile) dup2(fd, STDERR_FILENO);
            close(fd);
        }
        if (!opts.stderrFile.empty() && opts.stderrFile != opts.stdoutFile) {
            int fd = open(opts.stderrFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) _exit(127);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(file.c_str()));
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        if (opts.hasEnv) {
            auto envStrs = buildEnvStrings(opts.env);
            std::vector<char*> envp;
            for (auto& s : envStrs) envp.push_back(const_cast<char*>(s.c_str()));
            envp.push_back(nullptr);
            environ = envp.data();   // pre-exec in the forked child; execvp keeps PATH search
        }
        execvp(file.c_str(), argv.data());
        int execErrno = errno;
        // Report the exec failure to the parent, then exit. Loop guards against
        // a short write; the parent only inspects the first int.
        const char* p = reinterpret_cast<const char*>(&execErrno);
        size_t left = sizeof(execErrno);
        while (left > 0) {
            ssize_t n = write(execPipe[1], p, left);
            if (n <= 0) break;
            p += n;
            left -= static_cast<size_t>(n);
        }
        _exit(127);
    }
    handle->pid = pid;

    // Parent: drop the child's pipe ends, or stdout/stderr never reach EOF.
    closeChildEnds();

    // Parent: wait for the child to either exec (EOF) or report an errno.
    close(execPipe[1]);
    int childErrno = 0;
    ssize_t got = read(execPipe[0], &childErrno, sizeof(childErrno));
    close(execPipe[0]);
    if (got == static_cast<ssize_t>(sizeof(childErrno)) && childErrno != 0) {
        // exec failed in the child — reap the transient process and throw.
        int status = 0;
        waitpid(pid, &status, 0);
        return JS_ThrowInternalError(ctx, "spawn failed: %s: %s",
                                     file.c_str(), strerror(childErrno));
    }
#endif

    // Start draining before the handle goes into the registry. The threads hold
    // raw pointers into *handle, which is stable — the unique_ptr moves, the
    // pointee does not.
    if (opts.pipeStdio) {
        handle->piped = true;
        handle->out.highWater = static_cast<size_t>(opts.highWaterMark);
        handle->err.highWater = static_cast<size_t>(opts.highWaterMark);
        ChildHandle* hp = handle.get();
        hp->outThread = std::thread(pipeReader, hp->outRead, &hp->out, &hp->closing);
        hp->errThread = std::thread(pipeReader, hp->errRead, &hp->err, &hp->closing);
    }

    int id = g_nextChildId.fetch_add(1);
    int pidVal = (int)handle->pid;
    bool piped = handle->piped;
    {
        std::lock_guard<std::mutex> lock(g_childMutex);
        g_children[id] = std::move(handle);
    }

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "id", JS_NewInt32(ctx, id));
    JS_SetPropertyStr(ctx, obj, "pid", JS_NewInt32(ctx, pidVal));
    JS_SetPropertyStr(ctx, obj, "piped", JS_NewBool(ctx, piped));
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

    // A piped child outlives its own exit: the reader threads may still hold
    // buffered output, so the handle stays registered and this reports the
    // cached result until JS calls childRelease.
    auto exitInfo = [&](int code, const std::string& sig) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "exitCode", JS_NewInt32(ctx, code));
        if (sig.empty()) JS_SetPropertyStr(ctx, obj, "signal", JS_NULL);
        else             JS_SetPropertyStr(ctx, obj, "signal", JS_NewString(ctx, sig.c_str()));
        return obj;
    };

    if (h->exitReported) return exitInfo(h->exitCode, h->signal);

#ifdef _WIN32
    DWORD status = WaitForSingleObject(h->process, 0);
    if (status == WAIT_TIMEOUT) {
        return JS_NULL;
    }
    DWORD code = 0;
    GetExitCodeProcess(h->process, &code);
    CloseHandle(h->process);
    h->process = nullptr;   // the destructor must not double-close
    int exitCode = (int)code;
    std::string sig;
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

    h->exitCode = exitCode;
    h->signal = sig;
    h->exitReported = true;

    JSValue obj = exitInfo(exitCode, sig);

    if (!h->piped) {
        std::lock_guard<std::mutex> lock(g_childMutex);
        g_children.erase(id);
    }
    return obj;
}

// ---------------------------------------------------------------------------
// __brokit_cp_childRead(id)
//
// Drains whatever the reader threads have buffered. Returns
// { stdout, stderr, stdoutEof, stderrEof } where each stream is a Uint8Array
// (binary — rawvideo and text both survive) or null when nothing was pending.
// Draining is what releases backpressure, so a caller that stops reading
// stalls the child rather than growing the parent's heap.
// ---------------------------------------------------------------------------
static JSValue js_childRead(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "childRead: id required");
    int id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]) < 0) return JS_EXCEPTION;

    ChildHandle* h = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_childMutex);
        auto it = g_children.find(id);
        if (it == g_children.end())
            return JS_ThrowRangeError(ctx, "childRead: unknown child id %d", id);
        h = it->second.get();
    }
    if (!h->piped)
        return JS_ThrowTypeError(ctx, "childRead: child %d was not spawned with stdio:'pipe'", id);

    auto take = [&](PipeBuf& buf, JSValue& outVal, bool& eofOut) {
        std::vector<uint8_t> drained;
        {
            std::lock_guard<std::mutex> lock(buf.m);
            drained.swap(buf.data);
            eofOut = buf.eof;
        }
        // Notify outside the lock: the reader wakes straight into its wait
        // predicate instead of blocking on a mutex we still hold.
        if (!drained.empty()) buf.cv.notify_all();
        outVal = drained.empty()
            ? JS_NULL
            : JS_NewUint8ArrayCopy(ctx, drained.data(), drained.size());
    };

    JSValue outVal = JS_NULL, errVal = JS_NULL;
    bool outEof = false, errEof = false;
    take(h->out, outVal, outEof);
    take(h->err, errVal, errEof);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "stdout", outVal);
    JS_SetPropertyStr(ctx, obj, "stderr", errVal);
    JS_SetPropertyStr(ctx, obj, "stdoutEof", JS_NewBool(ctx, outEof));
    JS_SetPropertyStr(ctx, obj, "stderrEof", JS_NewBool(ctx, errEof));
    return obj;
}

// ---------------------------------------------------------------------------
// __brokit_cp_childWrite(id, data)
//
// Writes to the child's stdin. `data` is a string (UTF-8) or TypedArray/
// ArrayBuffer. Returns the byte count written, or -1 if stdin is already
// closed. Blocking: a child that never reads will stall the JS thread once
// the pipe buffer fills, so callers streaming large input should chunk it.
// ---------------------------------------------------------------------------
static JSValue js_childWrite(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "childWrite: id and data required");
    int id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]) < 0) return JS_EXCEPTION;

    ChildHandle* h = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_childMutex);
        auto it = g_children.find(id);
        if (it == g_children.end())
            return JS_ThrowRangeError(ctx, "childWrite: unknown child id %d", id);
        h = it->second.get();
    }
    if (!h->piped)
        return JS_ThrowTypeError(ctx, "childWrite: child %d was not spawned with stdio:'pipe'", id);

    const uint8_t* bytes = nullptr;
    size_t len = 0;
    std::string tmp;
    size_t byteOffset = 0, byteLen = 0, bytesPerElem = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[1], &byteOffset, &byteLen, &bytesPerElem);
    if (!JS_IsException(ab)) {
        size_t abLen = 0;
        uint8_t* abPtr = JS_GetArrayBuffer(ctx, &abLen, ab);
        if (abPtr) { bytes = abPtr + byteOffset; len = byteLen; }
        JS_FreeValue(ctx, ab);
    } else {
        // Not a TypedArray. Clear the probe's exception before trying the next
        // shape — both of these throw on a miss, and a leftover pending
        // exception would surface spuriously at an unrelated call site.
        JS_FreeValue(ctx, JS_GetException(ctx));
        size_t abLen = 0;
        uint8_t* abPtr = JS_GetArrayBuffer(ctx, &abLen, argv[1]);
        if (abPtr) {
            bytes = abPtr;
            len = abLen;
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
            const char* s = JS_ToCStringLen(ctx, &len, argv[1]);
            if (!s) return JS_EXCEPTION;
            tmp.assign(s, len);
            JS_FreeCString(ctx, s);
            bytes = reinterpret_cast<const uint8_t*>(tmp.data());
        }
    }
    if (!bytes) return JS_NewInt32(ctx, 0);

    std::lock_guard<std::mutex> lock(h->stdinMutex);
    size_t written = 0;
#ifdef _WIN32
    if (!h->inWrite) return JS_NewInt32(ctx, -1);
    while (written < len) {
        DWORD n = 0;
        if (!WriteFile(h->inWrite, bytes + written,
                       static_cast<DWORD>(len - written), &n, nullptr) || n == 0)
            break;
        written += n;
    }
#else
    if (h->inWrite < 0) return JS_NewInt32(ctx, -1);
    while (written < len) {
        ssize_t n = ::write(h->inWrite, bytes + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        written += static_cast<size_t>(n);
    }
#endif
    return JS_NewInt32(ctx, static_cast<int>(written));
}

// ---------------------------------------------------------------------------
// __brokit_cp_childCloseStdin(id)
//
// Sends EOF on the child's stdin. Tools that read a stream to completion
// (ffmpeg with `-i pipe:0`) never finish without it.
// ---------------------------------------------------------------------------
static JSValue js_childCloseStdin(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "childCloseStdin: id required");
    int id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]) < 0) return JS_EXCEPTION;

    std::lock_guard<std::mutex> lock(g_childMutex);
    auto it = g_children.find(id);
    if (it == g_children.end()) return JS_FALSE;
    it->second->closeStdin();
    return JS_TRUE;
}

// ---------------------------------------------------------------------------
// __brokit_cp_childRelease(id)
//
// Drops a piped child's handle: stops the reader threads and closes the pipes.
// Non-piped children are released automatically by childPoll on exit; piped
// ones are kept so post-exit output can still be drained, so JS must call this
// once it has seen EOF on both streams. Idempotent.
// ---------------------------------------------------------------------------
static JSValue js_childRelease(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "childRelease: id required");
    int id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]) < 0) return JS_EXCEPTION;

    // Move the handle out under the lock and destroy it after releasing, so a
    // reader thread's final buffer append can't deadlock against g_childMutex.
    std::unique_ptr<ChildHandle> doomed;
    {
        std::lock_guard<std::mutex> lock(g_childMutex);
        auto it = g_children.find(id);
        if (it == g_children.end()) return JS_FALSE;
        doomed = std::move(it->second);
        g_children.erase(it);
    }
    return JS_TRUE;
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
    JS_SetPropertyStr(ctx, global, "__brokit_cp_childRead",
                      JS_NewCFunction(ctx, js_childRead, "__brokit_cp_childRead", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_cp_childWrite",
                      JS_NewCFunction(ctx, js_childWrite, "__brokit_cp_childWrite", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_cp_childCloseStdin",
                      JS_NewCFunction(ctx, js_childCloseStdin, "__brokit_cp_childCloseStdin", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_cp_childRelease",
                      JS_NewCFunction(ctx, js_childRelease, "__brokit_cp_childRelease", 1));

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
