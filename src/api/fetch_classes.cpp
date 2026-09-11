#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_fetch_classes_main();

namespace brokit::api {

void installFetchClasses()
{
    bronze::embed::runEntry(bronze_fetch_classes_main);
}

} // namespace brokit::api
