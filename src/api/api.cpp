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
    installReadableStream(ctx);
    installFetch(ctx);
    installFS(ctx);
    installChildProcess(ctx);
    installEventSource(ctx);
}

} // namespace brokit::api
