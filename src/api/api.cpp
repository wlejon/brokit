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
    installAbortController(ctx);
    installStructuredClone(ctx);
    installBlob(ctx);
    installURLObject(ctx);
    installProcess(ctx);
    installOS(ctx);
    installPath(ctx);
    installFetch(ctx);
    installFS(ctx);
    installChildProcess(ctx);
}

} // namespace brokit::api
