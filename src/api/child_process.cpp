#include "api/child_process_internal.h"

extern "C" void bronze_child_process_main();

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>

namespace brokit::api {

// ---------------------------------------------------------------------------
// Children end with the app
//
// Windows leaves a child running when its parent exits, and a long-running
// child whose stdout reader is gone can loop forever writing into a dead pipe
// (a PowerShell script with ErrorAction SilentlyContinue, typeperf, ...). So
// every child that is not `detached` goes into one job object created with
// KILL_ON_JOB_CLOSE. The job handle is never closed: the OS closes it when
// this process exits however it exits (clean quit, crash, TerminateProcess),
// which ends the children and every descendant they started. The child is
// created suspended and resumed only once it is in the job, so it cannot
// start a grandchild outside it. Linux gets the same through
// PR_SET_PDEATHSIG in the forked child.
// ---------------------------------------------------------------------------
#ifdef _WIN32
HANDLE appJob()
{
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        if (!j) return HANDLE(nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
        // BREAKAWAY_OK: a child that is itself bro can still start a
        // detached process that outlives both.
        info.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        if (!SetInformationJobObject(j, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
            CloseHandle(j);
            return HANDLE(nullptr);
        }
        return j;
    }();
    return job;
}

// Start a child created with CREATE_SUSPENDED: into the app job unless
// detached, then running. A failed assignment (no job) still runs the child.
void startChild(const PROCESS_INFORMATION& pi, bool detached)
{
    if (!detached) {
        if (HANDLE job = appJob()) AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
}
#else
// In the forked child, before exec: die with the parent. PDEATHSIG is Linux
// only (and fires when the forking thread exits); the getppid() check catches
// a parent that exited between fork and prctl.
void dieWithParent(pid_t parent)
{
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != parent) _exit(127);
#else
    (void)parent;
#endif
}
#endif

// ---------------------------------------------------------------------------
// Helper: run a command and capture stdout/stderr
// ---------------------------------------------------------------------------

#ifdef _WIN32
// Script strings are UTF-8; the process APIs take UTF-16 (the W functions), so
// a non-ASCII path, argument or variable reaches the child intact rather than
// through the ANSI code page.
std::wstring widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Double-NUL-terminated "KEY=VALUE\0" block (UTF-16, CREATE_UNICODE_ENVIRONMENT).
std::wstring buildEnvBlock(const EnvList& env)
{
    std::wstring block;
    for (const auto& [k, v] : env) {
        block += widen(k);
        block += L'=';
        block += widen(v);
        block += L'\0';
    }
    block += L'\0';
    return block;
}
#else
extern "C" char** environ;

// "KEY=VALUE" strings + char* view for execve-family calls.
std::vector<std::string> buildEnvStrings(const EnvList& env)
{
    std::vector<std::string> out;
    out.reserve(env.size());
    for (const auto& [k, v] : env) out.push_back(k + "=" + v);
    return out;
}
#endif

#ifdef _WIN32

// Quote one argv entry for a CreateProcess command line so the child's
// CommandLineToArgvW / C runtime reads back exactly `s`. An empty argument is
// "" (left bare it would vanish); backslashes are literal except before a
// quote, where they are doubled, as they are before the closing quote. An
// argument with no space, tab or quote passes through, so a plain path stays
// readable in a process listing.
std::string quoteArg(const std::string& s)
{
    if (!s.empty() && s.find_first_of(" \t\n\v\"") == std::string::npos) return s;
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : s) {
        if (c == '\\') { ++backslashes; continue; }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
        } else {
            out.append(backslashes, '\\');
            out += c;
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

// Build a PROC_THREAD_ATTRIBUTE_HANDLE_LIST restricting inheritance to exactly
// the given handles. bInheritHandles=TRUE alone leaks EVERY inheritable handle
// in the process into the child — a concurrently spawned child then inherits
// another child's pipe write end, and that pipe's ReadFile never sees EOF
// until the unrelated child exits (the classic cross-spawn EOF hang).
// Returns false (with attrBuf left empty) if the attribute list cannot be
// built; callers then fall back to plain inheritance.
bool buildHandleList(std::vector<uint8_t>& attrBuf,
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

    STARTUPINFOEXW six = {};
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

    std::wstring envBlock;
    if (env) envBlock = buildEnvBlock(*env);
    std::wstring wCmdLine = widen(cmdLine);
    wCmdLine.push_back(L'\0');
    const std::wstring wCwd = widen(cwd);

    BOOL ok = CreateProcessW(
        nullptr,
        wCmdLine.data(),
        nullptr, nullptr,
        TRUE, // inherit handles (limited by the attribute list when present)
        CREATE_NO_WINDOW | CREATE_SUSPENDED | (haveAttrList ? EXTENDED_STARTUPINFO_PRESENT : 0) |
            (env ? CREATE_UNICODE_ENVIRONMENT : 0),
        env ? const_cast<wchar_t*>(envBlock.data()) : nullptr,
        wCwd.empty() ? nullptr : wCwd.c_str(),
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
    startChild(pi, false);

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

int makeCloexecPipe(int p[2])
{
#ifdef __linux__
    if (pipe2(p, O_CLOEXEC) == 0) return 0;
#endif
    if (pipe(p) != 0) return -1;
    fcntl(p[0], F_SETFD, fcntl(p[0], F_GETFD) | FD_CLOEXEC);
    fcntl(p[1], F_SETFD, fcntl(p[1], F_GETFD) | FD_CLOEXEC);
    return 0;
}

// See the Windows overload above for what `argv` means: null runs `command`
// through /bin/sh, non-null execs argv[0] directly with no shell in between.
static ExecResult runCommand(const std::string& command, const std::string& cwd,
                             const std::string& input, int timeoutMs, int maxBuffer,
                             const EnvList* env,
                             const std::vector<std::string>* argv = nullptr)
{
    ExecResult result;

    int stdoutPipe[2] = {-1, -1}, stderrPipe[2] = {-1, -1}, stdinPipe[2] = {-1, -1};
    auto closeAllPipes = [&]() {
        for (int fd : { stdoutPipe[0], stdoutPipe[1], stderrPipe[0], stderrPipe[1],
                        stdinPipe[0], stdinPipe[1] }) {
            if (fd >= 0) ::close(fd);
        }
    };
    if (makeCloexecPipe(stdoutPipe) != 0 ||
        makeCloexecPipe(stderrPipe) != 0 ||
        makeCloexecPipe(stdinPipe) != 0) {
        closeAllPipes();
        result.error = "Failed to create pipes";
        return result;
    }

    const pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        closeAllPipes();
        result.error = "Failed to fork";
        return result;
    }

    if (pid == 0) {
        // Child
        dieWithParent(parent);
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
    char buf[16384];

    auto drainFd = [&](int fd, std::string& out, bool& done) {
        while (!done) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                if (maxBuffer > 0) {
                    if (out.size() < static_cast<size_t>(maxBuffer)) {
                        size_t take = std::min(static_cast<size_t>(n),
                                               static_cast<size_t>(maxBuffer) - out.size());
                        out.append(buf, take);
                    }
                } else {
                    out.append(buf, static_cast<size_t>(n));
                }
            } else if (n == 0) {
                done = true;
            } else {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                done = true;
            }
        }
    };

    while (!stdoutDone || !stderrDone) {
        int pollWaitMs = 50;
        if (timeoutMs > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count();
            if (elapsed >= timeoutMs) {
                kill(pid, SIGKILL);
                result.timedOut = true;
                break;
            }
            pollWaitMs = static_cast<int>(std::min<long long>(pollWaitMs, timeoutMs - elapsed));
        }

        drainFd(stdoutPipe[0], result.stdoutData, stdoutDone);
        drainFd(stderrPipe[0], result.stderrData, stderrDone);
        if (stdoutDone && stderrDone) break;

        struct pollfd pfds[2];
        nfds_t nfds = 0;
        if (!stdoutDone) {
            pfds[nfds].fd = stdoutPipe[0];
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }
        if (!stderrDone) {
            pfds[nfds].fd = stderrPipe[0];
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }
        if (nfds > 0) {
            poll(pfds, nfds, pollWaitMs);
        }
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

ExecOptions parseOptions(std::span<const bronze::Value> a, size_t optIdx)
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
    if (ev::isDouble(toV)) opts.timeout = saturateI32(ev::toDouble(toV));

    bronze::Value shV = ev::getProperty(val, "shell");
    if (ev::isBool(shV)) opts.shell = ev::toBool(shV);

    bronze::Value detV = ev::getProperty(val, "detached");
    if (ev::isBool(detV)) opts.detached = ev::toBool(detV);

    bronze::Value mbV = ev::getProperty(val, "maxBuffer");
    if (ev::isDouble(mbV)) opts.maxBuffer = saturateI32(ev::toDouble(mbV));

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
                uint32_t count = saturateU32(ev::toDouble(lenV));
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
    // Inheriting: the environment as process.env shows the program, with
    // the changes the access gate kept out of the real one (process.cpp).
    if (!opts.hasEnv && envOverlayForChild(opts.env)) opts.hasEnv = true;

    bronze::Value soV = ev::getProperty(val, "stdoutFile");
    if (ev::isString(soV)) opts.stdoutFile = ev::toUtf8(soV);

    bronze::Value seV = ev::getProperty(val, "stderrFile");
    if (ev::isString(seV)) opts.stderrFile = ev::toUtf8(seV);

    bronze::Value stdioV = ev::getProperty(val, "stdio");
    if (ev::isString(stdioV)) opts.pipeStdio = (ev::toUtf8(stdioV) == "pipe");

    bronze::Value hwmV = ev::getProperty(val, "highWaterMark");
    if (ev::isDouble(hwmV)) {
        opts.highWaterMark = saturateI32(ev::toDouble(hwmV));
        if (opts.highWaterMark < 4096) opts.highWaterMark = 4096;
    }

    return opts;
}

bronze::Value stringToOutput(const std::string& data, const std::string& encoding)
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
    if (auto r = checkSpawnAccess(command); !r.empty()) return throwAccessError(r, kSpawnDeniedCode);
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
    if (auto r = checkSpawnAccess(command); !r.empty()) return throwAccessError(r, kSpawnDeniedCode);
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
            uint32_t len = saturateU32(ev::toDouble(lenVal));
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

    // Thrown rather than reported on the result as a failed start is: the
    // program asked for something it may not do, which is not a spawn error.
    const std::string line = opts.shell
        ? command
        : commandLineFor(childArgv[0], std::vector<std::string>(childArgv.begin() + 1, childArgv.end()));
    if (auto r = checkSpawnAccess(line); !r.empty()) return throwAccessError(r, kSpawnDeniedCode);

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
