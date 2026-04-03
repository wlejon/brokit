#include "api/api.h"
#include "runtime/runtime.h"
#include "event_target.js.h"

#include <cstring>

namespace brokit::api {

void installEventTarget(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_event_target, strlen(js_event_target),
                        "<event-target>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
