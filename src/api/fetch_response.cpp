#include "api/api.h"
#include "api/arg_reader.h"
#include "api/fetch_internal.h"
#include "api/object_builder.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace brokit::api {

namespace {

static thread_local std::vector<std::string> g_fetchBasePaths;

} // namespace

void addFetchBasePath(const std::string& path)
{
    g_fetchBasePaths.push_back(path);
}

// ---------------------------------------------------------------------------
// Helpers backed by JS factories from fetch_helpers.js
// ---------------------------------------------------------------------------
// `args` must be current at the call; they are rooted before the lookups
// below allocate.
bronze::Value callInternal(const char* fnName, std::span<const bronze::Value> args)
{
    std::vector<ev::Persistent> roots;
    roots.reserve(args.size());
    for (bronze::Value v : args) roots.emplace_back(v);

    ev::Persistent internals{ev::getGlobal("__brokit_fetch_internals")};
    if (!ev::isObject(internals.get())) {
        return ev::throwError("fetch: internals not installed");
    }
    ev::Persistent fn{ev::getProperty(internals.get(), fnName)};
    if (!ev::isFunction(fn.get())) {
        return ev::throwError(std::string("fetch: missing internal helper '") + fnName + "'");
    }
    std::vector<bronze::Value> current;
    current.reserve(roots.size());
    for (const ev::Persistent& r : roots) current.push_back(r.get());
    auto ret = ev::call(fn.get(), ev::undefined(), current);
    return ret.value;
}

// ---------------------------------------------------------------------------
// Local file helpers
// ---------------------------------------------------------------------------
bool isHttpUrl(const std::string& url)
{
    return url.size() > 7 &&
           (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0);
}

bool isDataUrl(const std::string& url)
{
    return url.size() >= 5 && url.compare(0, 5, "data:") == 0;
}

bool isBlobUrl(const std::string& url)
{
    return url.size() >= 5 && url.compare(0, 5, "blob:") == 0;
}

static std::string detectMimeType(const std::string& path)
{
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "css")  return "text/css; charset=utf-8";
    if (ext == "js" || ext == "mjs") return "application/javascript; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "png")  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif")  return "image/gif";
    if (ext == "svg")  return "image/svg+xml";
    if (ext == "webp") return "image/webp";
    if (ext == "woff") return "font/woff";
    if (ext == "woff2") return "font/woff2";
    if (ext == "ttf")  return "font/ttf";
    if (ext == "otf")  return "font/otf";
    if (ext == "txt")  return "text/plain; charset=utf-8";
    if (ext == "xml")  return "application/xml";
    if (ext == "wasm") return "application/wasm";
    return "application/octet-stream";
}

extern std::string resolveBrokitPrefixMount(const std::string& path);

std::string resolveLocalPath(const std::string& url)
{
    std::string mounted = resolveBrokitPrefixMount(url);
    if (!mounted.empty()) return mounted;

    std::string clean = url;
    if (clean.size() >= 2 && clean[0] == '.' && clean[1] == '/')
        clean = clean.substr(2);

    if (clean.size() >= 2 && clean[1] == ':') return clean;
    if (!clean.empty() && (clean[0] == '/' || clean[0] == '\\')) {
        std::ifstream absTest(clean, std::ios::binary);
        if (absTest.good()) return clean;
        clean = clean.substr(1);
    }

    auto g = ev::globalValue("globalThis");
    if (g.found && ev::isObject(g.value)) {
        // Rooted: the length read and every element read below allocate.
        ev::Persistent arr{ev::getProperty(g.value, "__brokit_fetch_base_paths")};
        if (ev::isObject(arr.get())) {
            auto lenVal = ev::getProperty(arr.get(), "length");
            // A script-writable global: bound the walk.
            int32_t len = ev::isNumber(lenVal)
                              ? static_cast<int32_t>((std::min)(saturateU32(ev::toDouble(lenVal)), kMaxScriptList))
                              : 0;
            for (int32_t i = len - 1; i >= 0; --i) {
                auto elem = ev::getElement(arr.get(), static_cast<uint32_t>(i));
                if (ev::isString(elem)) {
                    std::string candidate = ev::toUtf8(elem);
                    if (!candidate.empty() && candidate.back() != '/' && candidate.back() != '\\')
                        candidate += '/';
                    candidate += clean;
                    std::ifstream test(candidate, std::ios::binary);
                    if (test.good()) return candidate;
                }
            }
        }
    }

    for (int i = static_cast<int>(g_fetchBasePaths.size()) - 1; i >= 0; i--) {
        std::string candidate = g_fetchBasePaths[i];
        if (!candidate.empty() && candidate.back() != '/' && candidate.back() != '\\')
            candidate += '/';
        candidate += clean;
        std::ifstream test(candidate, std::ios::binary);
        if (test.good()) return candidate;
    }

    return clean;
}

// Build a Headers-like JS object from a flat header list ("name: value" lines).
bronze::Value buildHeaders(const std::vector<std::string>& headers)
{
    ObjectBuilder hdrs;
    for (auto& h : headers) {
        auto colon = h.find(':');
        if (colon != std::string::npos) {
            std::string name = h.substr(0, colon);
            std::string value = h.substr(colon + 1);
            while (!value.empty() && value[0] == ' ') value.erase(0, 1);
            for (auto& c : name) c = static_cast<char>(tolower(c));
            hdrs.set(name, value);
        }
    }

    bronze::Value hdrsVal = hdrs.build();
    return callInternal("headers", std::array<bronze::Value, 1>{hdrsVal});
}

// Build a Response for a data: URL — RFC 2397.
bronze::Value buildDataUrlResponse(const std::string& url)
{
    std::string rest = url.substr(5); // strip "data:"
    auto comma = rest.find(',');

    std::string meta;
    std::string payload;
    if (comma == std::string::npos) {
        payload = rest;
    } else {
        meta    = rest.substr(0, comma);
        payload = rest.substr(comma + 1);
    }

    bool isBase64 = false;
    std::string mime = "text/plain;charset=US-ASCII";
    if (!meta.empty()) {
        const std::string b64Tag = ";base64";
        if (meta.size() >= b64Tag.size() &&
            meta.compare(meta.size() - b64Tag.size(), b64Tag.size(), b64Tag) == 0) {
            isBase64 = true;
            meta = meta.substr(0, meta.size() - b64Tag.size());
        }
        if (!meta.empty()) mime = meta;
    }

    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::vector<uint8_t> data;
    if (isBase64) {
        static const int8_t tbl[128] = {
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
            52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
            -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
            15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
            -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
            41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        };
        uint32_t buf = 0;
        int bits = 0;
        for (char c : payload) {
            if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
            unsigned uc = static_cast<unsigned char>(c);
            if (uc >= 128) continue;
            int v = tbl[uc];
            if (v < 0) continue;
            buf = (buf << 6) | static_cast<uint32_t>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                data.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
            }
        }
    } else {
        data.reserve(payload.size());
        for (size_t i = 0; i < payload.size(); ++i) {
            if (payload[i] == '%' && i + 2 < payload.size()) {
                int hi = hexVal(payload[i+1]);
                int lo = hexVal(payload[i+2]);
                if (hi >= 0 && lo >= 0) {
                    data.push_back(static_cast<uint8_t>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            data.push_back(static_cast<uint8_t>(payload[i]));
        }
    }

    std::vector<std::string> headerLines = {
        "content-type: " + mime,
        "content-length: " + std::to_string(data.size()),
    };

    ObjectBuilder resp;
    resp.set("status", 200.0);
    resp.set("statusText", "OK");
    resp.set("ok", true);
    resp.set("url", url);
    resp.set("headers", buildHeaders(headerLines));
    resp.set("__body", ev::createArrayBuffer(std::span<const uint8_t>(data.data(), data.size())));

    callInternal("applyFileBody", std::array<bronze::Value, 1>{resp.build()});
    return resp.build();  // re-read from the builder's root: the call allocated
}

// Build a Response for a blob: URL from the URL.createObjectURL registry.
// `found` is false for a URL that was never minted or has been revoked.
bronze::Value buildBlobUrlResponse(const std::string& url, bool* found)
{
    bronze::Value blob = ev::undefined();
    if (!blobByObjectURL(url, &blob)) {
        *found = false;
        ObjectBuilder resp;
        resp.set("status", 404.0);
        resp.set("statusText", "Not Found");
        resp.set("ok", false);
        resp.set("url", url);
        resp.set("headers", buildHeaders({}));
        resp.set("__body", ev::createArrayBuffer(std::span<const uint8_t>()));
        callInternal("applyFileBody", std::array<bronze::Value, 1>{resp.build()});
        return resp.build();
    }
    *found = true;

    // The bytes live in the Blob's own C++ storage, which no collection moves,
    // so the span stays valid across the allocations below.
    const uint8_t* data = nullptr;
    size_t len = 0;
    std::string type;
    blobBytes(blob, &data, &len, &type);

    std::vector<std::string> headerLines;
    if (!type.empty()) headerLines.push_back("content-type: " + type);
    headerLines.push_back("content-length: " + std::to_string(len));

    ObjectBuilder resp;
    resp.set("status", 200.0);
    resp.set("statusText", "OK");
    resp.set("ok", true);
    resp.set("url", url);
    resp.set("headers", buildHeaders(headerLines));
    resp.set("__body", ev::createArrayBuffer(std::span<const uint8_t>(data, len)));

    callInternal("applyFileBody", std::array<bronze::Value, 1>{resp.build()});
    return resp.build();  // re-read from the builder's root: the call allocated
}

// Build a Response for a local file read
bronze::Value buildFileResponse(const std::string& url, const std::string& resolvedPath)
{
    std::ifstream file(resolvedPath, std::ios::in | std::ios::binary | std::ios::ate);
    if (!file) {
        ObjectBuilder resp;
        resp.set("status", 404.0);
        resp.set("statusText", "Not Found");
        resp.set("ok", false);
        resp.set("url", url);
        resp.set("headers", buildHeaders({}));

        callInternal("applyNotFoundBody", std::array<bronze::Value, 1>{resp.build()});
        return resp.build();
    }

    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);

    std::string mime = detectMimeType(resolvedPath);
    std::vector<std::string> headerLines = {
        "content-type: " + mime,
        "content-length: " + std::to_string(data.size()),
    };

    ObjectBuilder resp;
    resp.set("status", 200.0);
    resp.set("statusText", "OK");
    resp.set("ok", true);
    resp.set("url", url);
    resp.set("headers", buildHeaders(headerLines));
    resp.set("__body", ev::createArrayBuffer(std::span<const uint8_t>(data.data(), data.size())));

    callInternal("applyFileBody", std::array<bronze::Value, 1>{resp.build()});
    return resp.build();  // re-read from the builder's root: the call allocated
}

} // namespace brokit::api
