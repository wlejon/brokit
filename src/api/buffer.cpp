#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_buffer_main();

namespace brokit::api {

void installBuffer()
{
    bronze::embed::runEntry(bronze_buffer_main);
}

} // namespace brokit::api
