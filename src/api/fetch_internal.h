#pragma once

// Shared between fetch.cpp (the driver: request state, curl, streams, abort,
// tick, fetch()) and fetch_response.cpp (Response construction: the bridge to
// the fetch_helpers.js factories, header objects, and the responses for the
// local schemes — file paths, data: and blob: URLs).

#include "embed/embed.h"

#include <span>
#include <string>
#include <vector>

namespace brokit::api {

// Call the fetch_helpers.js factory `fnName`. `args` must be current at the
// call; they are rooted before the lookups allocate.
bronze::Value callInternal(const char* fnName, std::span<const bronze::Value> args);

// Build a Headers-like JS object from a flat header list ("name: value" lines).
bronze::Value buildHeaders(const std::vector<std::string>& headers);

bool isHttpUrl(const std::string& url);
bool isDataUrl(const std::string& url);
bool isBlobUrl(const std::string& url);

// A local fetch target resolved against the prefix mounts and the base paths.
std::string resolveLocalPath(const std::string& url);

// Responses for the local schemes, already built and bodied.
bronze::Value buildDataUrlResponse(const std::string& url);
// `found` is false for a URL that was never minted or has been revoked.
bronze::Value buildBlobUrlResponse(const std::string& url, bool* found);
bronze::Value buildFileResponse(const std::string& url, const std::string& resolvedPath);

} // namespace brokit::api
