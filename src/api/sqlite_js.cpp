#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_sqlite_main();

namespace brokit::api {

void installSqliteJS()
{
    bronze::embed::runEntry(bronze_sqlite_main);
}

} // namespace brokit::api
