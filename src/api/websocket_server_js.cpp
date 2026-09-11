#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_websocket_server_main();

namespace brokit::api {

void installWebSocketServerJS()
{
    bronze::embed::runEntry(bronze_websocket_server_main);
}

} // namespace brokit::api
