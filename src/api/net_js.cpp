#include "api/api.h"
#include "runtime/runtime.h"
#include "net.js.h"

#include <cstring>

namespace brokit::api {

// JS layer for the native net bindings: the Node-compat `net` module.
// Extends EventEmitter, so installNetJS must run after installEvents
// (api.cpp orders it so).
void installNetJS(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_net, strlen(js_net), "<net>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
