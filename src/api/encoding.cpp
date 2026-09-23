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
    // Copy out first: `data` points into the heap, and fromUtf8 allocates the
    // result string before it reads its input, so a collection there would
    // leave it reading the buffer's abandoned address.
    const std::string bytes(reinterpret_cast<const char*>(data), len);
    return ev::fromUtf8(bytes);
}

} // namespace

void installEncoding() {
    ev::setGlobalValue("__brokit_textencoder_encode",
                       ev::makeFunction(encoderEncode, 1, "__brokit_textencoder_encode"));
    ev::setGlobalValue("__brokit_textdecoder_decode",
                       ev::makeFunction(decoderDecode, 1, "__brokit_textdecoder_decode"));

    bronze::embed::runEntry(bronze_encoding_main);
}

} // namespace brokit::api
