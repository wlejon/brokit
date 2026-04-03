#include "api/api.h"
#include "runtime/runtime.h"
#include "path.js.h"

#include <cstring>

namespace brokit::api {

void installPath(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_path, strlen(js_path), "<path>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
