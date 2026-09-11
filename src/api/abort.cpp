#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_abort_main();

namespace brokit::api {

void installAbortController() {
    bronze::embed::runEntry(bronze_abort_main);
}

} // namespace brokit::api

