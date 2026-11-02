#pragma once

#include "api/api.h"
#include "api/access_policy.h"
#include "api/arg_reader.h"
#include "api/object_builder.h"
#include "api/signals.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

namespace brokit::api {

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

    // Held under the access gate while it runs (access_policy.h); a grant
    // taken back kills it, and its exit reports the refusal.
    uint64_t held = 0;
    std::string revokedBy;

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
        releaseHeldAccess(held);
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
        if (!exitReported && pid > 0) {
            int status = 0;
            waitpid(pid, &status, WNOHANG);
        }
#endif
    }
};

struct ExecResult {
    std::string stdoutData;
    std::string stderrData;
    int exitCode = -1;
    bool timedOut = false;
    std::string error;
};

// options.env, when present, REPLACES the child environment (Node semantics).
using EnvList = std::vector<std::pair<std::string, std::string>>;

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
    bool detached = false;  // spawn only: outlive the app (no job / PDEATHSIG, own session)
};

ExecOptions parseOptions(std::span<const bronze::Value> a, size_t optIdx);
bronze::Value stringToOutput(const std::string& data, const std::string& encoding);

#ifdef _WIN32
HANDLE appJob();
void startChild(const PROCESS_INFORMATION& pi, bool detached);
std::wstring widen(const std::string& s);
std::wstring buildEnvBlock(const EnvList& env);
std::string quoteArg(const std::string& s);
bool buildHandleList(std::vector<uint8_t>& attrBuf, const HANDLE* handles, size_t count);
#else
void dieWithParent(pid_t parent);
std::vector<std::string> buildEnvStrings(const EnvList& env);
int makeCloexecPipe(int p[2]);
#endif

// Async spawn bindings (child_process_spawn.cpp)
bronze::Value js_spawnAsync(bronze::Value, std::span<const bronze::Value> a);
bronze::Value js_childPoll(bronze::Value, std::span<const bronze::Value> a);
bronze::Value js_childRead(bronze::Value, std::span<const bronze::Value> a);
bronze::Value js_childWrite(bronze::Value, std::span<const bronze::Value> a);
bronze::Value js_childCloseStdin(bronze::Value, std::span<const bronze::Value> a);
bronze::Value js_childRelease(bronze::Value, std::span<const bronze::Value> a);
bronze::Value js_childKill(bronze::Value, std::span<const bronze::Value> a);

} // namespace brokit::api
