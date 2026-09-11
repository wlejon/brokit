#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_events_main();

namespace brokit::api {

void installEvents()
{
    bronze::embed::runEntry(bronze_events_main);
}

} // namespace brokit::api
