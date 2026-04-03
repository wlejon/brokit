#include "api/api.h"
#include "runtime/runtime.h"
#include "treewalker.js.h"

#include <cstring>

namespace brokit::api {

void installTreeWalker(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_treewalker, strlen(js_treewalker),
                        "<treewalker>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
