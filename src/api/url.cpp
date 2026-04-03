#include "api/api.h"
#include "runtime/runtime.h"
#include "url.js.h"

#include <cstring>

namespace brokit::api {

void installURL(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_url, strlen(js_url), "<url>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
