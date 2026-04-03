#include "api/api.h"
#include "runtime/runtime.h"
#include "writable_stream.js.h"

#include <cstring>

namespace brokit::api {

void installWritableStream(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_writable_stream, strlen(js_writable_stream),
                        "<writable_stream>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
