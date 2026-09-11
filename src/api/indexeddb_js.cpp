#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_indexeddb_main();

namespace brokit::api {

void installIndexedDBJS()
{
    bronze::embed::runEntry(bronze_indexeddb_main);
}

} // namespace brokit::api
