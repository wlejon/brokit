#include "api/api.h"
#include "runtime/runtime.h"
#include "base64.js.h"

#include <cstring>

namespace brokit::api {

void installBase64(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_base64, strlen(js_base64),
                        "<base64>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
