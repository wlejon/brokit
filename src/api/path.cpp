#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_path_main();

namespace brokit::api {

void installPath() {
    bronze::embed::runEntry(bronze_path_main);
}

} // namespace brokit::api
