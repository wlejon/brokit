#include "api/api.h"

namespace brokit::api {

void installAll(JSContext* ctx)
{
    installConsole(ctx);
    installTimers(ctx);
    installURL(ctx);
    installCrypto(ctx);
    installEncoding(ctx);
    installTreeWalker(ctx);
}

} // namespace brokit::api
