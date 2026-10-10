// fs.promises and the callback functions, asynchronous for real.
//
// A call resolves its path on the JS thread (mounts and base paths live in
// that thread's globals), copies what it writes, and queues the I/O on a
// small shared pool. The result, plain C++ data, waits in the calling
// thread's completion list until that thread's next __brokit_fs_async_tick
// turns it into JS values and settles the promise. So a page (or a Worker)
// that awaits a readdir of a slow disk keeps drawing frames meanwhile,
// where the old wrappers ran the synchronous call in place and only
// pretended, with an already-settled promise.

#include "api/fs_async.h"
#include "api/api.h"
#include "api/object_builder.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace brokit::api {

namespace {

fs::path u8p(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string u8s(const fs::path& p)
{
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// ---- results: plain data, built on a pool thread ------------------------------

struct FsError {
    std::string code, syscall, path, message;
};
struct StatOut {
    double size = 0, mtimeMs = 0, mode = 0;
    bool isFile = false, isDir = false, isLink = false;
};
struct DirentOut {
    std::string name;
    bool isFile = false, isDir = false, isLink = false;
};
struct Bytes { std::string data; };
struct Text { std::string data; };
struct Names { std::vector<std::string> names; };
struct Dirents { std::vector<DirentOut> entries; };

using Result = std::variant<std::monostate, FsError, Bytes, Text, StatOut, Names, Dirents>;

const char* codeFor(const std::error_code& ec)
{
    if (ec == std::errc::no_such_file_or_directory) return "ENOENT";
    if (ec == std::errc::file_exists) return "EEXIST";
    if (ec == std::errc::permission_denied) return "EACCES";
    if (ec == std::errc::is_a_directory) return "EISDIR";
    if (ec == std::errc::not_a_directory) return "ENOTDIR";
    if (ec == std::errc::directory_not_empty) return "ENOTEMPTY";
    if (ec == std::errc::no_space_on_device) return "ENOSPC";
    if (ec == std::errc::too_many_files_open) return "EMFILE";
    if (ec == std::errc::cross_device_link) return "EXDEV";
    return "ERR_FS";
}

FsError errorFrom(const char* syscall, const std::string& path, const std::error_code& ec)
{
    const char* code = codeFor(ec);
    return {code, syscall, path, std::string(code) + ": " + ec.message() + ", " + syscall + " '" + path + "'"};
}

FsError errorOf(const char* code, const char* syscall, const std::string& path, const std::string& what)
{
    return {code, syscall, path, std::string(code) + ": " + what + ", " + syscall + " '" + path + "'"};
}

double mtimeMsOf(const fs::path& p)
{
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    auto sys = std::chrono::time_point_cast<std::chrono::milliseconds>(
        t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return static_cast<double>(sys.time_since_epoch().count());
}

// stat (follow = true) or lstat, as the synchronous pair report them.
Result statPath(const std::string& path, bool follow)
{
    const fs::path p = u8p(path);
    std::error_code ec;
    const auto st = follow ? fs::status(p, ec) : fs::symlink_status(p, ec);
    if (ec) return errorFrom(follow ? "stat" : "lstat", path, ec);
    StatOut s;
    s.isFile = fs::is_regular_file(st);
    s.isDir = fs::is_directory(st);
    if (follow) {
        auto ls = fs::symlink_status(p, ec);
        s.isLink = !ec && fs::is_symlink(ls);
    } else {
        s.isLink = fs::is_symlink(st);
    }
    if (s.isFile) {
        auto size = fs::file_size(p, ec);
        s.size = ec ? 0.0 : static_cast<double>(size);
    }
    if (follow) {
        s.mtimeMs = mtimeMsOf(p);
#ifdef _WIN32
        DWORD attrs = GetFileAttributesW(p.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            int mode = 0444;
            if (!(attrs & FILE_ATTRIBUTE_READONLY)) mode |= 0222;
            if (s.isDir) mode |= 0111;
            s.mode = mode;
        }
#else
        struct stat raw;
        if (::stat(path.c_str(), &raw) == 0) s.mode = raw.st_mode & 07777;
#endif
    }
    return s;
}

Result readDir(const std::string& path, bool withFileTypes)
{
    std::error_code ec;
    fs::directory_iterator it(u8p(path), ec);
    if (ec) return errorFrom("scandir", path, ec);
    Names names;
    Dirents dirents;
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) return errorFrom("scandir", path, ec);
        const auto& entry = *it;
        std::string name = u8s(entry.path().filename());
        if (withFileTypes) {
            std::error_code e2;
            DirentOut d;
            d.name = std::move(name);
            d.isLink = entry.is_symlink(e2);
            d.isDir = !d.isLink && entry.is_directory(e2);
            d.isFile = !d.isLink && entry.is_regular_file(e2);
            dirents.entries.push_back(std::move(d));
        } else {
            names.names.push_back(std::move(name));
        }
    }
    if (withFileTypes) return dirents;
    return names;
}

Result readFile(const std::string& path, bool asText)
{
    std::error_code ec;
    if (fs::is_directory(u8p(path), ec)) return errorOf("EISDIR", "read", path, "illegal operation on a directory");
    std::ifstream f(u8p(path), std::ios::in | std::ios::binary);
    if (!f) return errorOf("ENOENT", "open", path, "no such file or directory");
    std::ostringstream ss;
    ss << f.rdbuf();
    if (asText) return Text{ss.str()};
    return Bytes{ss.str()};
}

Result writeFile(const std::string& path, const std::string& data, bool append)
{
    std::ofstream f(u8p(path), std::ios::out | std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!f) return errorOf("ENOENT", "open", path, "no such file or directory");
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!f) return errorOf("EIO", "write", path, "write failed");
    return std::monostate{};
}

// ---- the pool ---------------------------------------------------------------------

struct Job {
    std::thread::id owner;
    uint64_t id = 0;
    std::function<Result()> work;
};

struct Done {
    uint64_t id = 0;
    Result result;
};

class Pool {
public:
    void submit(Job job)
    {
        {
            std::lock_guard lock(mu_);
            if (!started_) start();
            queue_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

    std::vector<Done> take(std::thread::id owner)
    {
        std::vector<Done> out;
        std::lock_guard lock(doneMu_);
        auto it = done_.find(owner);
        if (it == done_.end() || it->second.empty()) return out;
        out.assign(std::make_move_iterator(it->second.begin()), std::make_move_iterator(it->second.end()));
        it->second.clear();
        return out;
    }

private:
    void start()
    {
        started_ = true;
        // Enough to keep a slow disk busy with several requests in flight;
        // few enough that a burst of calls does not flood the machine.
        const unsigned hw = (std::max)(2u, std::thread::hardware_concurrency());
        const unsigned n = (std::clamp)(hw / 2, 2u, 4u);
        for (unsigned i = 0; i < n; ++i) std::thread([this] { loop(); }).detach();
    }

    void loop()
    {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mu_);
                cv_.wait(lock, [this] { return !queue_.empty(); });
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            Result r;
            try {
                r = job.work();
            } catch (const std::exception& e) {
                r = FsError{"EIO", "", "", std::string("EIO: ") + e.what()};
            }
            std::lock_guard lock(doneMu_);
            done_[job.owner].push_back({job.id, std::move(r)});
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    bool started_ = false;

    std::mutex doneMu_;
    std::unordered_map<std::thread::id, std::deque<Done>> done_;
};

// Never freed: its threads run for the life of the process, and a pool torn
// down by static destruction would join threads blocked on a disk.
Pool& pool()
{
    static Pool* p = new Pool();
    return *p;
}

// This thread's unsettled promises, by job id. Heap-held and never destroyed:
// a thread_local destructor would run after the thread's runtime is gone.
struct Pending {
    uint64_t next = 1;
    std::unordered_map<uint64_t, ev::Persistent*> promises;
};
thread_local Pending* t_pending = nullptr;

Pending& pending()
{
    if (!t_pending) t_pending = new Pending();
    return *t_pending;
}

// ---- results to JS (on the owning thread) -----------------------------------------

bronze::Value statToJs(const StatOut& s)
{
    ObjectBuilder obj;
    obj.set("size", ev::fromDouble(s.size));
    obj.set("mtimeMs", ev::fromDouble(s.mtimeMs));
    obj.set("mode", ev::fromDouble(s.mode));
    obj.set("_isFile", ev::fromBool(s.isFile));
    obj.set("_isDirectory", ev::fromBool(s.isDir));
    obj.set("_isSymbolicLink", ev::fromBool(s.isLink));
    return obj.get();
}

bronze::Value errorToJs(const FsError& e)
{
    ev::Persistent err{newError("Error", e.message)};
    if (ev::isObject(err.get())) {
        err.set(ev::setProperty(err.get(), "code", ev::fromUtf8(e.code)));
        if (!e.syscall.empty()) err.set(ev::setProperty(err.get(), "syscall", ev::fromUtf8(e.syscall)));
        if (!e.path.empty()) err.set(ev::setProperty(err.get(), "path", ev::fromUtf8(e.path)));
    }
    return err.get();
}

bronze::Value resultToJs(const Result& r)
{
    return std::visit([](const auto& v) -> bronze::Value {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            return ev::undefined();
        } else if constexpr (std::is_same_v<T, FsError>) {
            return errorToJs(v);
        } else if constexpr (std::is_same_v<T, Bytes>) {
            bronze::Value u8 = ev::createTypedArray(elements::Uint8, static_cast<uint32_t>(v.data.size()));
            ev::fillTypedArray(u8, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(v.data.data()), v.data.size()));
            return u8;
        } else if constexpr (std::is_same_v<T, Text>) {
            return ev::fromUtf8(v.data);
        } else if constexpr (std::is_same_v<T, StatOut>) {
            return statToJs(v);
        } else if constexpr (std::is_same_v<T, Names>) {
            ArrayBuilder arr;
            for (const auto& n : v.names) arr.push(ev::fromUtf8(n));
            return arr.get();
        } else {
            ArrayBuilder arr;
            for (const auto& d : v.entries) {
                ObjectBuilder o;
                o.set("name", ev::fromUtf8(d.name));
                o.set("_isFile", ev::fromBool(d.isFile));
                o.set("_isDirectory", ev::fromBool(d.isDir));
                o.set("_isSymbolicLink", ev::fromBool(d.isLink));
                arr.push(o.get());
            }
            return arr.get();
        }
    }, r);
}

// ---- natives ------------------------------------------------------------------------

std::string argString(std::span<const bronze::Value> a, size_t i)
{
    return i < a.size() && !ev::isUndefined(a[i]) && !ev::isNull(a[i]) ? ev::toUtf8(a[i]) : std::string();
}

bool argBool(std::span<const bronze::Value> a, size_t i)
{
    return i < a.size() && ev::isBool(a[i]) && ev::toBool(a[i]);
}

std::string argBytes(std::span<const bronze::Value> a, size_t i)
{
    if (i >= a.size()) return {};
    bronze::Value v = a[i];
    if (ev::isString(v)) return ev::toUtf8(v);
    if (auto info = ev::typedArrayInfo(v)) return std::string(reinterpret_cast<const char*>(info.data), info.byteLength);
    if (auto ab = ev::arrayBufferInfo(v)) return std::string(reinterpret_cast<const char*>(ab.data), ab.byteLength);
    return ev::toUtf8(v);
}

// __brokit_fs_async(op, a, b, c) -> Promise. The arguments are read and the
// paths resolved here, on the JS thread; the work closure owns copies.
bronze::Value js_fs_async(bronze::Value, std::span<const bronze::Value> a)
{
    const std::string op = argString(a, 0);
    std::function<Result()> work;

    if (op == "readFile") {
        const std::string p = resolveFsPathOnThread(argString(a, 1), false);
        const bool text = !argString(a, 2).empty();
        work = [p, text] { return readFile(p, text); };
    } else if (op == "writeFile" || op == "appendFile") {
        const std::string p = resolveFsPathOnThread(argString(a, 1), true);
        std::string data = argBytes(a, 2);
        const bool append = op == "appendFile";
        work = [p, data = std::move(data), append] { return writeFile(p, data, append); };
    } else if (op == "stat" || op == "lstat") {
        const std::string p = resolveFsPathOnThread(argString(a, 1), false);
        const bool follow = op == "stat";
        work = [p, follow] { return statPath(p, follow); };
    } else if (op == "readdir") {
        const std::string p = resolveFsPathOnThread(argString(a, 1), false);
        const bool types = argBool(a, 2);
        work = [p, types] { return readDir(p, types); };
    } else if (op == "mkdir") {
        const std::string p = resolveFsPathOnThread(argString(a, 1), true);
        const bool recursive = argBool(a, 2);
        work = [p, recursive]() -> Result {
            std::error_code ec;
            if (recursive) fs::create_directories(u8p(p), ec);
            else fs::create_directory(u8p(p), ec);
            if (ec) return errorFrom("mkdir", p, ec);
            return std::monostate{};
        };
    } else if (op == "rm" || op == "rmdir" || op == "unlink") {
        const std::string p = resolveFsPathOnThread(argString(a, 1), false);
        const bool recursive = op == "rm" && argBool(a, 2);
        const bool force = op == "rm" && argBool(a, 3);
        const std::string syscall = op;
        work = [p, recursive, force, syscall]() -> Result {
            std::error_code ec;
            if (!fs::exists(u8p(p), ec)) {
                if (force) return std::monostate{};
                return errorOf("ENOENT", syscall.c_str(), p, "no such file or directory");
            }
            if (recursive) fs::remove_all(u8p(p), ec);
            else fs::remove(u8p(p), ec);
            if (ec && !force) return errorFrom(syscall.c_str(), p, ec);
            return std::monostate{};
        };
    } else if (op == "rename" || op == "copyFile") {
        const std::string from = resolveFsPathOnThread(argString(a, 1), false);
        const std::string to = resolveFsPathOnThread(argString(a, 2), true);
        const bool copy = op == "copyFile";
        work = [from, to, copy]() -> Result {
            std::error_code ec;
            if (copy) fs::copy_file(u8p(from), u8p(to), fs::copy_options::overwrite_existing, ec);
            else fs::rename(u8p(from), u8p(to), ec);
            if (ec) return errorFrom(copy ? "copyfile" : "rename", from, ec);
            return std::monostate{};
        };
    } else if (op == "realpath") {
        const std::string p = resolveFsPathOnThread(argString(a, 1), false);
        work = [p]() -> Result {
            std::error_code ec;
            auto c = fs::canonical(u8p(p), ec);
            if (ec) return errorFrom("realpath", p, ec);
            return Text{u8s(c)};
        };
    } else if (op == "access") {
        const std::string p = resolveFsPathOnThread(argString(a, 1), false);
        work = [p]() -> Result {
            std::error_code ec;
            if (!fs::exists(u8p(p), ec)) return errorOf("ENOENT", "access", p, "no such file or directory");
            return std::monostate{};
        };
    } else {
        return ev::throwTypeError(("fs: no asynchronous '" + op + "'").c_str());
    }

    Pending& pend = pending();
    const uint64_t id = pend.next++;
    auto* promise = new ev::Persistent(ev::createPromise());
    pend.promises.emplace(id, promise);
    pool().submit(Job{std::this_thread::get_id(), id, std::move(work)});
    return promise->get();
}

// Settle this thread's finished operations. Returns how many.
bronze::Value js_fs_async_tick(bronze::Value, std::span<const bronze::Value>)
{
    if (!t_pending || t_pending->promises.empty()) return ev::fromDouble(0);
    std::vector<Done> done = pool().take(std::this_thread::get_id());
    for (Done& d : done) {
        auto it = t_pending->promises.find(d.id);
        if (it == t_pending->promises.end()) continue;
        ev::Persistent* promise = it->second;
        t_pending->promises.erase(it);
        ev::Persistent value{resultToJs(d.result)};
        if (std::holds_alternative<FsError>(d.result)) ev::rejectPromise(promise->get(), value.get());
        else ev::resolvePromise(promise->get(), value.get());
        delete promise;
    }
    return ev::fromDouble(static_cast<double>(done.size()));
}

bronze::Value js_fs_async_has_pending(bronze::Value, std::span<const bronze::Value>)
{
    return ev::fromBool(t_pending && !t_pending->promises.empty());
}

}  // namespace

void installFSAsync()
{
    ev::setGlobalFunction("__brokit_fs_async", 4, js_fs_async);
    ev::setGlobalFunction("__brokit_fs_async_tick", 0, js_fs_async_tick);
    ev::setGlobalFunction("__brokit_fs_async_has_pending", 0, js_fs_async_has_pending);
}

}  // namespace brokit::api
