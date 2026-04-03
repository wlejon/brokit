#include "api/api.h"
#include "runtime/runtime.h"
#include "abort.js.h"

#include <cstring>

namespace brokit::api {

void installAbortController(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_abort, strlen(js_abort), "<abort>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
