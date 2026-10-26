#pragma once
// The access policy's helpers for brokit's own natives: each entry point that
// touches the disk, starts a process or opens a socket asks here first, on
// the JS thread that made the call, and turns a refusal into the Error the
// program sees. The policy itself and its public checks are in api.h.

#include "api/api.h"

#include <string>
#include <vector>

namespace brokit::api {

/// The error codes a refusal carries, as api.h documents them.
inline constexpr const char* kFsDeniedCode = "EACCES";
inline constexpr const char* kSpawnDeniedCode = "EACCES";
inline constexpr const char* kNetDeniedCode = "ERR_ACCESS_DENIED";

/// True when a policy is installed: callers skip building a check's argument
/// (an absolute path, a command line) when there is nothing to ask.
bool accessPolicyActive();

/// `path` as the fs hook receives it: absolute (a relative path is taken
/// against the process's working directory) and lexically normal.
std::string absoluteFsPath(const std::string& path);

/// checkFsAccess on `resolved` (a path after mounts and base paths) made
/// absolute: "" when allowed, else the refusal.
std::string fsRefusal(const std::string& resolved, FsAccess access);

/// The refusal as an Error with `message` and `code` (and `path` when one is
/// given), built GC-safely. The result is fresh: root it before allocating.
bronze::Value newAccessError(const std::string& message, const char* code, const std::string& path = {});

/// Throw newAccessError into the running program; returns what the NativeFn
/// should return.
bronze::Value throwAccessError(const std::string& message, const char* code, const std::string& path = {});

/// Throw an fs refusal for `resolved` (code EACCES, `path` the absolute path).
bronze::Value throwFsDenied(const std::string& refusal, const std::string& resolved);

/// A promise already rejected with newAccessError.
bronze::Value rejectedAccessPromise(const std::string& message, const char* code, const std::string& path = {});

/// The program and its arguments as one line, for the spawn hook: arguments
/// holding whitespace or nothing are quoted so the line reads unambiguously.
std::string commandLineFor(const std::string& file, const std::vector<std::string>& args);

/// `scheme://host:port` for the raw-socket forms (an IPv6 literal bracketed).
std::string socketUrl(const char* scheme, const std::string& host, int port);

}  // namespace brokit::api
