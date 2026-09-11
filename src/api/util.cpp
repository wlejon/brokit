#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_util_main();

namespace brokit::api {

void installUtil()
{
    bronze::embed::runEntry(bronze_util_main);
}

} // namespace brokit::api
