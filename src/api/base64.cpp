#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_base64_main();

namespace brokit::api {

void installBase64() {
    bronze::embed::runEntry(bronze_base64_main);
}

} // namespace brokit::api
