#include "api/api.h"
#include "runtime/runtime.h"
#include "util.js.h"

#include <cstring>

namespace brokit::api {

void installUtil(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_util, strlen(js_util), "<util>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
