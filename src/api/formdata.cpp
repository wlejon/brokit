#include "api/api.h"
#include "embed/embed.h"

extern "C" void bronze_formdata_main();

namespace brokit::api {

void installFormData()
{
    bronze::embed::runEntry(bronze_formdata_main);
}

} // namespace brokit::api
