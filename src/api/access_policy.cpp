// The process-wide access policy (api.h, "Access policy") and the helpers
// brokit's natives use to ask it.
//
// The policy is set before realms run, but every realm's thread reads it, and
// a test harness (or a host that revokes a grant) may replace it while they
// do. So it lives behind a shared_ptr swapped under a mutex: a check copies
// the pointer under the lock and calls the hook outside it, so a hook that
// blocks to ask the user holds no lock, and a replaced policy stays alive
// until the calls already in it return. The flag spares the common case,
// no policy at all, the lock.

#include "api/access_policy.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>

namespace fs = std::filesystem;

namespace brokit::api {

namespace {

std::mutex g_policyMu;
std::shared_ptr<const AccessPolicy> g_policy;
std::atomic<bool> g_policyActive{false};

std::shared_ptr<const AccessPolicy> currentPolicy()
{
    if (!g_policyActive.load(std::memory_order_acquire)) return nullptr;
    std::lock_guard lock(g_policyMu);
    return g_policy;
}

fs::path u8p(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string u8s(const fs::path& p)
{
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

}  // namespace

void setAccessPolicy(AccessPolicy policy)
{
    const bool any = policy.fs || policy.spawn || policy.net;
    auto next = any ? std::make_shared<const AccessPolicy>(std::move(policy)) : nullptr;
    std::lock_guard lock(g_policyMu);
    g_policy = std::move(next);
    g_policyActive.store(any, std::memory_order_release);
}

bool accessPolicyActive()
{
    return g_policyActive.load(std::memory_order_acquire);
}

std::string checkFsAccess(const std::string& path, FsAccess access)
{
    auto p = currentPolicy();
    if (!p || !p->fs) return {};
    return p->fs(path, access);
}

std::string checkSpawnAccess(const std::string& command)
{
    auto p = currentPolicy();
    if (!p || !p->spawn) return {};
    return p->spawn(command);
}

std::string checkNetAccess(const std::string& url)
{
    auto p = currentPolicy();
    if (!p || !p->net) return {};
    return p->net(url);
}

std::string absoluteFsPath(const std::string& path)
{
    std::error_code ec;
    fs::path abs = fs::absolute(u8p(path), ec);
    if (ec) abs = u8p(path);
    return u8s(abs.lexically_normal());
}

std::string fsRefusal(const std::string& resolved, FsAccess access)
{
    if (!accessPolicyActive()) return {};
    return checkFsAccess(absoluteFsPath(resolved), access);
}

bronze::Value newAccessError(const std::string& message, const char* code, const std::string& path)
{
    ev::Persistent err{newError("Error", message)};
    if (ev::isObject(err.get())) {
        // Each value is made in a statement of its own: built inside the call,
        // it could move the receiver after err.get() was read.
        ev::Persistent codeVal{ev::fromUtf8(code)};
        err.set(ev::setProperty(err.get(), "code", codeVal.get()));
        if (!path.empty()) {
            ev::Persistent pathVal{ev::fromUtf8(path)};
            err.set(ev::setProperty(err.get(), "path", pathVal.get()));
        }
    }
    return err.get();
}

bronze::Value throwAccessError(const std::string& message, const char* code, const std::string& path)
{
    return ev::throwValue(newAccessError(message, code, path));
}

bronze::Value throwFsDenied(const std::string& refusal, const std::string& resolved)
{
    return throwAccessError(refusal, kFsDeniedCode, absoluteFsPath(resolved));
}

bronze::Value rejectedAccessPromise(const std::string& message, const char* code, const std::string& path)
{
    ev::Persistent promise{ev::createPromise()};
    ev::Persistent err{newAccessError(message, code, path)};
    ev::rejectPromise(promise.get(), err.get());
    return promise.get();
}

std::string commandLineFor(const std::string& file, const std::vector<std::string>& args)
{
    auto quoted = [](const std::string& s) {
        if (!s.empty() && s.find_first_of(" \t\n\"") == std::string::npos) return s;
        std::string q = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') q += '\\';
            q += c;
        }
        return q + "\"";
    };
    std::string line = quoted(file);
    for (const auto& a : args) {
        line += ' ';
        line += quoted(a);
    }
    return line;
}

std::string socketUrl(const char* scheme, const std::string& host, int port)
{
    const bool v6 = host.find(':') != std::string::npos && (host.empty() || host.front() != '[');
    return std::string(scheme) + "://" + (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

}  // namespace brokit::api
