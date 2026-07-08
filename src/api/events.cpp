#include "api/api.h"
#include "runtime/runtime.h"
#include "events.js.h"

#include <cstring>

namespace brokit::api {

void installEvents(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_events, strlen(js_events), "<events>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
