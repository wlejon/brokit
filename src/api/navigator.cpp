#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_navigator_main();

namespace brokit::api {

void installNavigator() {
    bronze::embed::runEntry(bronze_navigator_main);
}

} // namespace brokit::api
