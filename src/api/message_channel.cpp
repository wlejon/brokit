#include "api/api.h"
#include "runtime/runtime.h"
#include "message_channel.js.h"

#include <cstring>

namespace brokit::api {

void installMessageChannel(JSContext* ctx)
{
    JSValue r = JS_Eval(ctx, js_message_channel, strlen(js_message_channel),
                        "<message-channel>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
