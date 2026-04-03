#include "api/api.h"
#include "runtime/runtime.h"
#include "structuredclone.js.h"

#include <cstring>

namespace brokit::api {

void installStructuredClone(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_structuredclone, strlen(js_structuredclone),
                        "<structuredClone>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
