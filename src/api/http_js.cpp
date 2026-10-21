#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_http_main();

namespace brokit::api {

void installHttpJS()
{
    bronze::embed::runEntry(bronze_http_main);
}

} // namespace brokit::api
