#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_net_main();
extern "C" void bronze_dgram_main();

namespace brokit::api {

void installNetJS()
{
    bronze::embed::runEntry(bronze_net_main);
    bronze::embed::runEntry(bronze_dgram_main);
}

} // namespace brokit::api
