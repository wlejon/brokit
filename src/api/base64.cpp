#include "api/api.h"
#include "api/arg_reader.h"
#include "embed/embed.h"

#include <cstdint>
#include <string>
#include <vector>

extern "C" void bronze_base64_main();

namespace brokit::api {

namespace {

Value nativeBtoa(Value, std::span<const Value> a) {
    std::string utf8 = ev::toUtf8(hasArg(a, 0) ? a[0] : ev::undefined());
    std::vector<uint8_t> latin1;
    latin1.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size(); ) {
        uint8_t b0 = static_cast<uint8_t>(utf8[i]);
        if (b0 < 0x80) {
            latin1.push_back(b0);
            i++;
        } else if ((b0 == 0xC2 || b0 == 0xC3) && i + 1 < utf8.size()) {
            uint8_t b1 = static_cast<uint8_t>(utf8[i + 1]);
            if ((b1 & 0xC0) != 0x80) {
                return throwDOMException(
                    "The string to be encoded contains characters outside of the Latin1 range.",
                    "InvalidCharacterError");
            }
            uint32_t cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
            latin1.push_back(static_cast<uint8_t>(cp));
            i += 2;
        } else {
            return throwDOMException(
                "The string to be encoded contains characters outside of the Latin1 range.",
                "InvalidCharacterError");
        }
    }

    static const char kBase64Chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    size_t len = latin1.size();
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b1 = latin1[i];
        uint32_t b2 = (i + 1 < len) ? latin1[i + 1] : 0;
        uint32_t b3 = (i + 2 < len) ? latin1[i + 2] : 0;
        uint32_t triple = (b1 << 16) | (b2 << 8) | b3;
        out.push_back(kBase64Chars[(triple >> 18) & 63]);
        out.push_back(kBase64Chars[(triple >> 12) & 63]);
        out.push_back((i + 1 < len) ? kBase64Chars[(triple >> 6) & 63] : '=');
        out.push_back((i + 2 < len) ? kBase64Chars[triple & 63] : '=');
    }
    return ev::fromUtf8(out);
}

int8_t decodeChar(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<int8_t>(c - 'A');
    if (c >= 'a' && c <= 'z') return static_cast<int8_t>(c - 'a' + 26);
    if (c >= '0' && c <= '9') return static_cast<int8_t>(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

Value nativeAtob(Value, std::span<const Value> a) {
    std::string utf8 = ev::toUtf8(hasArg(a, 0) ? a[0] : ev::undefined());
    std::string clean;
    clean.reserve(utf8.size());
    for (char ch : utf8) {
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\f') {
            clean.push_back(ch);
        }
    }

    if (clean.length() % 4 == 1) {
        return throwDOMException(
            "The string to be decoded is not correctly encoded.",
            "InvalidCharacterError");
    }

    std::vector<uint8_t> decoded;
    decoded.reserve((clean.length() / 4) * 3);
    size_t i = 0;
    size_t len = clean.length();
    while (i < len) {
        char c0 = clean[i++];
        char c1 = (i < len) ? clean[i++] : '=';
        char c2 = (i < len) ? clean[i++] : '=';
        char c3 = (i < len) ? clean[i++] : '=';

        int8_t d0 = decodeChar(c0);
        int8_t d1 = decodeChar(c1);
        int8_t d2 = (c2 == '=') ? 0 : decodeChar(c2);
        int8_t d3 = (c3 == '=') ? 0 : decodeChar(c3);

        if (d0 < 0 || d1 < 0 || (c2 != '=' && d2 < 0) || (c3 != '=' && d3 < 0) || (c2 == '=' && c3 != '=')) {
            return throwDOMException(
                "The string to be decoded is not correctly encoded.",
                "InvalidCharacterError");
        }

        uint32_t triple = (static_cast<uint32_t>(d0) << 18) |
                          (static_cast<uint32_t>(d1) << 12) |
                          (static_cast<uint32_t>(d2) << 6) |
                          static_cast<uint32_t>(d3);

        decoded.push_back(static_cast<uint8_t>((triple >> 16) & 255));
        if (c2 != '=') decoded.push_back(static_cast<uint8_t>((triple >> 8) & 255));
        if (c3 != '=') decoded.push_back(static_cast<uint8_t>(triple & 255));
    }

    std::string outUtf8;
    outUtf8.reserve(decoded.size() * 2);
    for (uint8_t byte : decoded) {
        if (byte < 0x80) {
            outUtf8.push_back(static_cast<char>(byte));
        } else {
            outUtf8.push_back(static_cast<char>(0xC0 | (byte >> 6)));
            outUtf8.push_back(static_cast<char>(0x80 | (byte & 0x3F)));
        }
    }
    return ev::fromUtf8(outUtf8);
}

} // namespace

void installBase64() {
    ev::Persistent btoaFn{ev::makeFunction(nativeBtoa, 1, "btoa")};
    ev::Persistent atobFn{ev::makeFunction(nativeAtob, 1, "atob")};

    ev::registerGlobal("btoa", btoaFn.get());
    ev::registerGlobal("atob", atobFn.get());

    ev::Persistent global;
    auto g = ev::globalValue("globalThis");
    if (g.found && ev::isObject(g.value)) {
        global.set(g.value);
        global.set(ev::setProperty(global.get(), "btoa", btoaFn.get()));
        global.set(ev::setProperty(global.get(), "atob", atobFn.get()));
    }
    auto w = ev::globalValue("window");
    if (w.found && ev::isObject(w.value) && w.value != global.get()) {
        ev::Persistent win{w.value};
        win.set(ev::setProperty(win.get(), "btoa", btoaFn.get()));
        win.set(ev::setProperty(win.get(), "atob", atobFn.get()));
    }

    bronze::embed::runEntry(bronze_base64_main);
}

} // namespace brokit::api
