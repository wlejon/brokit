#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_writable_stream_main();

namespace brokit::api {

void installWritableStream()
{
    bronze::embed::runEntry(bronze_writable_stream_main);
}

} // namespace brokit::api
