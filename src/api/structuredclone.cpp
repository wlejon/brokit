#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_structuredclone_main();

namespace brokit::api {

void installStructuredClone() {
    bronze::embed::runEntry(bronze_structuredclone_main);
}

} // namespace brokit::api

