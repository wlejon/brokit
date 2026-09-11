#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_readable_stream_main();

namespace brokit::api {

void installReadableStream()
{
    bronze::embed::runEntry(bronze_readable_stream_main);
}

} // namespace brokit::api
