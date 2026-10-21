// Native HTTP/1.1 parser bindings over llhttp — backs the Node-compatible
// `http` module (js/http.js).
//
// Each parser instance is owned by an integer handle in `g_parsers`. Feeding
// bytes via `__brokit_http_parser_execute(id, bytes)` runs `llhttp_execute`
// synchronously on the C stack and collects events into C++ vectors; JS values
// are constructed only after `llhttp_execute` returns so moving GC can never
// relocate heap values while C parser callbacks are on the stack.

#include "api/api.h"
#include "api/arg_reader.h"
#include "api/object_builder.h"

#include "llhttp.h"

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace brokit::api {

namespace {

struct HttpEvent {
    enum class Type {
        Headers,
        Body,
        Complete,
        Upgrade,
        Error,
    };

    Type type;
    std::string method;
    std::string url;
    std::string statusMessage;
    int statusCode = 0;
    int versionMajor = 1;
    int versionMinor = 1;
    bool upgrade = false;
    bool shouldKeepAlive = true;
    std::vector<std::string> rawHeaders;
    std::vector<uint8_t> data;
    std::string errorCode;
    std::string errorReason;
};

struct ParserHandle {
    llhttp_t parser{};
    llhttp_settings_t settings{};
    bool isResponse = false;
    bool skipBody = false;

    std::string urlBuf;
    std::string statusBuf;
    std::string curField;
    std::string curValue;
    bool lastWasValue = false;
    std::vector<std::string> rawHeaders;

    std::vector<HttpEvent> events;
};

static thread_local std::unordered_map<int, ParserHandle*> g_parsers;
static thread_local int g_nextParserId = 1;

void flushHeaderPair(ParserHandle* h) {
    if (!h->curField.empty()) {
        h->rawHeaders.push_back(std::move(h->curField));
        h->rawHeaders.push_back(std::move(h->curValue));
        h->curField.clear();
        h->curValue.clear();
    }
    h->lastWasValue = false;
}

int onMessageBegin(llhttp_t* p) {
    auto* h = static_cast<ParserHandle*>(p->data);
    h->urlBuf.clear();
    h->statusBuf.clear();
    h->curField.clear();
    h->curValue.clear();
    h->lastWasValue = false;
    h->rawHeaders.clear();
    return HPE_OK;
}

int onUrl(llhttp_t* p, const char* at, size_t len) {
    auto* h = static_cast<ParserHandle*>(p->data);
    h->urlBuf.append(at, len);
    return HPE_OK;
}

int onStatus(llhttp_t* p, const char* at, size_t len) {
    auto* h = static_cast<ParserHandle*>(p->data);
    h->statusBuf.append(at, len);
    return HPE_OK;
}

int onHeaderField(llhttp_t* p, const char* at, size_t len) {
    auto* h = static_cast<ParserHandle*>(p->data);
    if (h->lastWasValue) {
        flushHeaderPair(h);
    }
    h->curField.append(at, len);
    return HPE_OK;
}

int onHeaderValue(llhttp_t* p, const char* at, size_t len) {
    auto* h = static_cast<ParserHandle*>(p->data);
    h->lastWasValue = true;
    h->curValue.append(at, len);
    return HPE_OK;
}

int onHeadersComplete(llhttp_t* p) {
    auto* h = static_cast<ParserHandle*>(p->data);
    flushHeaderPair(h);

    HttpEvent ev;
    ev.type = HttpEvent::Type::Headers;
    if (!h->isResponse) {
        const char* m = llhttp_method_name(static_cast<llhttp_method_t>(llhttp_get_method(p)));
        ev.method = m ? m : "GET";
        ev.url = std::move(h->urlBuf);
    } else {
        ev.statusCode = llhttp_get_status_code(p);
        ev.statusMessage = std::move(h->statusBuf);
    }
    ev.versionMajor = llhttp_get_http_major(p);
    ev.versionMinor = llhttp_get_http_minor(p);
    ev.upgrade = llhttp_get_upgrade(p) != 0;
    ev.shouldKeepAlive = llhttp_should_keep_alive(p) != 0;
    ev.rawHeaders = std::move(h->rawHeaders);
    h->events.push_back(std::move(ev));
    return h->skipBody ? 1 : HPE_OK;
}

int onBody(llhttp_t* p, const char* at, size_t len) {
    if (len == 0) return HPE_OK;
    auto* h = static_cast<ParserHandle*>(p->data);
    if (!h->events.empty() && h->events.back().type == HttpEvent::Type::Body) {
        auto& prev = h->events.back().data;
        prev.insert(prev.end(), reinterpret_cast<const uint8_t*>(at),
                    reinterpret_cast<const uint8_t*>(at) + len);
        return HPE_OK;
    }
    HttpEvent ev;
    ev.type = HttpEvent::Type::Body;
    ev.data.assign(reinterpret_cast<const uint8_t*>(at),
                   reinterpret_cast<const uint8_t*>(at) + len);
    h->events.push_back(std::move(ev));
    return HPE_OK;
}

int onMessageComplete(llhttp_t* p) {
    auto* h = static_cast<ParserHandle*>(p->data);
    HttpEvent ev;
    ev.type = HttpEvent::Type::Complete;
    ev.upgrade = llhttp_get_upgrade(p) != 0;
    ev.shouldKeepAlive = llhttp_should_keep_alive(p) != 0;
    h->events.push_back(std::move(ev));
    return HPE_OK;
}

void initParserHandle(ParserHandle* h, bool isResponse) {
    h->isResponse = isResponse;
    llhttp_settings_init(&h->settings);
    h->settings.on_message_begin = onMessageBegin;
    h->settings.on_url = onUrl;
    h->settings.on_status = onStatus;
    h->settings.on_header_field = onHeaderField;
    h->settings.on_header_value = onHeaderValue;
    h->settings.on_headers_complete = onHeadersComplete;
    h->settings.on_body = onBody;
    h->settings.on_message_complete = onMessageComplete;
    llhttp_init(&h->parser, isResponse ? HTTP_RESPONSE : HTTP_REQUEST, &h->settings);
    h->parser.data = h;
}

bronze::Value serializeEvents(std::vector<HttpEvent>& events) {
    ArrayBuilder arr;
    for (auto& e : events) {
        ObjectBuilder obj;
        switch (e.type) {
        case HttpEvent::Type::Headers: {
            obj.set("type", "headers");
            if (!e.method.empty()) obj.set("method", e.method);
            if (!e.url.empty()) obj.set("url", e.url);
            if (e.statusCode > 0) obj.set("statusCode", static_cast<double>(e.statusCode));
            if (!e.statusMessage.empty()) obj.set("statusMessage", e.statusMessage);
            obj.set("versionMajor", static_cast<double>(e.versionMajor));
            obj.set("versionMinor", static_cast<double>(e.versionMinor));
            obj.set("upgrade", e.upgrade);
            obj.set("shouldKeepAlive", e.shouldKeepAlive);

            ArrayBuilder hdrs;
            for (const auto& s : e.rawHeaders) {
                hdrs.push(ev::fromUtf8(s));
            }
            obj.set("rawHeaders", hdrs.get());
            break;
        }
        case HttpEvent::Type::Body: {
            obj.set("type", "body");
            ev::Persistent ab{ev::createArrayBuffer(
                std::span<const uint8_t>(e.data.data(), e.data.size()))};
            bronze::Value u8 = ev::createTypedArrayView(
                elements::Uint8, ab.get(), 0, static_cast<uint32_t>(e.data.size()));
            obj.set("data", u8);
            break;
        }
        case HttpEvent::Type::Complete:
            obj.set("type", "complete");
            obj.set("upgrade", e.upgrade);
            obj.set("shouldKeepAlive", e.shouldKeepAlive);
            break;
        case HttpEvent::Type::Upgrade: {
            obj.set("type", "upgrade");
            ev::Persistent ab{ev::createArrayBuffer(
                std::span<const uint8_t>(e.data.data(), e.data.size()))};
            bronze::Value u8 = ev::createTypedArrayView(
                elements::Uint8, ab.get(), 0, static_cast<uint32_t>(e.data.size()));
            obj.set("head", u8);
            break;
        }
        case HttpEvent::Type::Error:
            obj.set("type", "error");
            obj.set("code", e.errorCode);
            obj.set("reason", e.errorReason);
            break;
        }
        arr.push(obj.build());
    }
    events.clear();
    return arr.get();
}

static bronze::Value js_http_parser_create(bronze::Value, std::span<const bronze::Value> args) {
    bool isResponse = boolAt(args, 0);
    bool skipBody = boolAt(args, 1);
    auto* h = new ParserHandle();
    initParserHandle(h, isResponse);
    h->skipBody = skipBody;
    int id = g_nextParserId++;
    g_parsers[id] = h;
    return ev::fromDouble(static_cast<double>(id));
}

static bronze::Value js_http_parser_execute(bronze::Value, std::span<const bronze::Value> args) {
    int id = i32At(args, 0);
    auto it = g_parsers.find(id);
    if (it == g_parsers.end()) return ev::makeArray(0);
    ParserHandle* h = it->second;

    if (args.size() < 2) return ev::makeArray(0);

    std::vector<uint8_t> copy;
    const uint8_t* ptr = nullptr;
    size_t len = 0;
    if (bufferBytes(args[1], &ptr, &len)) {
        copy.assign(ptr, ptr + len);
    } else if (ev::isString(args[1])) {
        std::string s = ev::toUtf8(args[1]);
        copy.assign(reinterpret_cast<const uint8_t*>(s.data()),
                    reinterpret_cast<const uint8_t*>(s.data()) + s.size());
    } else {
        return ev::makeArray(0);
    }

    if (copy.empty()) return ev::makeArray(0);

    const char* data = reinterpret_cast<const char*>(copy.data());
    llhttp_errno_t err = llhttp_execute(&h->parser, data, copy.size());

    if (err == HPE_PAUSED_UPGRADE) {
        const char* pos = llhttp_get_error_pos(&h->parser);
        size_t consumed = (pos && pos >= data) ? static_cast<size_t>(pos - data) : copy.size();
        HttpEvent up;
        up.type = HttpEvent::Type::Upgrade;
        if (consumed < copy.size()) {
            up.data.assign(copy.begin() + static_cast<ptrdiff_t>(consumed), copy.end());
        }
        h->events.push_back(std::move(up));
        llhttp_resume_after_upgrade(&h->parser);
    } else if (err != HPE_OK) {
        HttpEvent er;
        er.type = HttpEvent::Type::Error;
        const char* code = llhttp_errno_name(err);
        const char* reason = llhttp_get_error_reason(&h->parser);
        er.errorCode = code ? code : "HPE_INVALID";
        er.errorReason = (reason && *reason) ? reason : er.errorCode;
        h->events.push_back(std::move(er));
    }

    return serializeEvents(h->events);
}

static bronze::Value js_http_parser_finish(bronze::Value, std::span<const bronze::Value> args) {
    int id = i32At(args, 0);
    auto it = g_parsers.find(id);
    if (it == g_parsers.end()) return ev::makeArray(0);
    ParserHandle* h = it->second;

    llhttp_errno_t err = llhttp_finish(&h->parser);
    if (err != HPE_OK && err != HPE_PAUSED_UPGRADE) {
        HttpEvent er;
        er.type = HttpEvent::Type::Error;
        const char* code = llhttp_errno_name(err);
        const char* reason = llhttp_get_error_reason(&h->parser);
        er.errorCode = code ? code : "HPE_INVALID_EOF_STATE";
        er.errorReason = (reason && *reason) ? reason : er.errorCode;
        h->events.push_back(std::move(er));
    }
    return serializeEvents(h->events);
}

static bronze::Value js_http_parser_reset(bronze::Value, std::span<const bronze::Value> args) {
    int id = i32At(args, 0);
    auto it = g_parsers.find(id);
    if (it == g_parsers.end()) return ev::undefined();
    ParserHandle* h = it->second;
    initParserHandle(h, h->isResponse);
    h->events.clear();
    return ev::undefined();
}

static bronze::Value js_http_parser_destroy(bronze::Value, std::span<const bronze::Value> args) {
    int id = i32At(args, 0);
    auto it = g_parsers.find(id);
    if (it != g_parsers.end()) {
        delete it->second;
        g_parsers.erase(it);
    }
    return ev::undefined();
}

static bronze::Value js_http_status_text(bronze::Value, std::span<const bronze::Value> args) {
    int code = i32At(args, 0);
    const char* name = llhttp_status_name(static_cast<llhttp_status_t>(code));
    return ev::fromUtf8(name ? name : "OK");
}

} // namespace

void installHttp() {
    ev::registerFunction("__brokit_http_parser_create", js_http_parser_create, 1);
    ev::registerFunction("__brokit_http_parser_execute", js_http_parser_execute, 2);
    ev::registerFunction("__brokit_http_parser_finish", js_http_parser_finish, 1);
    ev::registerFunction("__brokit_http_parser_reset", js_http_parser_reset, 1);
    ev::registerFunction("__brokit_http_parser_destroy", js_http_parser_destroy, 1);
    ev::registerFunction("__brokit_http_status_text", js_http_status_text, 1);
}

} // namespace brokit::api
