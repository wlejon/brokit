#include "api/api.h"
#include "runtime/runtime.h"
#include "websocket_server.js.h"

#include <cstring>

namespace brokit::api {

// WebSocketServer rides the `net` module (TCP listener + sockets), so this
// must install after installNetJS; it also uses TextEncoder/TextDecoder,
// btoa, and setTimeout (api.cpp orders it so).
void installWebSocketServerJS(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_websocket_server, strlen(js_websocket_server),
                        "<websocket-server>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
