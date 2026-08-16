#include "api/api.h"
#include "runtime/runtime.h"

#include <cstring>
#include <string>
#include <vector>

namespace brokit::api {

// Blob data is stored as an opaque pointer in the JS object.
// Each Blob holds a contiguous byte buffer + MIME type string.

struct BlobData {
    std::vector<uint8_t> bytes;
    std::string type;
};

// thread_local: each thread (main + each worker) has its own JSRuntime,
// so each must allocate its own class ID from that runtime's counter.
// Sharing across threads causes ID collisions with other classes.
static thread_local JSClassID blobClassId = 0;
static thread_local JSClassID fileClassId = 0;

// File extends Blob — FileData embeds BlobData as its first member so methods
// on Blob.prototype can retrieve the underlying bytes from a File instance.
struct FileData {
    BlobData blob;
    std::string name;
    double lastModified;
};

static void blobFinalizer(JSRuntime*, JSValue val)
{
    auto* data = static_cast<BlobData*>(JS_GetOpaque(val, blobClassId));
    delete data;
}

static JSClassDef blobClassDef = {
    "Blob",
    blobFinalizer,
    nullptr, nullptr, nullptr
};

// Helper: extract bytes from a Blob or File JS object. File extends Blob,
// so methods on Blob.prototype must accept File instances too.
static BlobData* getBlobData(JSContext* ctx, JSValueConst val)
{
    auto* bdata = static_cast<BlobData*>(JS_GetOpaque(val, blobClassId));
    if (bdata) return bdata;
    auto* fdata = static_cast<FileData*>(JS_GetOpaque(val, fileClassId));
    if (fdata) return &fdata->blob;
    JS_ThrowTypeError(ctx, "not a Blob");
    return nullptr;
}

// Helper: flatten a single "part" (string, ArrayBuffer, TypedArray, or Blob) into bytes
static bool flattenPart(JSContext* ctx, JSValueConst part, std::vector<uint8_t>& out)
{
    // String?
    if (JS_IsString(part)) {
        const char* str = JS_ToCString(ctx, part);
        if (!str) return false;
        size_t len = strlen(str);
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(str),
                   reinterpret_cast<const uint8_t*>(str) + len);
        JS_FreeCString(ctx, str);
        return true;
    }

    // Blob?
    auto* bdata = static_cast<BlobData*>(JS_GetOpaque(part, blobClassId));
    if (!bdata) {
        // File? (File extends Blob)
        auto* fdata = static_cast<FileData*>(JS_GetOpaque(part, fileClassId));
        if (fdata) bdata = &fdata->blob;
    }
    if (bdata) {
        out.insert(out.end(), bdata->bytes.begin(), bdata->bytes.end());
        return true;
    }

    // TypedArray?
    size_t byte_offset = 0, byte_len = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, part, &byte_offset, &byte_len, &bpe);
    if (!JS_IsException(buf)) {
        size_t abLen = 0;
        uint8_t* ptr = JS_GetArrayBuffer(ctx, &abLen, buf);
        if (ptr) {
            out.insert(out.end(), ptr + byte_offset, ptr + byte_offset + byte_len);
        }
        JS_FreeValue(ctx, buf);
        return true;
    }
    // Clear the TypedArray exception
    JS_FreeValue(ctx, JS_GetException(ctx));

    // ArrayBuffer?
    size_t abLen = 0;
    uint8_t* ptr = JS_GetArrayBuffer(ctx, &abLen, part);
    if (ptr) {
        out.insert(out.end(), ptr, ptr + abLen);
        return true;
    }

    // Unknown — convert to string
    const char* str = JS_ToCString(ctx, part);
    if (!str) return false;
    size_t len = strlen(str);
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(str),
               reinterpret_cast<const uint8_t*>(str) + len);
    JS_FreeCString(ctx, str);
    return true;
}

// new Blob(parts?, options?)
static JSValue js_blob_constructor(JSContext* ctx, JSValueConst new_target,
                                    int argc, JSValueConst* argv)
{
    auto* data = new BlobData();

    // Process parts array
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        JSValue lengthVal = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lengthVal)) {
            delete data;
            return lengthVal;
        }
        uint32_t len = 0;
        JS_ToUint32(ctx, &len, lengthVal);
        JS_FreeValue(ctx, lengthVal);

        for (uint32_t i = 0; i < len; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, argv[0], i);
            if (JS_IsException(item)) {
                delete data;
                return item;
            }
            bool ok = flattenPart(ctx, item, data->bytes);
            JS_FreeValue(ctx, item);
            if (!ok) {
                delete data;
                return JS_EXCEPTION;
            }
        }
    }

    // Process options
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue typeVal = JS_GetPropertyStr(ctx, argv[1], "type");
        if (JS_IsString(typeVal)) {
            const char* t = JS_ToCString(ctx, typeVal);
            if (t) {
                data->type = t;
                // Normalize to lowercase per spec
                for (auto& c : data->type) c = static_cast<char>(tolower(c));
                JS_FreeCString(ctx, t);
            }
        }
        JS_FreeValue(ctx, typeVal);
    }

    // Create the JS object using the prototype from new.target
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) {
        delete data;
        return proto;
    }
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, blobClassId);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        delete data;
        return obj;
    }
    JS_SetOpaque(obj, data);
    return obj;
}

// Helper to create a getter JSValue from a 2-arg getter function
static JSValue newGetter(JSContext* ctx, JSValue (*fn)(JSContext*, JSValueConst),
                          const char* name)
{
    JSCFunctionType ft;
    ft.getter = fn;
    return JS_NewCFunction2(ctx, ft.generic, name, 0, JS_CFUNC_getter, 0);
}

// Blob.prototype.size
static JSValue js_blob_size(JSContext* ctx, JSValueConst this_val)
{
    auto* data = getBlobData(ctx, this_val);
    if (!data) return JS_ThrowTypeError(ctx, "not a Blob");
    return JS_NewFloat64(ctx, static_cast<double>(data->bytes.size()));
}

// Blob.prototype.type
static JSValue js_blob_type(JSContext* ctx, JSValueConst this_val)
{
    auto* data = getBlobData(ctx, this_val);
    if (!data) return JS_ThrowTypeError(ctx, "not a Blob");
    return JS_NewString(ctx, data->type.c_str());
}

// Blob.prototype.slice(start?, end?, contentType?)
static JSValue js_blob_slice(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv)
{
    auto* data = getBlobData(ctx, this_val);
    if (!data) return JS_ThrowTypeError(ctx, "not a Blob");

    int64_t size = static_cast<int64_t>(data->bytes.size());
    int64_t start = 0, end = size;

    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        JS_ToInt64(ctx, &start, argv[0]);
        if (start < 0) start = std::max<int64_t>(size + start, 0);
        else start = std::min(start, size);
    }
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        JS_ToInt64(ctx, &end, argv[1]);
        if (end < 0) end = std::max<int64_t>(size + end, 0);
        else end = std::min(end, size);
    }

    std::string contentType;
    if (argc >= 3 && JS_IsString(argv[2])) {
        const char* ct = JS_ToCString(ctx, argv[2]);
        if (ct) {
            contentType = ct;
            for (auto& c : contentType) c = static_cast<char>(tolower(c));
            JS_FreeCString(ctx, ct);
        }
    }

    int64_t span = std::max<int64_t>(end - start, 0);

    // Create new Blob with sliced data
    auto* sliced = new BlobData();
    if (span > 0) {
        sliced->bytes.assign(data->bytes.begin() + start, data->bytes.begin() + start + span);
    }
    sliced->type = contentType;

    // Get the Blob constructor's prototype
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue blobCtor = JS_GetPropertyStr(ctx, global, "Blob");
    JSValue proto = JS_GetPropertyStr(ctx, blobCtor, "prototype");
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, blobClassId);
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, blobCtor);
    JS_FreeValue(ctx, global);

    if (JS_IsException(obj)) {
        delete sliced;
        return obj;
    }
    JS_SetOpaque(obj, sliced);
    return obj;
}

// Blob.prototype.arrayBuffer() — returns a Promise that resolves with an ArrayBuffer
static JSValue js_blob_arrayBuffer(JSContext* ctx, JSValueConst this_val,
                                    int, JSValueConst*)
{
    auto* data = getBlobData(ctx, this_val);
    if (!data) return JS_ThrowTypeError(ctx, "not a Blob");

    JSValue ab = JS_NewArrayBufferCopy(ctx, data->bytes.data(),
                                        data->bytes.size());
    if (JS_IsException(ab)) return ab;

    // Wrap in resolved promise
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, ab);
        return promise;
    }
    JSValue ret = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &ab);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, ab);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

// Blob.prototype.text() — returns a Promise that resolves with a string
static JSValue js_blob_text(JSContext* ctx, JSValueConst this_val,
                             int, JSValueConst*)
{
    auto* data = getBlobData(ctx, this_val);
    if (!data) return JS_ThrowTypeError(ctx, "not a Blob");

    JSValue str = JS_NewStringLen(ctx, reinterpret_cast<const char*>(data->bytes.data()),
                                  data->bytes.size());
    if (JS_IsException(str)) return str;

    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, str);
        return promise;
    }
    JSValue ret = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &str);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, str);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

// File extends Blob — FileData is declared at top of file so getBlobData() can
// unwrap File instances. fileClassId is also declared up top.
static void fileFinalizer(JSRuntime*, JSValue val)
{
    auto* data = static_cast<FileData*>(JS_GetOpaque(val, fileClassId));
    delete data;
}

static JSClassDef fileClassDef = {
    "File",
    fileFinalizer,
    nullptr, nullptr, nullptr
};

// new File(parts, name, options?)
static JSValue js_file_constructor(JSContext* ctx, JSValueConst new_target,
                                    int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "File requires at least 2 arguments");

    auto* data = new FileData();

    // Process parts array (same as Blob)
    if (!JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        JSValue lengthVal = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lengthVal)) { delete data; return lengthVal; }
        uint32_t len = 0;
        JS_ToUint32(ctx, &len, lengthVal);
        JS_FreeValue(ctx, lengthVal);

        for (uint32_t i = 0; i < len; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, argv[0], i);
            if (JS_IsException(item)) { delete data; return item; }
            bool ok = flattenPart(ctx, item, data->blob.bytes);
            JS_FreeValue(ctx, item);
            if (!ok) { delete data; return JS_EXCEPTION; }
        }
    }

    // Name
    const char* name = JS_ToCString(ctx, argv[1]);
    if (!name) { delete data; return JS_EXCEPTION; }
    data->name = name;
    JS_FreeCString(ctx, name);

    // Default lastModified to now
    data->lastModified = 0; // Will set via JS Date.now() below if not provided

    // Options
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue typeVal = JS_GetPropertyStr(ctx, argv[2], "type");
        if (JS_IsString(typeVal)) {
            const char* t = JS_ToCString(ctx, typeVal);
            if (t) {
                data->blob.type = t;
                for (auto& c : data->blob.type) c = static_cast<char>(tolower(c));
                JS_FreeCString(ctx, t);
            }
        }
        JS_FreeValue(ctx, typeVal);

        JSValue lmVal = JS_GetPropertyStr(ctx, argv[2], "lastModified");
        if (!JS_IsUndefined(lmVal)) {
            double lm = 0;
            JS_ToFloat64(ctx, &lm, lmVal);
            data->lastModified = lm;
        }
        JS_FreeValue(ctx, lmVal);
    }

    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) { delete data; return proto; }
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, fileClassId);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) { delete data; return obj; }
    JS_SetOpaque(obj, data);
    return obj;
}

static JSValue js_file_name(JSContext* ctx, JSValueConst this_val)
{
    auto* data = static_cast<FileData*>(JS_GetOpaque2(ctx, this_val, fileClassId));
    if (!data) return JS_ThrowTypeError(ctx, "not a File");
    return JS_NewString(ctx, data->name.c_str());
}

static JSValue js_file_lastModified(JSContext* ctx, JSValueConst this_val)
{
    auto* data = static_cast<FileData*>(JS_GetOpaque2(ctx, this_val, fileClassId));
    if (!data) return JS_ThrowTypeError(ctx, "not a File");
    return JS_NewFloat64(ctx, data->lastModified);
}

static JSValue js_file_size(JSContext* ctx, JSValueConst this_val)
{
    auto* data = static_cast<FileData*>(JS_GetOpaque2(ctx, this_val, fileClassId));
    if (!data) return JS_ThrowTypeError(ctx, "not a File");
    return JS_NewFloat64(ctx, static_cast<double>(data->blob.bytes.size()));
}

static JSValue js_file_type(JSContext* ctx, JSValueConst this_val)
{
    auto* data = static_cast<FileData*>(JS_GetOpaque2(ctx, this_val, fileClassId));
    if (!data) return JS_ThrowTypeError(ctx, "not a File");
    return JS_NewString(ctx, data->blob.type.c_str());
}

// See api.h. A host-side view of a Blob's bytes, so a native consumer can
// resolve an object URL at the moment it is created rather than a microtask
// later, when whatever asked for it has already given up.
bool blobBytes(JSContext* ctx, JSValueConst val, const uint8_t** data,
               size_t* len, std::string* type)
{
    if (data) *data = nullptr;
    if (len) *len = 0;
    auto* bdata = static_cast<BlobData*>(JS_GetOpaque(val, blobClassId));
    if (!bdata) {
        auto* fdata = static_cast<FileData*>(JS_GetOpaque(val, fileClassId));
        if (fdata) bdata = &fdata->blob;
    }
    if (!bdata) return false;   // not a Blob; no exception, the caller asked
    (void)ctx;
    if (data) *data = bdata->bytes.data();
    if (len) *len = bdata->bytes.size();
    if (type) *type = bdata->type;
    return true;
}

void installBlob(JSContext* ctx)
{
    // Register Blob class
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (blobClassId == 0) JS_NewClassID(rt, &blobClassId);
    JS_NewClass(rt, blobClassId, &blobClassDef);

    // Blob constructor
    JSValue blobProto = JS_NewObject(ctx);

    // Getters
    JSAtom sizeAtom = JS_NewAtom(ctx, "size");
    JS_DefinePropertyGetSet(ctx, blobProto, sizeAtom,
                            newGetter(ctx, js_blob_size, "size"),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, sizeAtom);

    JSAtom typeAtom = JS_NewAtom(ctx, "type");
    JS_DefinePropertyGetSet(ctx, blobProto, typeAtom,
                            newGetter(ctx, js_blob_type, "type"),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, typeAtom);

    // Methods
    JS_SetPropertyStr(ctx, blobProto, "slice",
                      JS_NewCFunction(ctx, js_blob_slice, "slice", 3));
    JS_SetPropertyStr(ctx, blobProto, "arrayBuffer",
                      JS_NewCFunction(ctx, js_blob_arrayBuffer, "arrayBuffer", 0));
    JS_SetPropertyStr(ctx, blobProto, "text",
                      JS_NewCFunction(ctx, js_blob_text, "text", 0));

    JS_SetClassProto(ctx, blobClassId, blobProto);

    JSValue blobCtor = JS_NewCFunction2(ctx, js_blob_constructor, "Blob", 2,
                                         JS_CFUNC_constructor, 0);
    // Re-get proto since SetClassProto consumed our ref
    blobProto = JS_GetClassProto(ctx, blobClassId);
    JS_SetPropertyStr(ctx, blobCtor, "prototype", JS_DupValue(ctx, blobProto));
    JS_SetPropertyStr(ctx, blobProto, "constructor", JS_DupValue(ctx, blobCtor));
    JS_FreeValue(ctx, blobProto);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "Blob", blobCtor);

    // Register File class
    if (fileClassId == 0) JS_NewClassID(rt, &fileClassId);
    JS_NewClass(rt, fileClassId, &fileClassDef);

    // File prototype inherits from Blob prototype (File extends Blob)
    blobProto = JS_GetClassProto(ctx, blobClassId);
    JSValue fileProto = JS_NewObjectProto(ctx, blobProto);
    JS_FreeValue(ctx, blobProto);

    // File getters: name, lastModified, size, type
    JSAtom nameAtom = JS_NewAtom(ctx, "name");
    JS_DefinePropertyGetSet(ctx, fileProto, nameAtom,
                            newGetter(ctx, js_file_name, "name"),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, nameAtom);

    JSAtom lmAtom = JS_NewAtom(ctx, "lastModified");
    JS_DefinePropertyGetSet(ctx, fileProto, lmAtom,
                            newGetter(ctx, js_file_lastModified, "lastModified"),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, lmAtom);

    JSAtom fSizeAtom = JS_NewAtom(ctx, "size");
    JS_DefinePropertyGetSet(ctx, fileProto, fSizeAtom,
                            newGetter(ctx, js_file_size, "size"),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, fSizeAtom);

    JSAtom fTypeAtom = JS_NewAtom(ctx, "type");
    JS_DefinePropertyGetSet(ctx, fileProto, fTypeAtom,
                            newGetter(ctx, js_file_type, "type"),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, fTypeAtom);

    JS_SetClassProto(ctx, fileClassId, fileProto);

    JSValue fileCtor = JS_NewCFunction2(ctx, js_file_constructor, "File", 3,
                                         JS_CFUNC_constructor, 0);
    fileProto = JS_GetClassProto(ctx, fileClassId);
    JS_SetPropertyStr(ctx, fileCtor, "prototype", JS_DupValue(ctx, fileProto));
    JS_SetPropertyStr(ctx, fileProto, "constructor", JS_DupValue(ctx, fileCtor));

    JS_FreeValue(ctx, fileProto);
    JS_SetPropertyStr(ctx, global, "File", fileCtor);
    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
