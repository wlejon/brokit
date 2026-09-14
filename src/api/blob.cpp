#include "api/api.h"
#include "api/host_class.h"
#include "api/arg_reader.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace brokit::api {

namespace {

inline constexpr uint32_t kBlobTag = 0x424C4F42u; // 'BLOB'
inline constexpr uint32_t kFileTag = 0x46494C45u; // 'FILE'
inline constexpr uint32_t kReaderTag = 0x46524452u; // 'FRDR'

struct BlobData {
    uint32_t tag = kBlobTag;
    std::vector<uint8_t> bytes;
    std::string type;
};

struct FileData {
    uint32_t tag = kFileTag;
    BlobData blob;
    std::string name;
    double lastModified = 0;
    // Path relative to the directory the file was picked or dropped from;
    // empty for a File built any other way.
    std::string webkitRelativePath;
};

struct ReaderData {
    uint32_t tag = kReaderTag;
    int readyState = 0; // 0: EMPTY, 1: LOADING, 2: DONE
    ev::Persistent result;
    ev::Persistent error;
    ev::Persistent onload;
    ev::Persistent onerror;
    ev::Persistent onloadend;
};

HostClass g_blobClass;
HostClass g_fileClass;
HostClass g_fileReaderClass;

void blobDtor(void* p) { delete static_cast<BlobData*>(p); }
void fileDtor(void* p) { delete static_cast<FileData*>(p); }
void readerDtor(void* p) { delete static_cast<ReaderData*>(p); }

BlobData* getBlobData(Value v) {
    auto* b = static_cast<BlobData*>(ev::handleData(v));
    if (!b) return nullptr;
    if (b->tag == kBlobTag) return b;
    if (b->tag == kFileTag) return &reinterpret_cast<FileData*>(b)->blob;
    return nullptr;
}

FileData* getFileData(Value v) {
    auto* f = static_cast<FileData*>(ev::handleData(v));
    if (!f || f->tag != kFileTag) return nullptr;
    return f;
}

void appendPart(std::vector<uint8_t>& out, Value part) {
    if (const BlobData* b = getBlobData(part)) {
        out.insert(out.end(), b->bytes.begin(), b->bytes.end());
        return;
    }
    const uint8_t* data = nullptr;
    size_t len = 0;
    if (bufferBytes(part, &data, &len)) {
        if (len > 0 && data) {
            out.insert(out.end(), data, data + len);
        }
        return;
    }
    if (ev::isUndefined(part) || ev::isNull(part)) return;
    std::string s = ev::toUtf8(part);
    out.insert(out.end(), s.begin(), s.end());
}

std::vector<uint8_t> collectParts(Value partsValue) {
    std::vector<uint8_t> out;
    if (!ev::isObject(partsValue)) return out;
    ev::Persistent parts(partsValue);
    Value lenV = ev::getProperty(parts.get(), "length");
    if (ev::isUndefined(lenV) || ev::isObject(lenV)) return out;
    uint32_t len = static_cast<uint32_t>(ev::toDouble(lenV));
    for (uint32_t i = 0; i < len; ++i) {
        appendPart(out, ev::getElement(parts.get(), i));
    }
    return out;
}

Value blobSlice(Value thisVal, std::span<const Value> a) {
    BlobData* b = getBlobData(thisVal);
    if (!b) return ev::throwTypeError("Blob.slice: receiver is not a Blob");
    int64_t size = static_cast<int64_t>(b->bytes.size());
    int64_t start = a.size() > 0 ? i64At(a, 0) : 0;
    int64_t end = a.size() > 1 ? i64At(a, 1) : size;
    std::string type = a.size() > 2 ? strAt(a, 2) : "";

    if (start < 0) start = std::max<int64_t>(0, size + start);
    if (start > size) start = size;
    if (end < 0) end = std::max<int64_t>(0, size + end);
    if (end > size) end = size;
    if (end < start) end = start;

    auto* sliceBlob = new BlobData();
    sliceBlob->tag = kBlobTag;
    sliceBlob->type = type;
    sliceBlob->bytes.assign(b->bytes.begin() + start, b->bytes.begin() + end);

    return g_blobClass.make(sliceBlob, blobDtor);
}

Value blobArrayBuffer(Value thisVal, std::span<const Value>) {
    BlobData* b = getBlobData(thisVal);
    if (!b) return ev::throwTypeError("Blob.arrayBuffer: receiver is not a Blob");
    Value p = ev::createPromise();
    Value ab = ev::createArrayBuffer(std::span<const uint8_t>(b->bytes.data(), b->bytes.size()));
    ev::resolvePromise(p, ab);
    return p;
}

Value blobText(Value thisVal, std::span<const Value>) {
    BlobData* b = getBlobData(thisVal);
    if (!b) return ev::throwTypeError("Blob.text: receiver is not a Blob");
    Value p = ev::createPromise();
    Value s = ev::fromUtf8(std::string_view(reinterpret_cast<const char*>(b->bytes.data()), b->bytes.size()));
    ev::resolvePromise(p, s);
    return p;
}

} // namespace

bool blobBytes(Value val, const uint8_t** data, size_t* len, std::string* type) {
    BlobData* b = getBlobData(val);
    if (!b) return false;
    *data = b->bytes.data();
    *len = b->bytes.size();
    if (type) *type = b->type;
    return true;
}

bool setFileWebkitRelativePath(Value file, std::string_view path) {
    FileData* f = getFileData(file);
    if (!f) return false;
    f->webkitRelativePath.assign(path.begin(), path.end());
    return true;
}

void installBlob() {
    g_blobClass.install("Blob", 0,
        [](Value, std::span<const Value> a) {
            auto* b = new BlobData();
            b->tag = kBlobTag;
            if (a.size() > 0 && ev::isObject(a[0])) {
                b->bytes = collectParts(a[0]);
            }
            if (a.size() > 1 && ev::isObject(a[1])) {
                Value t = ev::getProperty(a[1], "type");
                if (ev::isString(t)) {
                    std::string s = ev::toUtf8(t);
                    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
                    b->type = s;
                }
            }
            return g_blobClass.make(b, blobDtor);
        },
        [](ObjectBuilder& proto) {
            proto.accessor("size", [](Value thisVal, std::span<const Value>) {
                BlobData* b = getBlobData(thisVal);
                return ev::fromDouble(b ? static_cast<double>(b->bytes.size()) : 0.0);
            });
            proto.accessor("type", [](Value thisVal, std::span<const Value>) {
                BlobData* b = getBlobData(thisVal);
                return ev::fromUtf8(b ? b->type : "");
            });
            proto.def("slice", 0, blobSlice);
            proto.def("arrayBuffer", 0, blobArrayBuffer);
            proto.def("text", 0, blobText);
        }
    );

    g_fileClass.install("File", 2,
        [](Value, std::span<const Value> a) {
            if (a.size() < 2 || ev::isUndefined(a[1])) return ev::throwTypeError("File constructor: at least 2 arguments required");
            auto* f = new FileData();
            f->tag = kFileTag;
            f->blob.tag = kBlobTag;
            if (ev::isObject(a[0])) {
                f->blob.bytes = collectParts(a[0]);
            }
            f->name = strAt(a, 1);
            f->lastModified = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            if (a.size() > 2 && ev::isObject(a[2])) {
                Value t = ev::getProperty(a[2], "type");
                if (ev::isString(t)) {
                    std::string s = ev::toUtf8(t);
                    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
                    f->blob.type = s;
                }
                Value lm = ev::getProperty(a[2], "lastModified");
                if (ev::isNumber(lm)) f->lastModified = ev::toDouble(lm);
                Value rp = ev::getProperty(a[2], "webkitRelativePath");
                if (ev::isString(rp)) f->webkitRelativePath = ev::toUtf8(rp);
            }
            return g_fileClass.make(f, fileDtor);
        },
        [](ObjectBuilder& proto) {
            proto.accessor("name", [](Value thisVal, std::span<const Value>) {
                FileData* f = getFileData(thisVal);
                return ev::fromUtf8(f ? f->name : "");
            });
            proto.accessor("lastModified", [](Value thisVal, std::span<const Value>) {
                FileData* f = getFileData(thisVal);
                return ev::fromDouble(f ? f->lastModified : 0.0);
            });
            proto.accessor("webkitRelativePath", [](Value thisVal, std::span<const Value>) {
                FileData* f = getFileData(thisVal);
                return ev::fromUtf8(f ? f->webkitRelativePath : "");
            });
        }
    );
    g_fileClass.inherit(g_blobClass);

    g_fileReaderClass.install("FileReader", 0,
        [](Value, std::span<const Value>) {
            auto* r = new ReaderData();
            return g_fileReaderClass.make(r, readerDtor);
        },
        [](ObjectBuilder& proto) {
            proto.accessor("readyState", [](Value thisVal, std::span<const Value>) {
                auto* r = static_cast<ReaderData*>(ev::handleData(thisVal));
                return ev::fromDouble(r ? static_cast<double>(r->readyState) : 0.0);
            });
            proto.accessor("result", [](Value thisVal, std::span<const Value>) {
                auto* r = static_cast<ReaderData*>(ev::handleData(thisVal));
                return r ? r->result.get() : ev::null();
            });
            proto.def("readAsArrayBuffer", 1, [](Value thisVal, std::span<const Value> a) {
                auto* r = static_cast<ReaderData*>(ev::handleData(thisVal));
                if (!r || a.empty()) return ev::undefined();
                BlobData* b = getBlobData(a[0]);
                if (!b) return ev::throwTypeError("readAsArrayBuffer: argument 1 is not a Blob");
                r->readyState = 2;
                r->result.set(ev::createArrayBuffer(std::span<const uint8_t>(b->bytes.data(), b->bytes.size())));
                Value onload = ev::getProperty(thisVal, "onload");
                if (ev::isFunction(onload)) ev::call(onload, thisVal, {});
                return ev::undefined();
            });
            proto.def("readAsText", 1, [](Value thisVal, std::span<const Value> a) {
                auto* r = static_cast<ReaderData*>(ev::handleData(thisVal));
                if (!r || a.empty()) return ev::undefined();
                BlobData* b = getBlobData(a[0]);
                if (!b) return ev::throwTypeError("readAsText: argument 1 is not a Blob");
                r->readyState = 2;
                r->result.set(ev::fromUtf8(std::string_view(reinterpret_cast<const char*>(b->bytes.data()), b->bytes.size())));
                Value onload = ev::getProperty(thisVal, "onload");
                if (ev::isFunction(onload)) ev::call(onload, thisVal, {});
                return ev::undefined();
            });
        }
    );
}

} // namespace brokit::api
