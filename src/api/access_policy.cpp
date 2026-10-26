// brokit's side of bronze's access gate (api.h, "Access"; embed/embed_access.h):
// the checks brokit's natives make, and the helpers that turn a refusal into
// the Error the program sees. The gate itself, and what is held open under
// it, are bronze's: one hook the host installs for every library.

#include "api/access_policy.h"

#include "embed/embed_access.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace brokit::api {

namespace {

fs::path u8p(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string u8s(const fs::path& p)
{
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

}  // namespace

bool accessPolicyActive()
{
    return ev::accessGateActive();
}

std::string checkFsAccess(const std::string& path, FsAccess access)
{
    return ev::checkAccess(access == FsAccess::Write ? ev::AccessKind::Write : ev::AccessKind::Read, path);
}

std::string checkSpawnAccess(const std::string& command)
{
    return ev::checkAccess(ev::AccessKind::Spawn, command);
}

std::string checkNetAccess(const std::string& url)
{
    return ev::checkAccess(ev::AccessKind::Net, url);
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
    return ev::newAccessError(message, code, path);
}

bronze::Value throwAccessError(const std::string& message, const char* code, const std::string& path)
{
    return ev::throwAccessError(message, code, path);
}

bronze::Value throwFsDenied(const std::string& refusal, const std::string& resolved)
{
    return throwAccessError(refusal, kFsDeniedCode, absoluteFsPath(resolved));
}

bronze::Value rejectedAccessPromise(const std::string& message, const char* code, const std::string& path)
{
    return ev::rejectedAccessPromise(message, code, path);
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

uint64_t holdGatedAccess(ev::AccessKind kind, std::string subject, std::function<void(const std::string&)> revoke)
{
    if (!ev::accessGateActive()) return 0;
    return ev::holdAccess(kind, std::move(subject), std::move(revoke));
}

void releaseHeldAccess(uint64_t& id)
{
    if (id == 0) return;
    ev::releaseAccess(id);
    id = 0;
}

std::string refusalText(const std::string& refusal)
{
    return ev::refusalMessage(refusal);
}

const char* refusalName(const std::string& refusal)
{
    return ev::isRevokedRefusal(refusal) ? "PermissionRevoked" : "Error";
}

}  // namespace brokit::api
