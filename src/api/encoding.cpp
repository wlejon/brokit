#include "api/api.h"
#include "api/arg_reader.h"
#include "embed/embed.h"

#include <string>
#include <string_view>

extern "C" void bronze_encoding_main();

namespace brokit::api {

namespace {

Value encoderEncode(Value, std::span<const Value> a) {
    std::string s = hasArg(a, 0) ? strAt(a, 0) : "";
    Value arr = ev::createTypedArray(ev::elements::Uint8, static_cast<uint32_t>(s.size()));
    if (!s.empty()) {
        ev::fillTypedArray(arr, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
    }
    return arr;
}

Value decoderDecode(Value, std::span<const Value> a) {
    if (a.empty() || ev::isUndefined(a[0]) || ev::isNull(a[0])) {
        return ev::fromUtf8("");
    }
    const uint8_t* data = nullptr;
    size_t len = 0;
    if (!bufferBytes(a[0], &data, &len)) {
        return ev::throwTypeError("TextDecoder.decode: expected ArrayBuffer or TypedArray");
    }
    if (len == 0 || !data) {
        return ev::fromUtf8("");
    }
    return ev::fromUtf8(std::string_view(reinterpret_cast<const char*>(data), len));
}

} // namespace

void installEncoding() {
    Value encFn = ev::makeFunction(encoderEncode, 1, "__brokit_textencoder_encode");
    Value decFn = ev::makeFunction(decoderDecode, 1, "__brokit_textdecoder_decode");

    ev::registerGlobal("__brokit_textencoder_encode", encFn);
    ev::registerGlobal("__brokit_textdecoder_decode", decFn);

    auto g = ev::globalValue("globalThis");
    if (g.found && ev::isObject(g.value)) {
        ev::setProperty(g.value, "__brokit_textencoder_encode", encFn);
        ev::setProperty(g.value, "__brokit_textdecoder_decode", decFn);
    }

    bronze::embed::runEntry(bronze_encoding_main);
}

} // namespace brokit::api
