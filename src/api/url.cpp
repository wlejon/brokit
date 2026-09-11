#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_url_main();

namespace brokit::api {

void installURL() {
    bronze::embed::runEntry(bronze_url_main);
}

} // namespace brokit::api
