#include "api/api.h"
#include "runtime/runtime.h"
#include "websocket.js.h"

#include <cstring>

namespace brokit::api {

void installWebSocketJS(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_websocket, strlen(js_websocket),
                        "<websocket>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
