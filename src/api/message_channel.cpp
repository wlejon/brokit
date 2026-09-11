#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_message_channel_main();

namespace brokit::api {

void installMessageChannel()
{
    bronze::embed::runEntry(bronze_message_channel_main);
}

} // namespace brokit::api
