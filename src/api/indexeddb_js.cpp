#include "api/api.h"
#include "runtime/runtime.h"
#include "indexeddb.js.h"

#include <cstring>

namespace brokit::api {

void installIndexedDBJS(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_indexeddb, strlen(js_indexeddb),
                        "<indexeddb>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
