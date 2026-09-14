#include "api/api.h"
#include "embed/embed.h"

#include <array>

extern "C" void bronze_url_object_main();

namespace brokit::api {

void installURLObject() {
    bronze::embed::runEntry(bronze_url_object_main);
}

bool blobByObjectURL(const std::string& url, Value* outBlob)
{
    ev::Persistent lookup(ev::getGlobal("__brokit_getBlobByURL"));
    if (!ev::isFunction(lookup.get())) return false;

    Value arg = ev::fromUtf8(url);
    ev::CallResult r = ev::call(lookup.get(), ev::undefined(), std::array<Value, 1>{arg});
    if (r.thrown || !ev::isObject(r.value)) return false;

    const uint8_t* data = nullptr;
    size_t len = 0;
    if (!blobBytes(r.value, &data, &len)) return false;

    if (outBlob) *outBlob = r.value;
    return true;
}

} // namespace brokit::api
