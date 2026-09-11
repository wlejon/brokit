#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_eventsource_main();

namespace brokit::api {

void installEventSource()
{
    bronze::embed::runEntry(bronze_eventsource_main);
}

} // namespace brokit::api
