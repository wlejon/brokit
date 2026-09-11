#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_event_target_main();

namespace brokit::api {

void installEventTarget()
{
    bronze::embed::runEntry(bronze_event_target_main);
}

} // namespace brokit::api
