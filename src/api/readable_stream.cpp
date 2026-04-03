#include "api/api.h"
#include "runtime/runtime.h"
#include "readable_stream.js.h"

#include <cstring>

namespace brokit::api {

void installReadableStream(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_readable_stream, strlen(js_readable_stream),
                        "<readable_stream>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
