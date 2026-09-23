#include "api/api.h"
#include "api/arg_reader.h"
#include "api/object_builder.h"

extern "C" void bronze_child_process_main();

#include <algorithm>
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

// Quote one argv entry for a CreateProcess command line. Only spaces, tabs and
// embedded quotes need it; anything else passes through so a plain path stays
// readable in a process listing.
static std::string quoteArg(const std::string& s)
{
    if (s.find_first_of(" \t\"") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
}

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

// Blocking run, capturing stdout/stderr.
//
// `argv` decides whether a shell is involved. When it is null the `command`
// string goes to `cmd /c` (exec/execSync semantics: the caller wrote a shell
// line and wants pipes, redirects and builtins). When it is non-null the
// entries are the literal argv — argv[0] is the executable — and the child is
// started directly, so nothing in a filename can be read as shell syntax.
// That is the split Node draws between exec and execFile/spawn, and it matters:
// argv here routinely holds user-chosen media paths.
static ExecResult runCommand(const std::string& command, const std::string& cwd,
                             const std::string& input, int timeoutMs, int maxBuffer,
                             const EnvList* env,
                             const std::vector<std::string>* argv = nullptr)
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

    // Build the command line. With argv, quote each entry and start the
    // executable directly; without it, hand the string to the shell.
    std::string cmdLine;
    if (argv && !argv->empty()) {
        for (const auto& a : *argv) {
            if (!cmdLine.empty()) cmdLine += " ";
            cmdLine += quoteArg(a);
        }
    } else {
        cmdLine = "cmd /c " + command;
    }

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

// See the Windows overload above for what `argv` means: null runs `command`
// through /bin/sh, non-null execs argv[0] directly with no shell in between.
static ExecResult runCommand(const std::string& command, const std::string& cwd,
                             const std::string& input, int timeoutMs, int maxBuffer,
                             const EnvList* env,
                             const std::vector<std::string>* argv = nullptr)
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

        // env REPLACES the child environment, so install it before exec.
        // Assigning `environ` rather than using execve/execvpe keeps one exec
        // path for both the shell and the argv case (execvpe is glibc-only).
        std::vector<std::string> envStrs;
        std::vector<char*> envp;
        if (env) {
            envStrs = buildEnvStrings(*env);
            for (auto& s : envStrs) envp.push_back(const_cast<char*>(s.c_str()));
            envp.push_back(nullptr);
            environ = envp.data();
        }

        if (argv && !argv->empty()) {
            std::vector<char*> cargv;
            cargv.reserve(argv->size() + 1);
            for (const auto& a : *argv) cargv.push_back(const_cast<char*>(a.c_str()));
            cargv.push_back(nullptr);
            execvp(cargv[0], cargv.data());   // PATH search, no shell
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
// Idle poll interval, and the most one read pass will take at once. The cap
// bounds the reader's scratch buffer; it must stay well above a video frame's
// worth of bytes or high-rate streams pay an extra wakeup per frame.
static constexpr int      kIdleSleepMs  = 1;
static constexpr uint32_t kMaxReadBytes = 4u * 1024u * 1024u;

// Kernel buffer for the child's stdout/stderr pipes. The CreatePipe default is
// a few KB, which forces a read pass (and, when the pipe drains, an idle sleep)
// every few KB. A megabyte lets a burst land in one pass and keeps the child
// from blocking in write() between our wakeups.
static constexpr uint32_t kPipeBufferBytes = 1u * 1024u * 1024u;

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
            // Only idle-wait when the pipe is genuinely empty. Sleeping after a
            // successful read is what caps throughput: a 2 ms pause per read
            // pass puts a hard ~30 MB/s ceiling on the stream, far below the
            // ~237 MB/s a 1080p30 rawvideo feed needs.
            std::this_thread::sleep_for(std::chrono::milliseconds(kIdleSleepMs));
            continue;
        }

        // Wait for room BEFORE reading, so unread bytes stay in the kernel
        // pipe buffer and the child blocks in write() — real backpressure
        // rather than an unbounded parent-side queue.
        size_t room = 0;
        {
            std::unique_lock<std::mutex> lock(buf->m);
            buf->cv.wait(lock, [&] {
                return buf->data.size() < buf->highWater ||
                       closing->load(std::memory_order_acquire);
            });
            if (closing->load(std::memory_order_acquire)) break;
            room = buf->highWater - buf->data.size();
        }

        // Take everything the pipe has, bounded by the headroom under
        // highWaterMark (so one big read can't blow past the caller's cap) and
        // by kMaxReadBytes (so one burst can't balloon the scratch buffer).
        // Capping at 64 KB here throttled the stream to one small read per
        // wakeup, which is what held rawvideo to a fraction of its needed rate.
        if (avail > kMaxReadBytes) avail = kMaxReadBytes;
        if (static_cast<size_t>(avail) > room) avail = static_cast<DWORD>(room);
        chunk.resize(avail);
        DWORD got = 0;
        if (!ReadFile(h, chunk.data(), avail, &got, nullptr) || got == 0) break;

        {
            std::lock_guard<std::mutex> lock(buf->m);
            buf->data.insert(buf->data.end(), chunk.begin(), chunk.begin() + got);
        }
        // Loop straight back to Peek — no sleep — so a saturated pipe is
        // drained at memory speed rather than one chunk per timer tick.
    }
    std::lock_guard<std::mutex> lock(buf->m);
    buf->eof = true;
}
#else
static void pipeReader(int fd, PipeBuf* buf, std::atomic<bool>* closing)
{
    // Heap, not stack: this is deliberately far larger than a page so a
    // saturated pipe drains in few syscalls. poll() returns as soon as POLLIN
    // is set, so unlike the Windows path there is no per-read idle cost — the
    // timeout below only bounds how often `closing` is re-checked.
    std::vector<uint8_t> chunkBuf(256u * 1024u);
    uint8_t* chunk = chunkBuf.data();
    const size_t chunkSize = chunkBuf.size();
    for (;;) {
        if (closing->load(std::memory_order_acquire)) break;

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = ::poll(&pfd, 1, kIdleSleepMs);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        size_t room = 0;
        {
            std::unique_lock<std::mutex> lock(buf->m);
            buf->cv.wait(lock, [&] {
                return buf->data.size() < buf->highWater ||
                       closing->load(std::memory_order_acquire);
            });
            if (closing->load(std::memory_order_acquire)) break;
            room = buf->highWater - buf->data.size();
        }

        // Bound the read by the headroom under highWaterMark so a single read
        // can't overshoot the caller's cap.
        ssize_t got = ::read(fd, chunk, std::min(chunkSize, room));
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
    // spawn/spawnSync: run the argv through a shell instead of exec'ing it
    // directly. Off by default (Node semantics) so an argument can never be
    // read as shell syntax; opt in when you actually want builtins/redirects.
    bool shell = false;
    bool hasEnv = false;
    EnvList env;            // replaces the child environment when hasEnv
    std::string stdoutFile; // spawn only: redirect child stdout to this file
    std::string stderrFile; // spawn only: redirect child stderr (may equal stdoutFile)
    bool pipeStdio = false; // spawn only: stdio:'pipe' — stream stdout/stderr, writable stdin
    int highWaterMark = 8 * 1024 * 1024; // spawn only: per-stream backpressure threshold
};

static ExecOptions parseOptions(std::span<const bronze::Value> a, size_t optIdx)
{
    ExecOptions opts;
    if (optIdx >= a.size() || !ev::isObject(a[optIdx])) return opts;

    // A reference into the rooted args span: stays current across the
    // allocating Object.keys call below, where a copy would go stale.
    const bronze::Value& val = a[optIdx];

    bronze::Value cwdV = ev::getProperty(val, "cwd");
    if (ev::isString(cwdV)) opts.cwd = ev::toUtf8(cwdV);

    bronze::Value encV = ev::getProperty(val, "encoding");
    if (ev::isString(encV)) opts.encoding = ev::toUtf8(encV);
    else if (ev::isNull(encV)) opts.encoding = "buffer";

    bronze::Value toV = ev::getProperty(val, "timeout");
    if (ev::isDouble(toV)) opts.timeout = static_cast<int>(ev::toDouble(toV));

    bronze::Value shV = ev::getProperty(val, "shell");
    if (ev::isBool(shV)) opts.shell = ev::toBool(shV);

    bronze::Value mbV = ev::getProperty(val, "maxBuffer");
    if (ev::isDouble(mbV)) opts.maxBuffer = static_cast<int>(ev::toDouble(mbV));

    bronze::Value inV = ev::getProperty(val, "input");
    if (ev::isString(inV)) opts.input = ev::toUtf8(inV);

    ev::Persistent envV{ev::getProperty(val, "env")};
    if (ev::isObject(envV.get())) {
        opts.hasEnv = true;
        ev::Persistent objCtor{ev::getGlobal("Object")};
        ev::Persistent keysFn{ev::getProperty(objCtor.get(), "keys")};
        auto r = ev::call(keysFn.get(), objCtor.get(), std::array<bronze::Value, 1>{envV.get()});
        ev::Persistent keys{r.thrown ? ev::undefined() : r.value};
        if (ev::isObject(keys.get())) {
            bronze::Value lenV = ev::getProperty(keys.get(), "length");
            if (ev::isDouble(lenV)) {
                uint32_t count = static_cast<uint32_t>(ev::toDouble(lenV));
                for (uint32_t i = 0; i < count; ++i) {
                    bronze::Value k = ev::getElement(keys.get(), i);
                    if (ev::isString(k)) {
                        std::string kStr = ev::toUtf8(k);
                        bronze::Value v = ev::getProperty(envV.get(), kStr);
                        if (!ev::isUndefined(v) && !ev::isNull(v)) {
                            opts.env.emplace_back(kStr, ev::toUtf8(v));
                        }
                    }
                }
            }
        }
    }

    bronze::Value soV = ev::getProperty(val, "stdoutFile");
    if (ev::isString(soV)) opts.stdoutFile = ev::toUtf8(soV);

    bronze::Value seV = ev::getProperty(val, "stderrFile");
    if (ev::isString(seV)) opts.stderrFile = ev::toUtf8(seV);

    bronze::Value stdioV = ev::getProperty(val, "stdio");
    if (ev::isString(stdioV)) opts.pipeStdio = (ev::toUtf8(stdioV) == "pipe");

    bronze::Value hwmV = ev::getProperty(val, "highWaterMark");
    if (ev::isDouble(hwmV)) {
        opts.highWaterMark = static_cast<int>(ev::toDouble(hwmV));
        if (opts.highWaterMark < 4096) opts.highWaterMark = 4096;
    }

    return opts;
}

static bronze::Value stringToOutput(const std::string& data, const std::string& encoding)
{
    if (encoding == "buffer") {
        bronze::Value v = ev::createTypedArray(elements::Uint8, static_cast<uint32_t>(data.size()));
        ev::fillTypedArray(v, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
        return v;
    }
    return ev::fromUtf8(data);
}

// execSync(command[, options])
static bronze::Value js_execSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("execSync: command required");

    std::string command = ev::toUtf8(a[0]);
    auto opts = parseOptions(a, 1);

    ExecResult res = runCommand(command, opts.cwd, opts.input, opts.timeout, opts.maxBuffer,
                                opts.hasEnv ? &opts.env : nullptr);

    if (!res.error.empty()) {
        return ev::throwTypeError(("execSync: " + res.error).c_str());
    }

    if (res.timedOut) {
        ObjectBuilder err;
        err.set("message", ev::fromUtf8("Command timed out"));
        err.set("code", ev::fromUtf8("ETIMEDOUT"));
        err.set("killed", ev::fromBool(true));
        err.set("stdout", stringToOutput(res.stdoutData, opts.encoding));
        err.set("stderr", stringToOutput(res.stderrData, opts.encoding));
        return ev::throwValue(err.get());
    }

    if (res.exitCode != 0) {
        ObjectBuilder err;
        std::string msg = "Command failed: " + command;
        err.set("message", ev::fromUtf8(msg));
        err.set("status", ev::fromDouble(res.exitCode));
        err.set("stdout", stringToOutput(res.stdoutData, opts.encoding));
        err.set("stderr", stringToOutput(res.stderrData, opts.encoding));
        err.set("code", ev::fromUtf8("ERR_CHILD_PROCESS"));
        return ev::throwValue(err.get());
    }

    return stringToOutput(res.stdoutData, opts.encoding);
}

// __brokit_cp_exec(command, options)
static bronze::Value js_cp_exec(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("exec: command required");

    std::string command = ev::toUtf8(a[0]);
    auto opts = parseOptions(a, 1);

    ExecResult res = runCommand(command, opts.cwd, opts.input, opts.timeout, opts.maxBuffer,
                                opts.hasEnv ? &opts.env : nullptr);

    ObjectBuilder obj;
    obj.set("stdout", stringToOutput(res.stdoutData, opts.encoding));
    obj.set("stderr", stringToOutput(res.stderrData, opts.encoding));
    obj.set("exitCode", ev::fromDouble(res.exitCode));
    if (!res.error.empty())
        obj.set("error", ev::fromUtf8(res.error));
    obj.set("timedOut", ev::fromBool(res.timedOut));

    return obj.get();
}

// spawnSync(command, args[, options])
static bronze::Value js_spawnSync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("spawnSync: command required");

    std::string command = ev::toUtf8(a[0]);

    std::vector<std::string> childArgv{ command };
    if (a.size() >= 2 && ev::isObject(a[1])) {
        bronze::Value lenVal = ev::getProperty(a[1], "length");
        if (ev::isDouble(lenVal)) {
            uint32_t len = static_cast<uint32_t>(ev::toDouble(lenVal));
            for (uint32_t i = 0; i < len; i++) {
                bronze::Value elem = ev::getElement(a[1], i);
                if (ev::isString(elem)) {
                    childArgv.push_back(ev::toUtf8(elem));
                }
            }
        }
    }

    size_t optIdx = (a.size() >= 2 && ev::isObject(a[1]) && ev::isDouble(ev::getProperty(a[1], "length"))) ? 2 : 1;
    auto opts = parseOptions(a, optIdx);

    if (opts.shell) {
        for (size_t i = 1; i < childArgv.size(); i++) {
            command += " ";
            command += childArgv[i].find_first_of(" \t") != std::string::npos
                           ? "\"" + childArgv[i] + "\""
                           : childArgv[i];
        }
    }

    ExecResult res = runCommand(command, opts.cwd, opts.input, opts.timeout, opts.maxBuffer,
                                opts.hasEnv ? &opts.env : nullptr,
                                opts.shell ? nullptr : &childArgv);

    ObjectBuilder obj;
    obj.set("stdout", stringToOutput(res.stdoutData, opts.encoding));
    obj.set("stderr", stringToOutput(res.stderrData, opts.encoding));
    obj.set("status", res.exitCode >= 0 ? ev::fromDouble(res.exitCode) : ev::null());
    obj.set("signal", res.timedOut ? ev::fromUtf8("SIGKILL") : ev::null());

    if (!res.error.empty()) {
        // Node reports a spawn failure on the result rather than throwing.
        obj.set("error", newError("Error", res.error));
    }

    return obj.get();
}

// __brokit_cp_spawnAsync(file, args, options)
static bronze::Value js_spawnAsync(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("spawnAsync: file required");

    std::string file = ev::toUtf8(a[0]);

    std::vector<std::string> args;
    if (a.size() >= 2 && ev::isObject(a[1])) {
        bronze::Value lenVal = ev::getProperty(a[1], "length");
        if (ev::isDouble(lenVal)) {
            uint32_t len = static_cast<uint32_t>(ev::toDouble(lenVal));
            for (uint32_t i = 0; i < len; i++) {
                bronze::Value elem = ev::getElement(a[1], i);
                if (ev::isString(elem)) {
                    args.push_back(ev::toUtf8(elem));
                }
            }
        }
    }

    size_t optIdx = (a.size() >= 2 && ev::isObject(a[1]) && ev::isDouble(ev::getProperty(a[1], "length"))) ? 2 : 1;
    auto opts = parseOptions(a, optIdx);

    auto handle = std::make_unique<ChildHandle>();

#ifdef _WIN32
    std::string cmdLine;
    if (opts.shell) {
        cmdLine = "cmd /c " + file;
        for (auto& arg : args) { cmdLine += " "; cmdLine += arg; }
    } else {
        cmdLine = quoteArg(file);
        for (auto& arg : args) { cmdLine += " "; cmdLine += quoteArg(arg); }
    }

    std::string envBlock;
    if (opts.hasEnv) envBlock = buildEnvBlock(opts.env);

    STARTUPINFOEXA six = {};
    six.StartupInfo.cb = sizeof(six);
    PROCESS_INFORMATION pi = {};

    HANDLE hOut = nullptr, hErr = nullptr, hIn = nullptr;
    BOOL inheritHandles = FALSE;
    std::vector<uint8_t> attrBuf;
    bool haveAttrList = false;
    if (opts.pipeStdio) {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
        DWORD pipeBufSize = static_cast<DWORD>(kPipeBufferBytes);
        if (!CreatePipe(&handle->outRead, &hOut, &sa, pipeBufSize) ||
            !CreatePipe(&handle->errRead, &hErr, &sa, pipeBufSize) ||
            !CreatePipe(&hIn, &handle->inWrite, &sa, pipeBufSize)) {
            if (hOut) CloseHandle(hOut);
            if (hErr) CloseHandle(hErr);
            if (hIn) CloseHandle(hIn);
            return ev::throwTypeError("spawn: cannot create stdio pipes");
        }
        SetHandleInformation(handle->outRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(handle->errRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(handle->inWrite, HANDLE_FLAG_INHERIT, 0);

        six.StartupInfo.hStdOutput = hOut;
        six.StartupInfo.hStdError = hErr;
        six.StartupInfo.hStdInput = hIn;
        six.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        inheritHandles = TRUE;

        HANDLE handlesToInherit[3] = { hOut, hErr, hIn };
        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        attrBuf.resize(attrSize);
        six.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
        if (InitializeProcThreadAttributeList(six.lpAttributeList, 1, 0, &attrSize) &&
            UpdateProcThreadAttribute(six.lpAttributeList, 0,
                                      PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      handlesToInherit, sizeof(handlesToInherit),
                                      nullptr, nullptr)) {
            haveAttrList = true;
        }
    }

    DWORD creationFlags = CREATE_NO_WINDOW;
    if (opts.hasEnv) creationFlags |= CREATE_UNICODE_ENVIRONMENT;
    if (haveAttrList) creationFlags |= EXTENDED_STARTUPINFO_PRESENT;

    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    BOOL ok = CreateProcessA(
        nullptr, cmdBuf.data(),
        nullptr, nullptr, inheritHandles,
        creationFlags,
        opts.hasEnv ? const_cast<char*>(envBlock.data()) : nullptr,
        opts.cwd.empty() ? nullptr : opts.cwd.c_str(),
        haveAttrList ? reinterpret_cast<STARTUPINFOA*>(&six) : &six.StartupInfo,
        &pi);

    DWORD createErr = ok ? 0 : GetLastError();
    if (haveAttrList) DeleteProcThreadAttributeList(six.lpAttributeList);
    if (hOut && hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
    if (hErr && hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
    if (hIn && hIn != INVALID_HANDLE_VALUE) CloseHandle(hIn);

    if (!ok) {
        char errBuf[256];
        snprintf(errBuf, sizeof(errBuf), "spawn failed: CreateProcess error %lu", createErr);
        return ev::throwTypeError(errBuf);
    }
    CloseHandle(pi.hThread);
    handle->process = pi.hProcess;
    handle->pid = pi.dwProcessId;
#else
    int outPipe[2] = {-1, -1}, errPipe[2] = {-1, -1}, inPipe[2] = {-1, -1};
    if (opts.pipeStdio) {
        if (pipe(outPipe) != 0 || pipe(errPipe) != 0 || pipe(inPipe) != 0) {
            for (int fd : { outPipe[0], outPipe[1], errPipe[0], errPipe[1],
                            inPipe[0], inPipe[1] })
                if (fd >= 0) ::close(fd);
            return ev::throwTypeError("spawn: cannot create stdio pipes");
        }
        handle->outRead = outPipe[0];
        handle->errRead = errPipe[0];
        handle->inWrite = inPipe[1];
#ifdef __linux__
        fcntl(outPipe[0], F_SETPIPE_SZ, static_cast<int>(kPipeBufferBytes));
        fcntl(errPipe[0], F_SETPIPE_SZ, static_cast<int>(kPipeBufferBytes));
        fcntl(inPipe[1],  F_SETPIPE_SZ, static_cast<int>(kPipeBufferBytes));
#endif
    }
    auto closeChildEnds = [&]() {
        if (!opts.pipeStdio) return;
        if (outPipe[1] >= 0) { ::close(outPipe[1]); outPipe[1] = -1; }
        if (errPipe[1] >= 0) { ::close(errPipe[1]); errPipe[1] = -1; }
        if (inPipe[0]  >= 0) { ::close(inPipe[0]);  inPipe[0]  = -1; }
    };

    int execPipe[2] = {-1, -1};
    if (pipe(execPipe) != 0) {
        closeChildEnds();
        return ev::throwTypeError("spawn failed: pipe");
    }
    fcntl(execPipe[1], F_SETFD, fcntl(execPipe[1], F_GETFD) | FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close(execPipe[0]);
        close(execPipe[1]);
        closeChildEnds();
        return ev::throwTypeError("spawn failed: fork");
    }
    if (pid == 0) {
        close(execPipe[0]);
        if (opts.pipeStdio) {
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
        std::string shellLine;
        std::vector<char*> cArgv;
        if (opts.shell) {
            shellLine = file;
            for (auto& aItem : args) { shellLine += " "; shellLine += aItem; }
            cArgv.push_back(const_cast<char*>("sh"));
            cArgv.push_back(const_cast<char*>("-c"));
            cArgv.push_back(const_cast<char*>(shellLine.c_str()));
        } else {
            cArgv.push_back(const_cast<char*>(file.c_str()));
            for (auto& aItem : args) cArgv.push_back(const_cast<char*>(aItem.c_str()));
        }
        cArgv.push_back(nullptr);
        std::vector<std::string> envStrs;
        std::vector<char*> envp;
        if (opts.hasEnv) {
            envStrs = buildEnvStrings(opts.env);
            for (auto& s : envStrs) envp.push_back(const_cast<char*>(s.c_str()));
            envp.push_back(nullptr);
            environ = envp.data();
        }
        execvp(opts.shell ? "/bin/sh" : file.c_str(), cArgv.data());
        int execErrno = errno;
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

    closeChildEnds();

    close(execPipe[1]);
    int childErrno = 0;
    ssize_t got = read(execPipe[0], &childErrno, sizeof(childErrno));
    close(execPipe[0]);
    if (got == static_cast<ssize_t>(sizeof(childErrno)) && childErrno != 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        return ev::throwTypeError(("spawn failed: " + file + ": " + strerror(childErrno)).c_str());
    }
#endif

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

    ObjectBuilder obj;
    obj.set("id", ev::fromDouble(id));
    obj.set("pid", ev::fromDouble(pidVal));
    obj.set("piped", ev::fromBool(piped));
    return obj.get();
}

// __brokit_cp_childPoll(id)
static bronze::Value js_childPoll(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("childPoll: id required");
    int id = i32At(a, 0);

    ChildHandle* h = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_childMutex);
        auto it = g_children.find(id);
        if (it == g_children.end()) {
            return ev::throwRangeError(("childPoll: unknown child id " + std::to_string(id)).c_str());
        }
        h = it->second.get();
    }

    auto exitInfo = [&](int code, const std::string& sig) {
        ObjectBuilder obj;
        obj.set("exitCode", ev::fromDouble(code));
        if (sig.empty()) obj.set("signal", ev::null());
        else             obj.set("signal", ev::fromUtf8(sig));
        return obj.get();
    };

    if (h->exitReported) return exitInfo(h->exitCode, h->signal);

#ifdef _WIN32
    DWORD status = WaitForSingleObject(h->process, 0);
    if (status == WAIT_TIMEOUT) {
        return ev::null();
    }
    DWORD code = 0;
    GetExitCodeProcess(h->process, &code);
    CloseHandle(h->process);
    h->process = nullptr;
    int exitCode = (int)code;
    std::string sig;
#else
    int status = 0;
    pid_t r = waitpid(h->pid, &status, WNOHANG);
    if (r == 0) return ev::null();
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

    bronze::Value obj = exitInfo(exitCode, sig);

    if (!h->piped) {
        std::lock_guard<std::mutex> lock(g_childMutex);
        g_children.erase(id);
    }
    return obj;
}

// __brokit_cp_childRead(id)
static bronze::Value js_childRead(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("childRead: id required");
    int id = i32At(a, 0);

    ChildHandle* h = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_childMutex);
        auto it = g_children.find(id);
        if (it == g_children.end())
            return ev::throwRangeError(("childRead: unknown child id " + std::to_string(id)).c_str());
        h = it->second.get();
    }
    if (!h->piped)
        return ev::throwTypeError(("childRead: child " + std::to_string(id) + " was not spawned with stdio:'pipe'").c_str());

    // Each chunk is stored on the rooted result the moment it is made: a raw
    // Value held while the next one allocates would go stale.
    ObjectBuilder obj;
    auto take = [&](PipeBuf& buf, std::string_view key, bool& eofOut) {
        std::vector<uint8_t> drained;
        {
            std::lock_guard<std::mutex> lock(buf.m);
            drained.swap(buf.data);
            eofOut = buf.eof;
        }
        if (!drained.empty()) buf.cv.notify_all();
        obj.set(key, drained.empty()
            ? ev::null()
            : stringToOutput(std::string(reinterpret_cast<const char*>(drained.data()), drained.size()), "buffer"));
    };

    bool outEof = false, errEof = false;
    take(h->out, "stdout", outEof);
    take(h->err, "stderr", errEof);

    obj.set("stdoutEof", ev::fromBool(outEof));
    obj.set("stderrEof", ev::fromBool(errEof));
    return obj.get();
}

// __brokit_cp_childWrite(id, data)
static bronze::Value js_childWrite(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.size() < 2) return ev::throwTypeError("childWrite: id and data required");
    int id = i32At(a, 0);

    ChildHandle* h = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_childMutex);
        auto it = g_children.find(id);
        if (it == g_children.end())
            return ev::throwRangeError(("childWrite: unknown child id " + std::to_string(id)).c_str());
        h = it->second.get();
    }
    if (!h->piped)
        return ev::throwTypeError(("childWrite: child " + std::to_string(id) + " was not spawned with stdio:'pipe'").c_str());

    const uint8_t* bytes = nullptr;
    size_t len = 0;
    std::string tmp;
    if (auto info = ev::typedArrayInfo(a[1])) {
        bytes = info.data;
        len = info.byteLength;
    } else if (auto infoAb = ev::arrayBufferInfo(a[1])) {
        bytes = infoAb.data;
        len = infoAb.byteLength;
    } else {
        tmp = ev::toUtf8(a[1]);
        bytes = reinterpret_cast<const uint8_t*>(tmp.data());
        len = tmp.size();
    }
    if (!bytes) return ev::fromDouble(0);

    std::lock_guard<std::mutex> lock(h->stdinMutex);
    size_t written = 0;
#ifdef _WIN32
    if (!h->inWrite) return ev::fromDouble(-1);
    while (written < len) {
        DWORD n = 0;
        if (!WriteFile(h->inWrite, bytes + written,
                       static_cast<DWORD>(len - written), &n, nullptr) || n == 0)
            break;
        written += n;
    }
#else
    if (h->inWrite < 0) return ev::fromDouble(-1);
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
    return ev::fromDouble(static_cast<double>(written));
}

// __brokit_cp_childCloseStdin(id)
static bronze::Value js_childCloseStdin(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("childCloseStdin: id required");
    int id = i32At(a, 0);

    std::lock_guard<std::mutex> lock(g_childMutex);
    auto it = g_children.find(id);
    if (it == g_children.end()) return ev::fromBool(false);
    it->second->closeStdin();
    return ev::fromBool(true);
}

// __brokit_cp_childRelease(id)
static bronze::Value js_childRelease(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("childRelease: id required");
    int id = i32At(a, 0);

    std::unique_ptr<ChildHandle> doomed;
    {
        std::lock_guard<std::mutex> lock(g_childMutex);
        auto it = g_children.find(id);
        if (it == g_children.end()) return ev::fromBool(false);
        doomed = std::move(it->second);
        g_children.erase(it);
    }
    return ev::fromBool(true);
}

// __brokit_cp_childKill(id, signal?)
static bronze::Value js_childKill(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.empty()) return ev::throwTypeError("childKill: id required");
    int id = i32At(a, 0);

    std::lock_guard<std::mutex> lock(g_childMutex);
    auto it = g_children.find(id);
    if (it == g_children.end()) return ev::fromBool(false);
    ChildHandle* h = it->second.get();
#ifdef _WIN32
    TerminateProcess(h->process, 1);
#else
    int sig = SIGTERM;
    if (a.size() >= 2 && ev::isString(a[1])) {
        std::string s = ev::toUtf8(a[1]);
        if (s == "SIGKILL") sig = SIGKILL;
        else if (s == "SIGINT") sig = SIGINT;
    }
    ::kill(h->pid, sig);
#endif
    return ev::fromBool(true);
}

void installChildProcess()
{
    ev::setGlobalFunction("__brokit_cp_execSync", 2, js_execSync);
    ev::setGlobalFunction("__brokit_cp_exec", 2, js_cp_exec);
    ev::setGlobalFunction("__brokit_cp_spawnSync", 3, js_spawnSync);
    ev::setGlobalFunction("__brokit_cp_spawnAsync", 3, js_spawnAsync);
    ev::setGlobalFunction("__brokit_cp_childPoll", 1, js_childPoll);
    ev::setGlobalFunction("__brokit_cp_childKill", 2, js_childKill);
    ev::setGlobalFunction("__brokit_cp_childRead", 1, js_childRead);
    ev::setGlobalFunction("__brokit_cp_childWrite", 2, js_childWrite);
    ev::setGlobalFunction("__brokit_cp_childCloseStdin", 1, js_childCloseStdin);
    ev::setGlobalFunction("__brokit_cp_childRelease", 1, js_childRelease);

    bronze::embed::runEntry(bronze_child_process_main);
}

} // namespace brokit::api
