#include "api/api.h"
#include "runtime/runtime.h"
#include "buffer.js.h"

#include <cstring>

namespace brokit::api {

void installBuffer(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_buffer, strlen(js_buffer), "<buffer>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
