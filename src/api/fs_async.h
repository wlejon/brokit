#pragma once
// fs's asynchronous half: fs.promises and the callback functions run their
// I/O on a small pool of threads and settle on the JS thread that asked, at
// its next __brokit_fs_async_tick. The page's thread never blocks on a disk.

#include <string>

namespace brokit::api {

/// The path a script named, resolved on the calling (JS) thread: prefix
/// mounts (/lib, /system) and the fs base paths. `forCreate` resolves a path
/// that need not exist yet against the first base path.
std::string resolveFsPathOnThread(const std::string& path, bool forCreate);

void installFSAsync();

}  // namespace brokit::api
