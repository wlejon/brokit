#include "api/api.h"
#include "runtime/runtime.h"
#include <cstring>
#include <string>
#include <vector>

namespace brokit::api {

static thread_local JSClassID blob_class_id = 0;

struct BlobData {
    std::vector<uint8_t> bytes;
    std::string type;
};

static void blob_finalizer(JSRuntime*, JSValue val)
{
    auto* w = static_cast<BlobData*>(JS_GetOpaque(val, blob_class_id));
    delete w;
}

static JSClassDef blob_class_def = { "Blob", blob_finalizer };

static thread_local JSClassID file_class_id = 0;

struct FileData {
    BlobData blob;
    std::string name;
    double lastModified = 0;
};

static void file_finalizer(JSRuntime*, JSValue val)
{
    auto* w = static_cast<FileData*>(JS_GetOpaque(val, file_class_id));
    delete w;
}

static JSClassDef file_class_def = { "File", file_finalizer };

static JSValue newGetter(JSContext* ctx, JSValue (*fn)(JSContext*, JSValueConst),
                          const char* name)
{
    JSCFunctionType ft;
    ft.getter = fn;
    return JS_NewCFunction2(ctx, ft.generic, name, 0, JS_CFUNC_getter, 0);
}

static BlobData* getBlobData(JSContext* ctx, JSValueConst val)
{
    auto* bdata = static_cast<BlobData*>(JS_GetOpaque(val, blob_class_id));
    if (bdata) return bdata;
    auto* fdata = static_cast<FileData*>(JS_GetOpaque(val, file_class_id));
    if (fdata) return &fdata->blob;
    JS_ThrowTypeError(ctx, "not a Blob");
    return nullptr;
}

static bool flattenPart(JSContext* ctx, JSValueConst part, std::vector<uint8_t>& out)
{
    if (JS_IsString(part)) {
        const char* str = JS_ToCString(ctx, part);
        if (!str) return false;
        size_t len = strlen(str);
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(str),
                   reinterpret_cast<const uint8_t*>(str) + len);
        JS_FreeCString(ctx, str);
        return true;
    }

    auto* bdata = static_cast<BlobData*>(JS_GetOpaque(part, blob_class_id));
    if (!bdata) {
        auto* fdata = static_cast<FileData*>(JS_GetOpaque(part, file_class_id));
        if (fdata) bdata = &fdata->blob;
    }
    if (bdata) {
        out.insert(out.end(), bdata->bytes.begin(), bdata->bytes.end());
        return true;
    }

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
    JS_FreeValue(ctx, JS_GetException(ctx));

    size_t abLen = 0;
    uint8_t* ptr = JS_GetArrayBuffer(ctx, &abLen, part);
    if (ptr) {
        out.insert(out.end(), ptr, ptr + abLen);
        return true;
    }

    const char* str = JS_ToCString(ctx, part);
    if (!str) return false;
    size_t len = strlen(str);
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(str),
               reinterpret_cast<const uint8_t*>(str) + len);
    JS_FreeCString(ctx, str);
    return true;
}

static JSValue blob_slice(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* data = getBlobData(ctx, this_val);
    if (!data) return JS_EXCEPTION;
    
    int64_t size = static_cast<int64_t>(data->bytes.size());
    int64_t start = 0;
    int64_t end = size;
    
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        JS_ToInt64(ctx, &start, argv[0]);
        if (start < 0) start = std::max<int64_t>(0, size + start);
        else start = std::min<int64_t>(size, start);
    }
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        JS_ToInt64(ctx, &end, argv[1]);
        if (end < 0) end = std::max<int64_t>(0, size + end);
        else end = std::min<int64_t>(size, end);
    }
    
    std::string contentType;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        const char* ctStr = JS_ToCString(ctx, argv[2]);
        if (ctStr) {
            contentType = ctStr;
            for (auto& c : contentType) c = static_cast<char>(tolower(c));
            JS_FreeCString(ctx, ctStr);
        }
    }
    
    auto* newData = new BlobData();
    newData->type = contentType;
    if (start < end) {
        newData->bytes.assign(data->bytes.begin() + start, data->bytes.begin() + end);
    }
    
    JSValue proto = JS_GetClassProto(ctx, blob_class_id);
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, blob_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        delete newData;
        return obj;
    }
    JS_SetOpaque(obj, newData);
    return obj;
}

static JSValue blob_text(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* data = getBlobData(ctx, this_val);
    if (!data) return JS_EXCEPTION;
    
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) return promise;
    
    JSValue str = JS_NewStringLen(ctx, reinterpret_cast<const char*>(data->bytes.data()), data->bytes.size());
    JSValue ret = JS_Call(ctx, resolving_funcs[0], JS_UNDEFINED, 1, &str);
    JS_FreeValue(ctx, str);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return promise;
}

static JSValue blob_array_buffer(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* data = getBlobData(ctx, this_val);
    if (!data) return JS_EXCEPTION;
    
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) return promise;
    
    JSValue ab = JS_NewArrayBufferCopy(ctx, data->bytes.data(), data->bytes.size());
    JSValue ret = JS_Call(ctx, resolving_funcs[0], JS_UNDEFINED, 1, &ab);
    JS_FreeValue(ctx, ab);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return promise;
}

static JSValue blob_bytes(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv)
{
    auto* data = getBlobData(ctx, this_val);
    if (!data) return JS_EXCEPTION;
    
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) return promise;
    
    JSValue ab = JS_NewArrayBufferCopy(ctx, data->bytes.data(), data->bytes.size());
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue u8ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
    JSValue u8arr = JS_CallConstructor(ctx, u8ctor, 1, &ab);
    JS_FreeValue(ctx, u8ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, ab);
    
    JSValue ret = JS_Call(ctx, resolving_funcs[0], JS_UNDEFINED, 1, &u8arr);
    JS_FreeValue(ctx, u8arr);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return promise;
}

static JSValue js_blob_size(JSContext* ctx, JSValueConst this_val)
{
    auto* data = getBlobData(ctx, this_val);
    if (!data) return JS_ThrowTypeError(ctx, "not a Blob");
    return JS_NewFloat64(ctx, static_cast<double>(data->bytes.size()));
}

static JSValue js_blob_type(JSContext* ctx, JSValueConst this_val)
{
    auto* data = getBlobData(ctx, this_val);
    if (!data) return JS_ThrowTypeError(ctx, "not a Blob");
    return JS_NewString(ctx, data->type.c_str());
}

static JSValue js_blob_constructor(JSContext* ctx, JSValueConst new_target,
                                    int argc, JSValueConst* argv)
{
    auto* data = new BlobData();
    
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (!JS_IsArray(argv[0])) {
            delete data;
            return JS_ThrowTypeError(ctx, "Blob parts must be an sequence/array");
        }
        JSValue lenVal = JS_GetPropertyStr(ctx, argv[0], "length");
        uint32_t len = 0;
        JS_ToUint32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (uint32_t i = 0; i < len; i++) {
            JSValue part = JS_GetPropertyUint32(ctx, argv[0], i);
            bool ok = flattenPart(ctx, part, data->bytes);
            JS_FreeValue(ctx, part);
            if (!ok) {
                delete data;
                return JS_EXCEPTION;
            }
        }
    }
    
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_IsObject(argv[1])) {
        JSValue typeVal = JS_GetPropertyStr(ctx, argv[1], "type");
        if (JS_IsString(typeVal)) {
            const char* typeStr = JS_ToCString(ctx, typeVal);
            if (typeStr) {
                data->type = typeStr;
                for (auto& c : data->type) c = static_cast<char>(tolower(c));
                JS_FreeCString(ctx, typeStr);
            }
        }
        JS_FreeValue(ctx, typeVal);
    }
    
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) {
        delete data;
        return proto;
    }
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, blob_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        delete data;
        return obj;
    }
    JS_SetOpaque(obj, data);
    return obj;
}

static JSValue js_file_name(JSContext* ctx, JSValueConst this_val)
{
    auto* data = static_cast<FileData*>(JS_GetOpaque2(ctx, this_val, file_class_id));
    if (!data) return JS_ThrowTypeError(ctx, "not a File");
    return JS_NewString(ctx, data->name.c_str());
}

static JSValue js_file_lastModified(JSContext* ctx, JSValueConst this_val)
{
    auto* data = static_cast<FileData*>(JS_GetOpaque2(ctx, this_val, file_class_id));
    if (!data) return JS_ThrowTypeError(ctx, "not a File");
    return JS_NewFloat64(ctx, data->lastModified);
}

static JSValue js_file_webkitRelativePath(JSContext* ctx, JSValueConst this_val)
{
    auto* data = static_cast<FileData*>(JS_GetOpaque2(ctx, this_val, file_class_id));
    if (!data) return JS_ThrowTypeError(ctx, "not a File");
    return JS_NewString(ctx, "");
}

static JSValue js_file_path(JSContext* ctx, JSValueConst this_val)
{
    auto* data = static_cast<FileData*>(JS_GetOpaque2(ctx, this_val, file_class_id));
    if (!data) return JS_ThrowTypeError(ctx, "not a File");
    return JS_NewString(ctx, "");
}

static JSValue js_file_constructor(JSContext* ctx, JSValueConst new_target,
                                    int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "File constructor requires at least 2 arguments (bits, name)");
    if (!JS_IsArray(argv[0])) return JS_ThrowTypeError(ctx, "File bits must be a sequence/array");
    
    const char* nameStr = JS_ToCString(ctx, argv[1]);
    if (!nameStr) return JS_EXCEPTION;
    
    auto* data = new FileData();
    data->name = nameStr;
    JS_FreeCString(ctx, nameStr);
    
    JSValue lenVal = JS_GetPropertyStr(ctx, argv[0], "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);
    for (uint32_t i = 0; i < len; i++) {
        JSValue part = JS_GetPropertyUint32(ctx, argv[0], i);
        bool ok = flattenPart(ctx, part, data->blob.bytes);
        JS_FreeValue(ctx, part);
        if (!ok) {
            delete data;
            return JS_EXCEPTION;
        }
    }
    
    data->lastModified = 0;
    if (argc > 2 && !JS_IsUndefined(argv[2]) && JS_IsObject(argv[2])) {
        JSValue typeVal = JS_GetPropertyStr(ctx, argv[2], "type");
        if (JS_IsString(typeVal)) {
            const char* typeStr = JS_ToCString(ctx, typeVal);
            if (typeStr) {
                data->blob.type = typeStr;
                for (auto& c : data->blob.type) c = static_cast<char>(tolower(c));
                JS_FreeCString(ctx, typeStr);
            }
        }
        JS_FreeValue(ctx, typeVal);
    
        JSValue lmVal = JS_GetPropertyStr(ctx, argv[2], "lastModified");
        if (JS_IsNumber(lmVal)) {
            JS_ToFloat64(ctx, &data->lastModified, lmVal);
        }
        JS_FreeValue(ctx, lmVal);
    }
    
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) {
        delete data;
        return proto;
    }
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, file_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        delete data;
        return obj;
    }
    JS_SetOpaque(obj, data);
    return obj;
}

bool blobBytes(JSContext* ctx, JSValueConst val, const uint8_t** data, size_t* len, std::string* type)
{
    auto* bdata = static_cast<BlobData*>(JS_GetOpaque(val, blob_class_id));
    if (bdata) {
        *data = bdata->bytes.data();
        *len = bdata->bytes.size();
        if (type) *type = bdata->type;
        return true;
    }
    auto* fdata = static_cast<FileData*>(JS_GetOpaque(val, file_class_id));
    if (fdata) {
        *data = fdata->blob.bytes.data();
        *len = fdata->blob.bytes.size();
        if (type) *type = fdata->blob.type;
        return true;
    }
    return false;
}

void installBlob(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue global = JS_GetGlobalObject(ctx);

    // Register Blob class
    if (blob_class_id == 0) JS_NewClassID(rt, &blob_class_id);
    JS_NewClass(rt, blob_class_id, &blob_class_def);

    JSValue blobProto = JS_NewObject(ctx);

    JSAtom blob_size_atom = JS_NewAtom(ctx, "size");
    JS_DefinePropertyGetSet(ctx, blobProto, blob_size_atom,
                            newGetter(ctx, js_blob_size, "size"),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, blob_size_atom);
    JSAtom blob_type_atom = JS_NewAtom(ctx, "type");
    JS_DefinePropertyGetSet(ctx, blobProto, blob_type_atom,
                            newGetter(ctx, js_blob_type, "type"),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, blob_type_atom);

    JS_SetPropertyStr(ctx, blobProto, "slice",
        JS_NewCFunction(ctx, blob_slice, "slice", 3));
    JS_SetPropertyStr(ctx, blobProto, "text",
        JS_NewCFunction(ctx, blob_text, "text", 0));
    JS_SetPropertyStr(ctx, blobProto, "arrayBuffer",
        JS_NewCFunction(ctx, blob_array_buffer, "arrayBuffer", 0));
    JS_SetPropertyStr(ctx, blobProto, "bytes",
        JS_NewCFunction(ctx, blob_bytes, "bytes", 0));

    JS_SetClassProto(ctx, blob_class_id, blobProto);

    JSValue blobCtor = JS_NewCFunction2(ctx, js_blob_constructor, "Blob", 2,
                                         JS_CFUNC_constructor, 0);
    blobProto = JS_GetClassProto(ctx, blob_class_id);
    JS_SetPropertyStr(ctx, blobCtor, "prototype", JS_DupValue(ctx, blobProto));
    JS_SetPropertyStr(ctx, blobProto, "constructor", JS_DupValue(ctx, blobCtor));
    JS_FreeValue(ctx, blobProto);

    JS_SetPropertyStr(ctx, global, "Blob", blobCtor);

    // Register File class
    if (file_class_id == 0) JS_NewClassID(rt, &file_class_id);
    JS_NewClass(rt, file_class_id, &file_class_def);

    JSValue parentProto = JS_GetClassProto(ctx, blob_class_id);
    JSValue fileProto = JS_NewObjectProto(ctx, parentProto);
    JS_FreeValue(ctx, parentProto);

    JSAtom file_name_atom = JS_NewAtom(ctx, "name");
    JS_DefinePropertyGetSet(ctx, fileProto, file_name_atom,
                            newGetter(ctx, js_file_name, "name"),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, file_name_atom);
    JSAtom file_lastModified_atom = JS_NewAtom(ctx, "lastModified");
    JS_DefinePropertyGetSet(ctx, fileProto, file_lastModified_atom,
                            newGetter(ctx, js_file_lastModified, "lastModified"),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, file_lastModified_atom);
    JSAtom file_webkitRelativePath_atom = JS_NewAtom(ctx, "webkitRelativePath");
    JS_DefinePropertyGetSet(ctx, fileProto, file_webkitRelativePath_atom,
                            newGetter(ctx, js_file_webkitRelativePath, "webkitRelativePath"),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, file_webkitRelativePath_atom);
    JSAtom file_path_atom = JS_NewAtom(ctx, "path");
    JS_DefinePropertyGetSet(ctx, fileProto, file_path_atom,
                            newGetter(ctx, js_file_path, "path"),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, file_path_atom);

    JS_SetClassProto(ctx, file_class_id, fileProto);

    JSValue fileCtor = JS_NewCFunction2(ctx, js_file_constructor, "File", 3,
                                         JS_CFUNC_constructor, 0);
    fileProto = JS_GetClassProto(ctx, file_class_id);
    JS_SetPropertyStr(ctx, fileCtor, "prototype", JS_DupValue(ctx, fileProto));
    JS_SetPropertyStr(ctx, fileProto, "constructor", JS_DupValue(ctx, fileCtor));
    JS_FreeValue(ctx, fileProto);

    JS_SetPropertyStr(ctx, global, "File", fileCtor);

    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
