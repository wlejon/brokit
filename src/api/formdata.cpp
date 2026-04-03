#include "api/api.h"
#include "runtime/runtime.h"
#include "formdata.js.h"

#include <cstring>

namespace brokit::api {

void installFormData(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_formdata, strlen(js_formdata),
                        "<FormData>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
