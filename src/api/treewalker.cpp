#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_treewalker_main();

namespace brokit::api {

void installTreeWalker()
{
    bronze::embed::runEntry(bronze_treewalker_main);
}

} // namespace brokit::api
