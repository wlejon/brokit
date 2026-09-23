// CompressionStream / DecompressionStream — native codec layer.

#include "api/api.h"
#include "api/arg_reader.h"
#include "api/host_class.h"
#include "api/object_builder.h"

#include "miniz.h"

#include <cstring>
#include <string>
#include <vector>

extern "C" void bronze_compression_main();

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

static HostClass g_codecClass;

static void codecDtor(void* p)
{
    delete static_cast<CodecState*>(p);
}

static CodecState* getCodec(bronze::Value thisVal)
{
    auto* st = static_cast<CodecState*>(g_codecClass.unwrap(thisVal));
    return st;
}

static bool initDeflate(CodecState* st)
{
    int windowBits = (st->format == Format::Zlib) ? MZ_DEFAULT_WINDOW_BITS
                                                  : -MZ_DEFAULT_WINDOW_BITS;
    int rc = mz_deflateInit2(&st->strm, MZ_DEFAULT_COMPRESSION, MZ_DEFLATED,
                             windowBits, 9, MZ_DEFAULT_STRATEGY);
    st->strmLive = (rc == MZ_OK);
    return st->strmLive;
}

static bool initInflate(CodecState* st)
{
    int windowBits = (st->format == Format::Zlib) ? MZ_DEFAULT_WINDOW_BITS
                                                  : -MZ_DEFAULT_WINDOW_BITS;
    int rc = mz_inflateInit2(&st->strm, windowBits);
    st->strmLive = (rc == MZ_OK);
    return st->strmLive;
}

static bronze::Value makeU8(const std::vector<uint8_t>& buf)
{
    bronze::Value v = ev::createTypedArray(elements::Uint8, static_cast<uint32_t>(buf.size()));
    ev::fillTypedArray(v, buf);
    return v;
}

static bool pumpDeflate(CodecState* st, int flush, std::vector<uint8_t>& out)
{
    uint8_t buf[16384];
    for (;;) {
        st->strm.next_out = buf;
        st->strm.avail_out = sizeof(buf);
        int rc = mz_deflate(&st->strm, flush);
        size_t written = sizeof(buf) - st->strm.avail_out;
        if (written > 0) {
            out.insert(out.end(), buf, buf + written);
        }
        if (rc == MZ_STREAM_END) return true;
        if (rc != MZ_OK && rc != MZ_BUF_ERROR) return false;
        if (st->strm.avail_out > 0) break;
    }
    return true;
}

static bool pumpInflate(CodecState* st, std::vector<uint8_t>& out, bool& streamEnd)
{
    uint8_t buf[16384];
    streamEnd = false;
    for (;;) {
        st->strm.next_out = buf;
        st->strm.avail_out = sizeof(buf);
        int rc = mz_inflate(&st->strm, MZ_NO_FLUSH);
        size_t written = sizeof(buf) - st->strm.avail_out;
        if (written > 0) {
            out.insert(out.end(), buf, buf + written);
            st->crcOut = mz_crc32(st->crcOut, buf, written);
            st->totalOut += written;
        }
        if (rc == MZ_STREAM_END) {
            streamEnd = true;
            return true;
        }
        if (rc != MZ_OK && rc != MZ_BUF_ERROR) return false;
        if (st->strm.avail_out > 0) break;
    }
    return true;
}

static size_t parseGzipHeader(const uint8_t* p, size_t len)
{
    if (len < 10) return 0;
    if (p[0] != 0x1f || p[1] != 0x8b) return 0;
    if (p[2] != 8) return 0; // CM_DEFLATE
    uint8_t flg = p[3];
    size_t off = 10;

    if (flg & 0x04) { // FEXTRA
        if (off + 2 > len) return 0;
        uint16_t xlen = static_cast<uint16_t>(p[off]) |
                        (static_cast<uint16_t>(p[off + 1]) << 8);
        off += 2 + xlen;
        if (off > len) return 0;
    }
    if (flg & 0x08) { // FNAME
        while (off < len && p[off] != 0) off++;
        if (off >= len) return 0;
        off++;
    }
    if (flg & 0x10) { // FCOMMENT
        while (off < len && p[off] != 0) off++;
        if (off >= len) return 0;
        off++;
    }
    if (flg & 0x02) { // FHCRC
        if (off + 2 > len) return 0;
        off += 2;
    }
    return off;
}

static bronze::Value codecPush(bronze::Value thisVal, std::span<const bronze::Value> a)
{
    auto* st = getCodec(thisVal);
    if (!st) return ev::throwTypeError("not a compression codec");
    if (st->closed) return ev::throwTypeError("codec is closed");

    if (a.empty()) return ev::throwTypeError("push() expects a chunk");
    // The "_u8" read may allocate (property-key interning), so the chunk is
    // re-read from the rooted args span rather than held in a local copy.
    bronze::Value chunk = a[0];
    if (!ev::typedArrayInfo(a[0]) && ev::isObject(a[0])) {
        bronze::Value u8 = ev::getProperty(a[0], "_u8");
        chunk = ev::isObject(u8) ? u8 : a[0];
    }
    auto info = ev::typedArrayInfo(chunk);
    if (!info) return ev::throwTypeError("push() expects a TypedArray");

    const uint8_t* inPtr = info.data;
    size_t inLen = info.byteLength;

    std::vector<uint8_t> out;

    if (st->compress) {
        if (!st->strmLive && !initDeflate(st))
            return ev::throwTypeError("failed to initialize compressor");

        if (st->format == Format::Gzip && !st->wroteGzipHeader) {
            static const uint8_t kGzHdr[10] = {
                0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03
            };
            out.insert(out.end(), kGzHdr, kGzHdr + sizeof(kGzHdr));
            st->wroteGzipHeader = true;
        }

        if (inLen > 0) {
            st->crcIn = mz_crc32(st->crcIn, inPtr, inLen);
            st->totalIn += inLen;
            st->strm.next_in = inPtr;
            st->strm.avail_in = static_cast<unsigned int>(inLen);
            if (!pumpDeflate(st, MZ_NO_FLUSH, out))
                return ev::throwTypeError("deflate failed");
        }
    } else {
        if (st->finished) {
            if (st->format == Format::Gzip) {
                st->trailer.insert(st->trailer.end(), inPtr, inPtr + inLen);
                return makeU8(out);
            }
            if (inLen > 0)
                return ev::throwTypeError("unexpected data after end of stream");
            return makeU8(out);
        }

        if (st->format == Format::Gzip && !st->headerDone) {
            st->hdr.insert(st->hdr.end(), inPtr, inPtr + inLen);
            size_t hdrSize = parseGzipHeader(st->hdr.data(), st->hdr.size());
            if (hdrSize == 0) {
                if (st->hdr.size() >= 2 && (st->hdr[0] != 0x1f || st->hdr[1] != 0x8b))
                    return ev::throwTypeError("not a gzip stream (bad magic)");
                return makeU8(out);
            }
            st->headerDone = true;
            if (!st->strmLive && !initInflate(st))
                return ev::throwTypeError("failed to initialize decompressor");

            size_t remainder = st->hdr.size() - hdrSize;
            if (remainder > 0) {
                st->strm.next_in = st->hdr.data() + hdrSize;
                st->strm.avail_in = static_cast<unsigned int>(remainder);
                bool end = false;
                if (!pumpInflate(st, out, end))
                    return ev::throwTypeError("inflate failed");
                if (end) {
                    st->finished = true;
                    if (st->strm.avail_in > 0) {
                        st->trailer.insert(st->trailer.end(),
                                           st->strm.next_in,
                                           st->strm.next_in + st->strm.avail_in);
                    }
                }
            }
            return makeU8(out);
        }

        if (!st->strmLive && !initInflate(st))
            return ev::throwTypeError("failed to initialize decompressor");

        if (inLen > 0) {
            st->strm.next_in = inPtr;
            st->strm.avail_in = static_cast<unsigned int>(inLen);
            bool end = false;
            if (!pumpInflate(st, out, end))
                return ev::throwTypeError("inflate failed");
            if (end) {
                st->finished = true;
                if (st->strm.avail_in > 0) {
                    if (st->format == Format::Gzip) {
                        st->trailer.insert(st->trailer.end(),
                                           st->strm.next_in,
                                           st->strm.next_in + st->strm.avail_in);
                    } else {
                        return ev::throwTypeError("unexpected trailing bytes after stream");
                    }
                }
            }
        }
    }

    return makeU8(out);
}

static bronze::Value codecFinish(bronze::Value thisVal, std::span<const bronze::Value>)
{
    auto* st = getCodec(thisVal);
    if (!st) return ev::throwTypeError("not a compression codec");
    if (st->closed) return ev::throwTypeError("codec is already closed");
    st->closed = true;

    std::vector<uint8_t> out;

    if (st->compress) {
        if (!st->strmLive && !initDeflate(st))
            return ev::throwTypeError("failed to initialize compressor");

        if (st->format == Format::Gzip && !st->wroteGzipHeader) {
            static const uint8_t kGzHdr[10] = {
                0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03
            };
            out.insert(out.end(), kGzHdr, kGzHdr + sizeof(kGzHdr));
            st->wroteGzipHeader = true;
        }

        st->strm.next_in = nullptr;
        st->strm.avail_in = 0;
        if (!pumpDeflate(st, MZ_FINISH, out))
            return ev::throwTypeError("deflate finish failed");

        if (st->format == Format::Gzip) {
            uint32_t crc = static_cast<uint32_t>(st->crcIn);
            uint32_t isize = static_cast<uint32_t>(st->totalIn & 0xffffffffu);
            uint8_t tr[8] = {
                static_cast<uint8_t>(crc & 0xff),
                static_cast<uint8_t>((crc >> 8) & 0xff),
                static_cast<uint8_t>((crc >> 16) & 0xff),
                static_cast<uint8_t>((crc >> 24) & 0xff),
                static_cast<uint8_t>(isize & 0xff),
                static_cast<uint8_t>((isize >> 8) & 0xff),
                static_cast<uint8_t>((isize >> 16) & 0xff),
                static_cast<uint8_t>((isize >> 24) & 0xff),
            };
            out.insert(out.end(), tr, tr + sizeof(tr));
        }
        return makeU8(out);
    }

    if (!st->finished)
        return ev::throwTypeError("compressed data was truncated");
    if (st->format == Format::Gzip) {
        if (st->trailer.size() < 8)
            return ev::throwTypeError("gzip trailer was truncated");
        if (st->trailer.size() > 8)
            return ev::throwTypeError("unexpected trailing bytes after gzip stream");
        const uint8_t* t = st->trailer.data();
        uint32_t crc = static_cast<uint32_t>(t[0]) | (static_cast<uint32_t>(t[1]) << 8) |
                       (static_cast<uint32_t>(t[2]) << 16) | (static_cast<uint32_t>(t[3]) << 24);
        uint32_t isize = static_cast<uint32_t>(t[4]) | (static_cast<uint32_t>(t[5]) << 8) |
                         (static_cast<uint32_t>(t[6]) << 16) | (static_cast<uint32_t>(t[7]) << 24);
        if (crc != static_cast<uint32_t>(st->crcOut))
            return ev::throwTypeError("gzip crc32 check failed");
        if (isize != static_cast<uint32_t>(st->totalOut & 0xffffffffu))
            return ev::throwTypeError("gzip size check failed");
    }
    return makeU8(out);
}

static bronze::Value codecCreate(bronze::Value, std::span<const bronze::Value> a)
{
    if (a.size() < 2)
        return ev::throwTypeError("create(mode, format) expects 2 arguments");

    std::string mode = ev::toUtf8(a[0]);
    std::string fmt = ev::toUtf8(a[1]);

    bool compress = (mode == "compress");
    if (!compress && mode != "decompress")
        return ev::throwTypeError(("unknown codec mode '" + mode + "'").c_str());

    Format format;
    if (fmt == "gzip") format = Format::Gzip;
    else if (fmt == "deflate") format = Format::Zlib;
    else if (fmt == "deflate-raw") format = Format::Raw;
    else return ev::throwTypeError(("Unsupported compression format: '" + fmt + "'").c_str());

    auto* st = new CodecState();
    st->compress = compress;
    st->format = format;

    bronze::Value obj = g_codecClass.make(st, codecDtor);
    return obj;
}

} // namespace

void installCompression()
{
    g_codecClass.install("__BrokitCodec", 0, nullptr, [](ObjectBuilder& proto) {
        proto.def("push", 1, codecPush);
        proto.def("finish", 0, codecFinish);
    });

    ObjectBuilder ns;
    ns.def("create", 2, codecCreate);
    ev::setGlobalValue("__brokit_compression", ns.get());

    bronze::embed::runEntry(bronze_compression_main);
}

} // namespace brokit::api
