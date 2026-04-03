#include "api/api.h"
#include "runtime/runtime.h"
#include "fetch_classes.js.h"

#include <cstring>

namespace brokit::api {

void installFetchClasses(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_fetch_classes, strlen(js_fetch_classes),
                        "<fetch-classes>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
