#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_websocket_main();

namespace brokit::api {

void installWebSocketJS()
{
    bronze::embed::runEntry(bronze_websocket_main);
}

} // namespace brokit::api
