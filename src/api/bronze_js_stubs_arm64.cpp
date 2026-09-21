// Native arm64 entry stubs for Bronze compiled JS polyfills on Apple Silicon.
// Bronze/Brass code generator is x86_64; on Apple Silicon (arm64), these stubs
// satisfy the static entry points required by brokit_api. When BRASS_HOST_BACKEND
// is disabled, they fall back to evaluating the polyfill scripts dynamically
// so the runtime polyfills still initialize instead of becoming inert no-ops.

#include "embed/embed.h"
#include "eval/eval.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace bronze::embed {
#ifndef BRONZE_EMBED_EVALSCRIPT_INLINE_DEFINED
#define BRONZE_EMBED_EVALSCRIPT_INLINE_DEFINED
inline CallResult evalScript(std::string_view source, const eval::EvalOptions& options = {}) {
    return eval::evalScript(source, options);
}
#endif
} // namespace bronze::embed

namespace {

static bool evaluatePolyfill(const char* name) {
    const std::string filename = std::string(name) + ".js";
    std::vector<fs::path> candidates = {
        fs::path("src/api/js") / filename,
        fs::path("js") / filename,
        fs::path(__FILE__).parent_path() / "js" / filename,
    };
    if (const char* env = std::getenv("BROKIT_JS_DIR")) {
        candidates.insert(candidates.begin(), fs::path(env) / filename);
    }

    for (const auto& path : candidates) {
        std::error_code ec;
        if (fs::exists(path, ec)) {
            std::ifstream file(path, std::ios::in | std::ios::binary);
            if (file) {
                std::ostringstream ss;
                ss << file.rdbuf();
                std::string code = ss.str();
                bronze::eval::EvalOptions opts;
                opts.filename = filename;
                bronze::embed::CallResult res = bronze::embed::evalScript(code, opts);
                return !res.thrown;
            }
        }
    }
    return false;
}

} // namespace

extern "C" {
void bronze_abort_main() { evaluatePolyfill("abort"); }
void bronze_structuredclone_main() { evaluatePolyfill("structuredclone"); }
void bronze_timers_main() { evaluatePolyfill("timers"); }
void bronze_console_time_main() { evaluatePolyfill("console_time"); }
void bronze_url_main() { evaluatePolyfill("url"); }
void bronze_encoding_main() { evaluatePolyfill("encoding"); }
void bronze_treewalker_main() { evaluatePolyfill("treewalker"); }
void bronze_url_object_main() { evaluatePolyfill("url_object"); }
void bronze_path_main() { evaluatePolyfill("path"); }
void bronze_readable_stream_main() { evaluatePolyfill("readable_stream"); }
void bronze_fs_main() { evaluatePolyfill("fs"); }
void bronze_child_process_main() { evaluatePolyfill("child_process"); }
void bronze_indexeddb_main() { evaluatePolyfill("indexeddb"); }
void bronze_writable_stream_main() { evaluatePolyfill("writable_stream"); }
void bronze_websocket_main() { evaluatePolyfill("websocket"); }
void bronze_eventsource_main() { evaluatePolyfill("eventsource"); }
void bronze_formdata_main() { evaluatePolyfill("formdata"); }
void bronze_fetch_classes_main() { evaluatePolyfill("fetch_classes"); }
void bronze_xhr_main() { evaluatePolyfill("xhr"); }
void bronze_base64_main() { evaluatePolyfill("base64"); }
void bronze_navigator_main() { evaluatePolyfill("navigator"); }
void bronze_event_target_main() { evaluatePolyfill("event_target"); }
void bronze_message_channel_main() { evaluatePolyfill("message_channel"); }
void bronze_fs_watch_main() { evaluatePolyfill("fs_watch"); }
void bronze_fetch_helpers_main() { evaluatePolyfill("fetch_helpers"); }
void bronze_process_main() { evaluatePolyfill("process"); }
void bronze_buffer_main() { evaluatePolyfill("buffer"); }
void bronze_events_main() { evaluatePolyfill("events"); }
void bronze_util_main() { evaluatePolyfill("util"); }
void bronze_compression_main() { evaluatePolyfill("compression"); }
void bronze_net_main() { evaluatePolyfill("net"); }
void bronze_dgram_main() { evaluatePolyfill("dgram"); }
void bronze_websocket_server_main() { evaluatePolyfill("websocket_server"); }
}
