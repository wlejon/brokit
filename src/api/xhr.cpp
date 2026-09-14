#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_xhr_main();

namespace brokit::api {

void installXMLHttpRequest()
{
    bronze::embed::runEntry(bronze_xhr_main);
}

} // namespace brokit::api
