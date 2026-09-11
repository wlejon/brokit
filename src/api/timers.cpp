#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_timers_main();

namespace brokit::api {

void installTimers() {
    bronze::embed::runEntry(bronze_timers_main);
}

} // namespace brokit::api

