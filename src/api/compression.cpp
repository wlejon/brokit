// CompressionStream / DecompressionStream — native codec layer.
//
// This file implements the incremental DEFLATE codec behind the web-standard
// CompressionStream / DecompressionStream classes (the class shells live in
// js/compression.js and wrap this in a TransformStream). Three formats:
//
//   "deflate"      ZLIB-wrapped DEFLATE (RFC 1950) — header + adler32 trailer
//   "deflate-raw"  raw DEFLATE (RFC 1951) — no wrapper
//   "gzip"         gzip (RFC 1952) — header + crc32/size trailer
//
// miniz handles the RFC 1950/1951 streams natively; the gzip header/trailer
// (and its crc32/ISIZE bookkeeping) is implemented here around a raw stream.
//
// The JS side sees one hidden factory:
//   __brokit_compression.create(mode, format)  →  codec object with
//     .push(u8)   feed input, returns whatever output is ready (Uint8Array)
//     .finish()   flush + emit trailer / verify trailer, returns Uint8Array
//
// All data errors (corrupt stream, truncated stream, trailing garbage, bad
// gzip header/trailer) throw TypeError, matching Chrome's behavior of
// erroring the stream with a TypeError.

#include "api/api.h"
#include "runtime/runtime.h"
#include "compression.js.h"

#include "miniz.h"

#include <cstring>
#include <string>
#include <vector>

namespace brokit::api {

namespace {

enum class Format { Gzip, Zlib, Raw };

struct CodecState {
    bool compress = false;
    Format format = Format::Raw;

    mz_stream strm{};
    bool strmLive = false;   // mz_deflateInit2 / mz_inflateInit2 done
    bool finished = false;   // codec reached end-of-stream (inflate) / finish() ran
    bool closed = false;     // finish() completed — no further calls

    // gzip compress: crc32 + total size of the UNCOMPRESSED input (trailer).
    mz_ulong crcIn = MZ_CRC32_INIT;
    uint64_t totalIn = 0;
    bool wroteGzipHeader = false;

    // gzip decompress: buffered header bytes until a full header parses.
    std::vector<uint8_t> hdr;
    bool headerDone = false;
    // gzip decompress: crc32 + total size of the decompressed OUTPUT, checked
    // against the 8-byte trailer collected after the deflate stream ends.
    mz_ulong crcOut = MZ_CRC32_INIT;
    uint64_t totalOut = 0;
    std::vector<uint8_t> trailer;

    ~CodecState()
    {
        if (strmLive) {
            if (compress) mz_deflateEnd(&strm);
            else mz_inflateEnd(&strm);
        }
    }
};

// thread_local: each thread (main + each worker) has its own JSRuntime, so
// each must allocate its own class ID from that runtime's counter (same
// pattern as blob.cpp).
thread_local JSClassID codecClassId = 0;

void codecFinalizer(JSRuntime*, JSValue val)
{
    delete static_cast<CodecState*>(JS_GetOpaque(val, codecClassId));
}

JSClassDef codecClassDef = {
    "CompressionCodec",
    codecFinalizer,
    nullptr, nullptr, nullptr
};

CodecState* getCodec(JSContext* ctx, JSValueConst thisVal)
{
    auto* st = static_cast<CodecState*>(JS_GetOpaque(thisVal, codecClassId));
    if (!st) JS_ThrowTypeError(ctx, "not a compression codec");
    return st;
}

bool initDeflate(CodecState* st)
{
    int windowBits = (st->format == Format::Zlib) ? MZ_DEFAULT_WINDOW_BITS
                                                  : -MZ_DEFAULT_WINDOW_BITS;
    int rc = mz_deflateInit2(&st->strm, MZ_DEFAULT_COMPRESSION, MZ_DEFLATED,
                             windowBits, 9, MZ_DEFAULT_STRATEGY);
    st->strmLive = (rc == MZ_OK);
    return st->strmLive;
}

bool initInflate(CodecState* st)
{
    int windowBits = (st->format == Format::Zlib) ? MZ_DEFAULT_WINDOW_BITS
                                                  : -MZ_DEFAULT_WINDOW_BITS;
    int rc = mz_inflateInit2(&st->strm, windowBits);
    st->strmLive = (rc == MZ_OK);
    return st->strmLive;
}

void put32le(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}

// 10-byte fixed gzip header: magic, CM=deflate, no flags, mtime 0, XFL 0,
// OS 255 (unknown).
void writeGzipHeader(std::vector<uint8_t>& out)
{
    static const uint8_t hdr[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff };
    out.insert(out.end(), hdr, hdr + 10);
}

// Try to parse a complete gzip header from st->hdr. Returns the number of
// header bytes consumed, 0 if more input is needed, or -1 on a malformed
// header (bad magic / compression method / reserved flags).
long parseGzipHeader(const std::vector<uint8_t>& h)
{
    if (h.size() < 10) return 0;
    if (h[0] != 0x1f || h[1] != 0x8b) return -1;
    if (h[2] != 8) return -1; // CM must be deflate
    uint8_t flg = h[3];
    if (flg & 0xe0) return -1; // reserved flag bits must be zero
    size_t pos = 10;
    if (flg & 0x04) { // FEXTRA: 2-byte little-endian length + payload
        if (h.size() < pos + 2) return 0;
        size_t xlen = h[pos] | (static_cast<size_t>(h[pos + 1]) << 8);
        pos += 2;
        if (h.size() < pos + xlen) return 0;
        pos += xlen;
    }
    if (flg & 0x08) { // FNAME: NUL-terminated
        while (pos < h.size() && h[pos] != 0) pos++;
        if (pos >= h.size()) return 0;
        pos++;
    }
    if (flg & 0x10) { // FCOMMENT: NUL-terminated
        while (pos < h.size() && h[pos] != 0) pos++;
        if (pos >= h.size()) return 0;
        pos++;
    }
    if (flg & 0x02) { // FHCRC: 2-byte header crc (not verified — Chrome skips it too)
        if (h.size() < pos + 2) return 0;
        pos += 2;
    }
    return static_cast<long>(pos);
}

// Run mz_deflate/mz_inflate over [data, data+len), appending all produced
// output to `out`. Returns MZ_OK / MZ_STREAM_END on success, else an error.
// On MZ_STREAM_END, *remaining reports how many input bytes were NOT consumed.
int pump(CodecState* st, const uint8_t* data, size_t len, int flushMode,
         std::vector<uint8_t>& out, size_t* remaining)
{
    uint8_t buf[65536];
    st->strm.next_in = data;
    st->strm.avail_in = static_cast<unsigned int>(len);
    for (;;) {
        st->strm.next_out = buf;
        st->strm.avail_out = sizeof(buf);
        int rc = st->compress ? mz_deflate(&st->strm, flushMode)
                              : mz_inflate(&st->strm, flushMode);
        size_t produced = sizeof(buf) - st->strm.avail_out;
        if (produced) out.insert(out.end(), buf, buf + produced);
        if (rc == MZ_STREAM_END) {
            if (remaining) *remaining = st->strm.avail_in;
            return MZ_STREAM_END;
        }
        if (rc == MZ_BUF_ERROR) {
            // No progress possible — needs more input (or, under MZ_FINISH
            // with pending output, another spin, which `produced` covers).
            if (produced == 0) { if (remaining) *remaining = st->strm.avail_in; return MZ_OK; }
            continue;
        }
        if (rc != MZ_OK) return rc;
        if (st->strm.avail_in == 0 && st->strm.avail_out != 0 && flushMode != MZ_FINISH) {
            if (remaining) *remaining = 0;
            return MZ_OK;
        }
        // Otherwise keep looping: either input remains or output filled the
        // buffer exactly (or MZ_FINISH still draining).
    }
}

JSValue makeU8(JSContext* ctx, const std::vector<uint8_t>& bytes)
{
    JSValue ab = JS_NewArrayBufferCopy(ctx, bytes.data(), bytes.size());
    if (JS_IsException(ab)) return ab;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
    JS_FreeValue(ctx, global);
    JSValue u8 = JS_CallConstructor(ctx, ctor, 1, &ab);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, ab);
    return u8;
}

// Decompress `data` (post-gzip-header for gzip). Handles end-of-stream +
// trailer/trailing-garbage policing. Returns false with a pending JS
// exception on error.
bool decompressChunk(JSContext* ctx, CodecState* st, const uint8_t* data,
                     size_t len, std::vector<uint8_t>& out)
{
    if (st->finished) {
        // Deflate stream already ended. gzip still collects its 8 trailer
        // bytes; anything beyond that (any format) is trailing garbage.
        if (st->format == Format::Gzip && st->trailer.size() < 8) {
            size_t need = 8 - st->trailer.size();
            size_t take = len < need ? len : need;
            st->trailer.insert(st->trailer.end(), data, data + take);
            data += take;
            len -= take;
        }
        if (len > 0) {
            JS_ThrowTypeError(ctx, "junk found after end of compressed data");
            return false;
        }
        return true;
    }

    size_t before = out.size();
    size_t remaining = 0;
    int rc = pump(st, data, len, MZ_NO_FLUSH, out, &remaining);
    if (rc != MZ_OK && rc != MZ_STREAM_END) {
        JS_ThrowTypeError(ctx, "invalid compressed data (%s)", mz_error(rc));
        return false;
    }
    if (st->format == Format::Gzip) {
        size_t produced = out.size() - before;
        if (produced) {
            st->crcOut = mz_crc32(st->crcOut, out.data() + before, produced);
            st->totalOut += produced;
        }
    }
    if (rc == MZ_STREAM_END) {
        st->finished = true;
        if (remaining > 0) {
            // Recurse once into the finished-path to consume the gzip
            // trailer / detect trailing garbage.
            return decompressChunk(ctx, st, data + (len - remaining), remaining, out);
        }
    }
    return true;
}

JSValue js_codec_push(JSContext* ctx, JSValueConst thisVal, int argc,
                      JSValueConst* argv)
{
    CodecState* st = getCodec(ctx, thisVal);
    if (!st) return JS_EXCEPTION;
    if (st->closed) return JS_ThrowTypeError(ctx, "codec is finished");
    if (argc < 1) return JS_ThrowTypeError(ctx, "push: expected a Uint8Array");

    size_t byteOffset = 0, byteLen = 0, bpe = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOffset, &byteLen, &bpe);
    if (JS_IsException(ab)) return ab;
    size_t abLen = 0;
    uint8_t* abPtr = JS_GetArrayBuffer(ctx, &abLen, ab);
    JS_FreeValue(ctx, ab); // argv[0] keeps the buffer alive for this call
    if (!abPtr && byteLen > 0)
        return JS_ThrowTypeError(ctx, "push: detached buffer");
    const uint8_t* data = abPtr + byteOffset;

    std::vector<uint8_t> out;

    if (st->compress) {
        if (!st->strmLive && !initDeflate(st))
            return JS_ThrowTypeError(ctx, "deflate init failed");
        if (st->format == Format::Gzip) {
            if (!st->wroteGzipHeader) {
                writeGzipHeader(out);
                st->wroteGzipHeader = true;
            }
            if (byteLen) st->crcIn = mz_crc32(st->crcIn, data, byteLen);
            st->totalIn += byteLen;
        }
        int rc = pump(st, data, byteLen, MZ_NO_FLUSH, out, nullptr);
        if (rc != MZ_OK && rc != MZ_STREAM_END)
            return JS_ThrowTypeError(ctx, "deflate failed (%s)", mz_error(rc));
    } else {
        size_t len = byteLen;
        if (st->format == Format::Gzip && !st->headerDone) {
            st->hdr.insert(st->hdr.end(), data, data + len);
            long used = parseGzipHeader(st->hdr);
            if (used < 0)
                return JS_ThrowTypeError(ctx, "invalid gzip header");
            if (used == 0)
                return makeU8(ctx, out); // need more header bytes
            st->headerDone = true;
            if (!initInflate(st))
                return JS_ThrowTypeError(ctx, "inflate init failed");
            // Feed what followed the header out of the buffered bytes.
            std::vector<uint8_t> rest(st->hdr.begin() + used, st->hdr.end());
            st->hdr.clear();
            st->hdr.shrink_to_fit();
            if (!rest.empty() &&
                !decompressChunk(ctx, st, rest.data(), rest.size(), out))
                return JS_EXCEPTION;
            return makeU8(ctx, out);
        }
        if (!st->strmLive && !initInflate(st))
            return JS_ThrowTypeError(ctx, "inflate init failed");
        if (!decompressChunk(ctx, st, data, len, out))
            return JS_EXCEPTION;
    }

    return makeU8(ctx, out);
}

JSValue js_codec_finish(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*)
{
    CodecState* st = getCodec(ctx, thisVal);
    if (!st) return JS_EXCEPTION;
    if (st->closed) return JS_ThrowTypeError(ctx, "codec is finished");
    st->closed = true;

    std::vector<uint8_t> out;

    if (st->compress) {
        if (!st->strmLive && !initDeflate(st))
            return JS_ThrowTypeError(ctx, "deflate init failed");
        if (st->format == Format::Gzip && !st->wroteGzipHeader) {
            writeGzipHeader(out);
            st->wroteGzipHeader = true;
        }
        int rc = pump(st, nullptr, 0, MZ_FINISH, out, nullptr);
        if (rc != MZ_STREAM_END)
            return JS_ThrowTypeError(ctx, "deflate flush failed (%s)", mz_error(rc));
        st->finished = true;
        if (st->format == Format::Gzip) {
            put32le(out, static_cast<uint32_t>(st->crcIn));
            put32le(out, static_cast<uint32_t>(st->totalIn & 0xffffffffu));
        }
        return makeU8(ctx, out);
    }

    // Decompression: closing before the compressed stream (and, for gzip,
    // its full 8-byte trailer) has been seen is a truncation error.
    if (!st->finished)
        return JS_ThrowTypeError(ctx, "compressed data was truncated");
    if (st->format == Format::Gzip) {
        if (st->trailer.size() < 8)
            return JS_ThrowTypeError(ctx, "gzip trailer was truncated");
        const uint8_t* t = st->trailer.data();
        uint32_t crc = static_cast<uint32_t>(t[0]) | (static_cast<uint32_t>(t[1]) << 8) |
                       (static_cast<uint32_t>(t[2]) << 16) | (static_cast<uint32_t>(t[3]) << 24);
        uint32_t isize = static_cast<uint32_t>(t[4]) | (static_cast<uint32_t>(t[5]) << 8) |
                         (static_cast<uint32_t>(t[6]) << 16) | (static_cast<uint32_t>(t[7]) << 24);
        if (crc != static_cast<uint32_t>(st->crcOut))
            return JS_ThrowTypeError(ctx, "gzip crc32 check failed");
        if (isize != static_cast<uint32_t>(st->totalOut & 0xffffffffu))
            return JS_ThrowTypeError(ctx, "gzip size check failed");
    }
    return makeU8(ctx, out);
}

// __brokit_compression.create(mode, format)
JSValue js_codec_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "create(mode, format) expects 2 arguments");
    const char* modeStr = JS_ToCString(ctx, argv[0]);
    if (!modeStr) return JS_EXCEPTION;
    std::string mode(modeStr);
    JS_FreeCString(ctx, modeStr);
    const char* fmtStr = JS_ToCString(ctx, argv[1]);
    if (!fmtStr) return JS_EXCEPTION;
    std::string fmt(fmtStr);
    JS_FreeCString(ctx, fmtStr);

    bool compress = (mode == "compress");
    if (!compress && mode != "decompress")
        return JS_ThrowTypeError(ctx, "unknown codec mode '%s'", mode.c_str());

    Format format;
    if (fmt == "gzip") format = Format::Gzip;
    else if (fmt == "deflate") format = Format::Zlib;
    else if (fmt == "deflate-raw") format = Format::Raw;
    else return JS_ThrowTypeError(ctx, "Unsupported compression format: '%s'", fmt.c_str());

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(codecClassId));
    if (JS_IsException(obj)) return obj;
    auto* st = new CodecState();
    st->compress = compress;
    st->format = format;
    JS_SetOpaque(obj, st);
    JS_SetPropertyStr(ctx, obj, "push",
        JS_NewCFunction(ctx, js_codec_push, "push", 1));
    JS_SetPropertyStr(ctx, obj, "finish",
        JS_NewCFunction(ctx, js_codec_finish, "finish", 0));
    return obj;
}

} // namespace

void installCompression(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (codecClassId == 0) JS_NewClassID(rt, &codecClassId);
    if (!JS_IsRegisteredClass(rt, codecClassId)) {
        JS_NewClass(rt, codecClassId, &codecClassDef);
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ns = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ns, "create",
        JS_NewCFunction(ctx, js_codec_create, "create", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_compression", ns);
    JS_FreeValue(ctx, global);

    // JS layer: CompressionStream / DecompressionStream over TransformStream.
    JSValue r = JS_Eval(ctx, js_compression, strlen(js_compression),
                        "<compression>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        Runtime::checkException(ctx, r);
    }
    JS_FreeValue(ctx, r);
}

} // namespace brokit::api
