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
    installStorage(ctx);
    installIndexedDB(ctx);
    installIndexedDBJS(ctx);
    installReadableStream(ctx);
    installFetch(ctx);
    installWritableStream(ctx);
    installFS(ctx);
    installChildProcess(ctx);
    installWebSocket(ctx);
    installWebSocketJS(ctx);
    installEventSource(ctx);
    installFormData(ctx);
}

} // namespace brokit::api
