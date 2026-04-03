#include "api/api.h"
#include "runtime/runtime.h"
#include "navigator.js.h"

#include <cstring>

namespace brokit::api {

void installNavigator(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_navigator, strlen(js_navigator),
                        "<navigator>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
