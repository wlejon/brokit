#include "api/api.h"
#include "runtime/runtime.h"
#include "eventsource.js.h"

#include <cstring>

namespace brokit::api {

void installEventSource(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_eventsource, strlen(js_eventsource),
                        "<eventsource>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
