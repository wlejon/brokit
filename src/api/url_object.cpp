#include "api/api.h"
#include "runtime/runtime.h"
#include "url_object.js.h"

#include <cstring>

namespace brokit::api {

void installURLObject(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_url_object, strlen(js_url_object),
                        "<url_object>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
