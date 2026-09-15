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
    uint64_t generation = 0;
    ev::Persistent result;
    ev::Persistent error;
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

Value blobBytesMethod(Value thisVal, std::span<const Value>) {
    BlobData* b = getBlobData(thisVal);
    if (!b) return ev::throwTypeError("Blob.bytes: receiver is not a Blob");
    Value p = ev::createPromise();
    Value u8 = ev::createTypedArray(ev::elements::Uint8, static_cast<uint32_t>(b->bytes.size()));
    ev::fillTypedArray(u8, std::span<const uint8_t>(b->bytes.data(), b->bytes.size()));
    ev::resolvePromise(p, u8);
    return p;
}

std::string base64Encode(const std::vector<uint8_t>& in) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t n = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) |
                           uint32_t(in[i + 2]);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += kAlphabet[(n >> 6) & 63];
        out += kAlphabet[n & 63];
    }
    if (i + 1 == in.size()) {
        const uint32_t n = uint32_t(in[i]) << 16;
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const uint32_t n = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += kAlphabet[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

static HostTaskPoster g_hostTaskPoster;

std::string readerListenerKey(const std::string& type) {
    return "__brokitListeners_" + type;
}

void addReaderListener(Value target, const std::string& type, Value fn) {
    if (!ev::isFunction(fn)) return;
    ev::Persistent targetP(target);
    ev::Persistent fnP(fn);
    const std::string key = readerListenerKey(type);
    Value list = ev::getProperty(targetP.get(), key);
    if (!ev::isObject(list)) {
        list = ev::createObject();
        ev::setProperty(list, "length", ev::fromDouble(0));
        ev::setProperty(targetP.get(), key, list);
    }
    Value lenV = ev::getProperty(list, "length");
    uint32_t len = ev::isNumber(lenV) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
    for (uint32_t i = 0; i < len; ++i) {
        Value existing = ev::getElement(list, i);
        if (ev::toBits(existing) == ev::toBits(fnP.get())) return;
    }
    ev::setElement(list, len, fnP.get());
    ev::setProperty(list, "length", ev::fromDouble(len + 1));
}

void removeReaderListener(Value target, const std::string& type, Value fn) {
    if (!ev::isFunction(fn)) return;
    ev::Persistent targetP(target);
    ev::Persistent fnP(fn);
    const std::string key = readerListenerKey(type);
    Value list = ev::getProperty(targetP.get(), key);
    if (!ev::isObject(list)) return;
    Value lenV = ev::getProperty(list, "length");
    uint32_t len = ev::isNumber(lenV) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
    uint32_t found = len;
    for (uint32_t i = 0; i < len; ++i) {
        Value existing = ev::getElement(list, i);
        if (ev::toBits(existing) == ev::toBits(fnP.get())) {
            found = i;
            break;
        }
    }
    if (found == len) return;
    for (uint32_t i = found + 1; i < len; ++i) {
        Value moved = ev::getElement(list, i);
        ev::setElement(list, i - 1, moved);
    }
    ev::setElement(list, len - 1, ev::undefined());
    ev::setProperty(list, "length", ev::fromDouble(len - 1));
}

void dispatchReaderEvent(Value target, const std::string& type) {
    ev::Persistent targetP(target);
    std::vector<ev::Persistent> handlers;
    {
        Value on = ev::getProperty(targetP.get(), "on" + type);
        if (ev::isFunction(on)) handlers.emplace_back(on);
    }
    {
        Value list = ev::getProperty(targetP.get(), readerListenerKey(type));
        if (ev::isObject(list)) {
            Value lenV = ev::getProperty(list, "length");
            uint32_t len = ev::isNumber(lenV) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
            for (uint32_t i = 0; i < len; ++i) {
                Value h = ev::getElement(list, i);
                if (ev::isFunction(h)) handlers.emplace_back(h);
            }
        }
    }
    if (handlers.empty()) return;

    for (ev::Persistent& handler : handlers) {
        ev::Persistent evt(ev::createObject());
        evt.set(ev::setProperty(evt.get(), "type", ev::fromUtf8(type)));
        evt.set(ev::setProperty(evt.get(), "target", targetP.get()));
        Value arg = evt.get();
        ev::call(handler.get(), targetP.get(), std::span<const Value>(&arg, 1));
    }
}

void startRead(Value self, Value blobValue,
               std::function<Value(const std::vector<uint8_t>&)> produce) {
    auto* r = static_cast<ReaderData*>(ev::handleData(self));
    if (!r) return;
    BlobData* blob = getBlobData(blobValue);

    r->readyState = 1; // LOADING
    r->result.set(ev::null());
    r->error.set(ev::null());

    const uint64_t generation = ++r->generation;
    std::vector<uint8_t> bytes = blob ? blob->bytes : std::vector<uint8_t>();
    const bool haveBlob = blob != nullptr;

    ev::Persistent target(self);

    auto task = [target, generation, bytes = std::move(bytes), haveBlob,
                 produce = std::move(produce)]() mutable {
        Value selfVal = target.get();
        auto* reader = static_cast<ReaderData*>(ev::handleData(selfVal));
        if (!reader || reader->generation != generation) return;

        reader->generation = generation;
        reader->readyState = 2; // DONE

        if (!haveBlob) {
            ObjectBuilder err;
            err.set("name", ev::fromUtf8("NotFoundError"));
            err.set("message", ev::fromUtf8("FileReader: argument is not a Blob"));
            reader->error.set(err.get());
            dispatchReaderEvent(target.get(), "loadstart");
            dispatchReaderEvent(target.get(), "error");
            dispatchReaderEvent(target.get(), "loadend");
            return;
        }

        reader->result.set(produce(bytes));
        dispatchReaderEvent(target.get(), "loadstart");
        dispatchReaderEvent(target.get(), "progress");
        dispatchReaderEvent(target.get(), "load");
        dispatchReaderEvent(target.get(), "loadend");
    };

    if (g_hostTaskPoster) {
        g_hostTaskPoster(std::move(task));
    } else {
        Value setTimeoutFn = ev::getProperty(ev::globalValue("globalThis").value, "setTimeout");
        if (ev::isFunction(setTimeoutFn)) {
            auto sharedTask = std::make_shared<std::function<void()>>(std::move(task));
            Value cb = ev::makeFunction([sharedTask](Value, std::span<const Value>) {
                (*sharedTask)();
                return ev::undefined();
            }, 0, "fileReaderTask");
            Value zero = ev::fromDouble(0);
            Value args[2] = { cb, zero };
            ev::call(setTimeoutFn, ev::undefined(), args);
        }
    }
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

void setHostTaskPoster(HostTaskPoster poster) {
    g_hostTaskPoster = std::move(poster);
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
            proto.def("bytes", 0, blobBytesMethod);
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
            Value obj = g_fileReaderClass.make(r, readerDtor);
            for (const char* slot : {"onload", "onerror", "onloadend", "onloadstart",
                                     "onprogress", "onabort"}) {
                ev::setProperty(obj, slot, ev::null());
            }
            return obj;
        },
        [](ObjectBuilder& proto) {
            proto.set("EMPTY", ev::fromDouble(0));
            proto.set("LOADING", ev::fromDouble(1));
            proto.set("DONE", ev::fromDouble(2));

            proto.accessor("readyState", [](Value thisVal, std::span<const Value>) {
                auto* r = static_cast<ReaderData*>(ev::handleData(thisVal));
                return ev::fromDouble(r ? static_cast<double>(r->readyState) : 0.0);
            });
            proto.accessor("result", [](Value thisVal, std::span<const Value>) {
                auto* r = static_cast<ReaderData*>(ev::handleData(thisVal));
                return (r && !ev::isUndefined(r->result.get())) ? r->result.get() : ev::null();
            });
            proto.accessor("error", [](Value thisVal, std::span<const Value>) {
                auto* r = static_cast<ReaderData*>(ev::handleData(thisVal));
                return (r && !ev::isUndefined(r->error.get())) ? r->error.get() : ev::null();
            });

            proto.def("readAsArrayBuffer", 1, [](Value thisVal, std::span<const Value> a) {
                if (a.empty()) return ev::undefined();
                startRead(thisVal, a[0], [](const std::vector<uint8_t>& bytes) {
                    return ev::createArrayBuffer(std::span<const uint8_t>(bytes.data(), bytes.size()));
                });
                return ev::undefined();
            });
            proto.def("readAsText", 1, [](Value thisVal, std::span<const Value> a) {
                if (a.empty()) return ev::undefined();
                startRead(thisVal, a[0], [](const std::vector<uint8_t>& bytes) {
                    return ev::fromUtf8(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                });
                return ev::undefined();
            });
            proto.def("readAsBinaryString", 1, [](Value thisVal, std::span<const Value> a) {
                if (a.empty()) return ev::undefined();
                startRead(thisVal, a[0], [](const std::vector<uint8_t>& bytes) {
                    std::string utf8;
                    utf8.reserve(bytes.size() * 2);
                    for (uint8_t c : bytes) {
                        if (c < 0x80) {
                            utf8 += static_cast<char>(c);
                        } else {
                            utf8 += static_cast<char>(0xC0 | (c >> 6));
                            utf8 += static_cast<char>(0x80 | (c & 0x3F));
                        }
                    }
                    return ev::fromUtf8(utf8);
                });
                return ev::undefined();
            });
            proto.def("readAsDataURL", 1, [](Value thisVal, std::span<const Value> a) {
                if (a.empty()) return ev::undefined();
                BlobData* b = getBlobData(a[0]);
                std::string mime = b && !b->type.empty() ? b->type : "application/octet-stream";
                startRead(thisVal, a[0], [mime](const std::vector<uint8_t>& bytes) {
                    return ev::fromUtf8("data:" + mime + ";base64," + base64Encode(bytes));
                });
                return ev::undefined();
            });
            proto.def("abort", 0, [](Value thisVal, std::span<const Value>) {
                auto* r = static_cast<ReaderData*>(ev::handleData(thisVal));
                if (!r) return ev::undefined();
                ++r->generation;
                r->readyState = 2; // DONE
                r->result.set(ev::null());
                dispatchReaderEvent(thisVal, "abort");
                dispatchReaderEvent(thisVal, "loadend");
                return ev::undefined();
            });
            proto.def("addEventListener", 2, [](Value thisVal, std::span<const Value> a) {
                if (a.size() < 2) return ev::undefined();
                std::string type = ev::toUtf8(a[0]);
                addReaderListener(thisVal, type, a[1]);
                return ev::undefined();
            });
            proto.def("removeEventListener", 2, [](Value thisVal, std::span<const Value> a) {
                if (a.size() < 2) return ev::undefined();
                std::string type = ev::toUtf8(a[0]);
                removeReaderListener(thisVal, type, a[1]);
                return ev::undefined();
            });
        }
    );
    g_fileReaderClass.setStatic("EMPTY", ev::fromDouble(0));
    g_fileReaderClass.setStatic("LOADING", ev::fromDouble(1));
    g_fileReaderClass.setStatic("DONE", ev::fromDouble(2));
}

} // namespace brokit::api
