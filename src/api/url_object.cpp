#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_url_object_main();

namespace brokit::api {

void installURLObject() {
    bronze::embed::runEntry(bronze_url_object_main);
}

} // namespace brokit::api
