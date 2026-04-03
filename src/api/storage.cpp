#include "api/api.h"
#include "runtime/runtime.h"

#include <cstring>
#include <fstream>
#include <map>
#include <string>

extern "C" {
#include "quickjs.h"
}

namespace brokit::api {

// ---------------------------------------------------------------------------
// Per-context storage state (heap-allocated, pointer stashed in JS global)
// ---------------------------------------------------------------------------

struct StorageState {
    std::map<std::string, std::string> storage;
    std::string storagePath; // empty = in-memory only (sessionStorage)
};

static const char* kLocalStorageKey = "__brokit_localStorage_ptr";
static const char* kSessionStorageKey = "__brokit_sessionStorage_ptr";

static StorageState* getState(JSContext* ctx, const char* key) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, key);
    StorageState* state = nullptr;
    if (JS_IsNumber(val)) {
        int64_t ptr = 0;
        JS_ToInt64(ctx, &ptr, val);
        state = reinterpret_cast<StorageState*>(static_cast<intptr_t>(ptr));
    }
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, global);
    return state;
}

// ---------------------------------------------------------------------------
// JSON persistence (minimal, no external deps)
// ---------------------------------------------------------------------------

static void loadStorage(StorageState* state)
{
    state->storage.clear();
    if (state->storagePath.empty()) return;

    std::ifstream file(state->storagePath);
    if (!file.is_open()) return;

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    size_t pos = content.find('{');
    if (pos == std::string::npos) return;
    pos++;

    auto parseString = [&](size_t& p) -> std::string {
        if (p >= content.size() || content[p] != '"') return "";
        p++;
        std::string result;
        while (p < content.size() && content[p] != '"') {
            if (content[p] == '\\' && p + 1 < content.size()) {
                p++;
                switch (content[p]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case 'n': result += '\n'; break;
                    case 't': result += '\t'; break;
                    default: result += content[p]; break;
                }
            } else {
                result += content[p];
            }
            p++;
        }
        if (p < content.size()) p++;
        return result;
    };

    while (pos < content.size()) {
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\n' ||
               content[pos] == '\r' || content[pos] == '\t' || content[pos] == ','))
            pos++;
        if (pos >= content.size() || content[pos] == '}') break;

        std::string key = parseString(pos);
        while (pos < content.size() && content[pos] != ':') pos++;
        if (pos < content.size()) pos++;
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) pos++;

        std::string value = parseString(pos);
        if (!key.empty()) {
            state->storage[key] = value;
        }
    }
}

static void saveStorage(StorageState* state)
{
    if (!state || state->storagePath.empty()) return;

    std::ofstream file(state->storagePath);
    if (!file.is_open()) return;

    auto escapeJson = [](const std::string& s) -> std::string {
        std::string result;
        for (char c : s) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\t': result += "\\t"; break;
                default: result += c; break;
            }
        }
        return result;
    };

    file << "{\n";
    bool first = true;
    for (auto& [key, val] : state->storage) {
        if (!first) file << ",\n";
        file << "  \"" << escapeJson(key) << "\": \"" << escapeJson(val) << "\"";
        first = false;
    }
    file << "\n}\n";
}

// ---------------------------------------------------------------------------
// Shared JS callback implementations
// ---------------------------------------------------------------------------

static std::string jsStr(JSContext* ctx, JSValueConst val) {
    const char* s = JS_ToCString(ctx, val);
    std::string r = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    return r;
}

// Template-based factory for storage JS functions — avoids duplicating
// localStorage and sessionStorage implementations.
struct StorageFunctions {
    const char* stateKey;

    StorageState* get(JSContext* ctx) const {
        return getState(ctx, stateKey);
    }
};

static StorageFunctions g_localFns  = { kLocalStorageKey };
static StorageFunctions g_sessionFns = { kSessionStorageKey };

// Macro to define JS functions for a storage type
#define DEFINE_STORAGE_FUNCS(PREFIX, FNSPTR)                                 \
static JSValue PREFIX##_getItem(JSContext* ctx, JSValueConst, int argc,      \
                                JSValueConst* argv) {                        \
    if (argc < 1) return JS_NULL;                                            \
    auto* st = FNSPTR.get(ctx); if (!st) return JS_NULL;                     \
    auto it = st->storage.find(jsStr(ctx, argv[0]));                         \
    if (it == st->storage.end()) return JS_NULL;                             \
    return JS_NewString(ctx, it->second.c_str());                            \
}                                                                            \
static JSValue PREFIX##_setItem(JSContext* ctx, JSValueConst, int argc,      \
                                JSValueConst* argv) {                        \
    if (argc < 2) return JS_UNDEFINED;                                       \
    auto* st = FNSPTR.get(ctx); if (!st) return JS_UNDEFINED;                \
    st->storage[jsStr(ctx, argv[0])] = jsStr(ctx, argv[1]);                  \
    saveStorage(st);                                                         \
    return JS_UNDEFINED;                                                     \
}                                                                            \
static JSValue PREFIX##_removeItem(JSContext* ctx, JSValueConst, int argc,   \
                                   JSValueConst* argv) {                     \
    if (argc < 1) return JS_UNDEFINED;                                       \
    auto* st = FNSPTR.get(ctx); if (!st) return JS_UNDEFINED;                \
    st->storage.erase(jsStr(ctx, argv[0]));                                  \
    saveStorage(st);                                                         \
    return JS_UNDEFINED;                                                     \
}                                                                            \
static JSValue PREFIX##_clear(JSContext* ctx, JSValueConst, int,             \
                              JSValueConst*) {                               \
    auto* st = FNSPTR.get(ctx); if (!st) return JS_UNDEFINED;                \
    st->storage.clear();                                                     \
    saveStorage(st);                                                         \
    return JS_UNDEFINED;                                                     \
}                                                                            \
static JSValue PREFIX##_key(JSContext* ctx, JSValueConst, int argc,          \
                            JSValueConst* argv) {                            \
    if (argc < 1) return JS_NULL;                                            \
    auto* st = FNSPTR.get(ctx); if (!st) return JS_NULL;                     \
    int32_t idx = 0; JS_ToInt32(ctx, &idx, argv[0]);                         \
    if (idx < 0 || (size_t)idx >= st->storage.size()) return JS_NULL;        \
    auto it = st->storage.begin(); std::advance(it, idx);                    \
    return JS_NewString(ctx, it->first.c_str());                             \
}                                                                            \
static JSValue PREFIX##_length(JSContext* ctx, JSValueConst) {               \
    auto* st = FNSPTR.get(ctx);                                              \
    if (!st) return JS_NewInt32(ctx, 0);                                     \
    return JS_NewInt32(ctx, (int32_t)st->storage.size());                    \
}                                                                            \
static const JSCFunctionListEntry PREFIX##_funcs[] = {                       \
    JS_CFUNC_DEF("getItem", 1, PREFIX##_getItem),                            \
    JS_CFUNC_DEF("setItem", 2, PREFIX##_setItem),                            \
    JS_CFUNC_DEF("removeItem", 1, PREFIX##_removeItem),                      \
    JS_CFUNC_DEF("clear", 0, PREFIX##_clear),                                \
    JS_CFUNC_DEF("key", 1, PREFIX##_key),                                    \
    JS_CGETSET_DEF("length", PREFIX##_length, nullptr),                      \
};

DEFINE_STORAGE_FUNCS(ls, g_localFns)
DEFINE_STORAGE_FUNCS(ss, g_sessionFns)

// ---------------------------------------------------------------------------
// Install helpers
// ---------------------------------------------------------------------------

static void installStorageObject(JSContext* ctx, const char* name,
                                 const char* stateKey, StorageState* state,
                                 const JSCFunctionListEntry* funcs, int count)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, stateKey,
                      JS_NewInt64(ctx, static_cast<int64_t>(
                          reinterpret_cast<intptr_t>(state))));

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, obj, funcs, count);
    JS_SetPropertyStr(ctx, global, name, obj);
    JS_FreeValue(ctx, global);
}

void installStorage(JSContext* ctx)
{
    // localStorage — in-memory by default, call setStoragePath() for persistence
    auto* localState = new StorageState();
    installStorageObject(ctx, "localStorage", kLocalStorageKey, localState,
                         ls_funcs, sizeof(ls_funcs) / sizeof(ls_funcs[0]));

    // sessionStorage — always in-memory
    auto* sessionState = new StorageState();
    installStorageObject(ctx, "sessionStorage", kSessionStorageKey, sessionState,
                         ss_funcs, sizeof(ss_funcs) / sizeof(ss_funcs[0]));
}

void setStoragePath(JSContext* ctx, const std::string& path)
{
    auto* state = getState(ctx, kLocalStorageKey);
    if (state) {
        state->storagePath = path;
        loadStorage(state);
    }
}

void cleanupStorage(JSContext* ctx)
{
    auto* ls = getState(ctx, kLocalStorageKey);
    delete ls;
    auto* ss = getState(ctx, kSessionStorageKey);
    delete ss;

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, kLocalStorageKey, JS_UNDEFINED);
    JS_SetPropertyStr(ctx, global, kSessionStorageKey, JS_UNDEFINED);
    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
