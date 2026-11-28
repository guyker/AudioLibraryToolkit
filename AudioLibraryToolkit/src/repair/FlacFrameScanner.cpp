#include "RepairIntegrity.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace RepairIntegrity
{
namespace
{
using U64 = std::uint64_t;
using Byte = std::uint8_t;
constexpr U64 MaxSamples = (U64{1} << 36) - 1;

struct Problem
{
    Issue issue;
};

[[noreturn]] void stop(std::string rule, std::string detail, U64 at, U64 length = 0,
    std::string expected = {}, std::string observed = {})
{
    throw Problem{{std::move(rule), std::move(detail), at, length, std::move(expected),
        std::move(observed), false, {}}};
}

[[noreturn]] void unsupported(std::string reason, U64 at)
{
    stop("flac.unsupported-stream", std::move(reason), at);
}

std::string hex(U64 value, unsigned digits)
{
    std::string result(digits, '0');
    while (digits)
    {
        result[--digits] = "0123456789abcdef"[value & 15];
        value >>= 4;
    }
    return result;
}

// Keep a fixed input window, independent of file size or any declared metadata/frame size.
class Input
{
    std::ifstream _file;
    struct Window
    {
        std::array<Byte, 65536> data;
        U64 base = 0;
        size_t valid = 0;
        bool contains(U64 pos) const { return pos >= base && pos - base < valid; }
    };
    // Audio and SEEKTABLE advance independently; don't reread 64 KiB for each seek point.
    std::array<Window, 2> _windows;
    unsigned _active = 0;
public:
    U64 size;
    explicit Input(const std::filesystem::path& path) : _file(path, std::ios::binary | std::ios::ate)
    {
        const auto length = _file.tellg();
        if (length < 0) throw std::runtime_error("Cannot open or measure FLAC input");
        size = static_cast<U64>(length);
    }
    Byte at(U64 pos)
    {
        if (pos >= size) unsupported("Read outside FLAC input", size);
        if (!_windows[_active].contains(pos))
        {
            _active ^= 1;
            auto& w = _windows[_active];
            if (!w.contains(pos))
            {
                w.base = pos - pos % w.data.size();
                w.valid = static_cast<size_t>(std::min<U64>(w.data.size(), size - w.base));
                _file.seekg(static_cast<std::streamoff>(w.base));
                if (!_file.read(reinterpret_cast<char*>(w.data.data()), w.valid))
                    throw std::runtime_error("Cannot read complete FLAC input window");
            }
        }
        const auto& w = _windows[_active];
        return w.data[static_cast<size_t>(pos - w.base)];
    }
    bool match(U64 pos, std::string_view text)
    {
        if (pos > size || text.size() > size - pos) return false;
        for (char c : text) if (at(pos++) != static_cast<Byte>(c)) return false;
        return true;
    }
    U64 be(U64 pos, unsigned n)
    {
        U64 value = 0;
        while (n--) value = (value << 8) | at(pos++);
        return value;
    }
    bool zeros(U64 pos, U64 end)
    {
        while (pos < end) if (at(pos++)) return false;
        return true;
    }
};

template<unsigned Width, unsigned Poly>
constexpr auto crcTable()
{
    std::array<unsigned, 256> table{};
    for (unsigned i = 0; i < table.size(); ++i)
    {
        unsigned value = i << (Width - 8);
        for (int bit = 0; bit < 8; ++bit)
            value = (value << 1) ^ ((value & (1u << (Width - 1))) ? Poly : 0);
        table[i] = value & ((1u << Width) - 1);
    }
    return table;
}
constexpr auto Crc8 = crcTable<8, 0x07>();
constexpr auto Crc16 = crcTable<16, 0x8005>();

class FrameReader
{
    Input& _input;
    U64 _start, _end, _limit;
    unsigned _bits = 0, _byte = 0;
public:
    U64 pos;
    unsigned crc8 = 0, crc16 = 0;
    FrameReader(Input& input, U64 start, U64 end, U64 limit) :
        _input(input), _start(start), _end(end), _limit(limit), pos(start) {}
    unsigned byte()
    {
        if (pos == _end)
            stop("flac.truncated-frame", "EOF before the complete FLAC frame and footer", _start, pos - _start);
        // A pathological unary code must not consume unbounded work on a single purported frame.
        if (pos - _start >= _limit) unsupported("Frame exceeds structural inspection limit", _start);
        const unsigned b = _input.at(pos++);
        crc16 = ((crc16 << 8) & 0xffff) ^ Crc16[(crc16 >> 8) ^ b];
        return b;
    }
    unsigned header()
    {
        unsigned b = byte();
        crc8 = Crc8[crc8 ^ b];
        return b;
    }
    unsigned bits(unsigned n)
    {
        unsigned value = 0;
        while (n)
        {
            if (!_bits) { _byte = byte(); _bits = 8; }
            const unsigned take = std::min(n, _bits);
            _bits -= take;
            n -= take;
            value = (value << take) | ((_byte >> _bits) & ((1u << take) - 1));
        }
        return value;
    }
    void skip(unsigned n)
    {
        const unsigned first = std::min(_bits, n);
        _bits -= first;
        n -= first;
        while (n >= 8) { byte(); n -= 8; }
        bits(n);
    }
    unsigned unary(unsigned limit)
    {
        unsigned count = 0;
        for (;;)
        {
            if (!_bits) { _byte = byte(); _bits = 8; }
            const auto part = static_cast<Byte>(_byte << (8 - _bits));
            const unsigned zeros = std::min<unsigned>(std::countl_zero(part), _bits);
            if (zeros > limit - count) unsupported("Unary value outside FLAC coding range", pos - 1);
            count += zeros;
            _bits -= zeros;
            if (_bits) { --_bits; return count; }
        }
    }
    void align()
    {
        if (bits(_bits)) unsupported("Nonzero FLAC frame alignment padding", pos - 1);
    }
};

struct Format { unsigned rate, channels, bits; };
struct Frame { unsigned block, crc, headerCrc; bool variable; U64 number, numberEnd, headerCrcAt, end; };

U64 codedNumber(FrameReader& r, bool variable)
{
    const Byte lead = static_cast<Byte>(r.header());
    const auto bytes = static_cast<unsigned>(std::countl_one(lead));
    if (bytes == 0) return lead;
    const U64 start = r.pos - 1;
    if (bytes == 1 || bytes > (variable ? 7u : 6u)) unsupported("Invalid FLAC coded number", start);
    U64 value = lead & (0x7fu >> bytes);
    for (unsigned i = 1; i < bytes; ++i)
    {
        const unsigned b = r.header();
        if ((b & 0xc0) != 0x80) unsupported("Invalid coded-number continuation", r.pos - 1);
        value = (value << 6) | (b & 63);
    }
    if (value < (U64{1} << (bytes == 2 ? 7 : 5 * bytes - 4)))
        unsupported("Overlong FLAC coded number", start);
    return value;
}

void residual(FrameReader& r, unsigned block, unsigned order)
{
    const unsigned method = r.bits(2);
    if (method > 1) unsupported("Reserved residual coding method", r.pos - 1);
    const unsigned partitions = 1u << r.bits(4);
    const unsigned per = block / partitions;
    if (block % partitions || per <= order) unsupported("Invalid residual partition dimensions", r.pos - 1);
    const unsigned paramBits = 4 + method;
    for (unsigned i = 0; i < partitions; ++i)
    {
        const unsigned count = per - (i == 0 ? order : 0);
        const unsigned param = r.bits(paramBits);
        if (param == (1u << paramBits) - 1)
            r.skip(count * r.bits(5));
        else
            for (unsigned j = 0; j < count; ++j)
            {
                constexpr unsigned maxFolded = 0xfffffffeu;
                const unsigned high = r.unary(maxFolded >> param);
                const unsigned low = r.bits(param);
                if (((high << param) | low) > maxFolded) unsupported("Residual exceeds 31-bit magnitude", r.pos - 1);
            }
    }
}

void subframe(FrameReader& r, unsigned block, unsigned width)
{
    if (r.bits(1)) unsupported("Invalid subframe header padding", r.pos - 1);
    const unsigned type = r.bits(6);
    if (r.bits(1))
    {
        if (width < 2) unsupported("No significant bits remain", r.pos - 1);
        width -= 1 + r.unary(width - 2);
    }
    if (type <= 1) { r.skip((type ? block : 1) * width); return; }
    if ((type < 8 || type > 12) && type < 32) unsupported("Reserved FLAC subframe type", r.pos - 1);
    const unsigned order = type < 32 ? type - 8 : type - 31;
    if (order >= block) unsupported("Predictor order too large for block", r.pos - 1);
    r.skip(order * width);
    if (type >= 32)
    {
        const auto precision = r.bits(4) + 1;
        if (precision == 16 || r.bits(5) >= 16) unsupported("Invalid LPC precision or shift", r.pos - 1);
        r.skip(order * precision);
    }
    residual(r, block, order);
}

Frame readFrame(Input& input, U64 pos, const Format& fmt, U64 end, U64 limit = 32u << 20)
{
    FrameReader r(input, pos, end, limit);
    const unsigned first = r.header(), second = r.header();
    if (first != 0xff || (second & 0xfe) != 0xf8) stop("flac.lost-sync", "Invalid frame sync", pos, 2);
    const bool variable = (second & 1) != 0;
    const unsigned sizes = r.header(), encoding = r.header(), assignment = encoding >> 4;
    const unsigned bc = sizes >> 4, rc = sizes & 15, depthCode = (encoding >> 1) & 7;
    if (!bc || rc == 15 || depthCode == 3 || assignment > 10 || (encoding & 1))
        unsupported("Reserved FLAC frame header field", pos + 2);
    const U64 number = codedNumber(r, variable), numberEnd = r.pos;
    unsigned block = 0;
    if (bc == 1) block = 192;
    else if (bc <= 5) block = 144u << bc;
    else if (bc == 6) block = r.header() + 1;
    else if (bc == 7) { block = r.header() << 8; block += r.header() + 1; }
    else block = 1u << bc;
    if (block == 65536) unsupported("Forbidden block size 65536", pos + 2);
    constexpr unsigned rates[] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    unsigned rate;
    if (rc == 0) rate = fmt.rate;
    else if (rc < 12) rate = rates[rc];
    else if (rc == 12) rate = r.header() * 1000;
    else { rate = r.header() << 8; rate += r.header(); if (rc == 14) rate *= 10; }
    const U64 headerCrcAt = r.pos;
    const unsigned calculated = r.crc8, stored = r.byte();
    if (calculated != stored) stop("flac.header-crc", "FLAC header CRC-8 mismatch", r.pos - 1, 1,
        hex(calculated, 2), hex(stored, 2));
    constexpr unsigned widths[] = {0, 8, 12, 0, 16, 20, 24, 32};
    const unsigned bits = depthCode ? widths[depthCode] : fmt.bits;
    const unsigned channels = assignment <= 7 ? assignment + 1 : 2;
    if (rate != fmt.rate || bits != fmt.bits || channels != fmt.channels)
        unsupported("Frame sample interpretation differs from STREAMINFO", pos + 2);
    for (unsigned ch = 0; ch < channels; ++ch)
    {
        const bool side = (ch == 0 && assignment == 9) || (ch == 1 && (assignment == 8 || assignment == 10));
        subframe(r, block, bits + side);
    }
    r.align();
    const unsigned calculatedFrame = r.crc16;
    unsigned storedFrame = r.byte() << 8;
    storedFrame |= r.byte();
    if (calculatedFrame != storedFrame) stop("flac.frame-crc", "FLAC frame CRC-16 mismatch", r.pos - 2, 2,
        hex(calculatedFrame, 4), hex(storedFrame, 4));
    return {block, storedFrame, stored, variable, number, numberEnd, headerCrcAt, r.pos};
}

struct Metadata
{
    U64 info = 0, audio = 0, seek = 0, seekLength = 0;
    bool malformedSeek = false;
    Format format{};
};

std::optional<U64> synchsafe(Input& in, U64 pos)
{
    U64 size = 0;
    for (unsigned i = 0; i < 4; ++i)
    {
        const auto part = in.at(pos + i);
        if (part > 127) return {};
        size = (size << 7) | part;
    }
    return size;
}

bool id3Frames(Input& in, U64 pos, U64 end, unsigned version, bool footer)
{
    const unsigned id = version == 2 ? 3 : 4, header = version == 2 ? 6 : 10;
    for (unsigned count = 0; pos < end; ++count)
    {
        if (!in.at(pos)) return !footer && in.zeros(pos, end);
        if (count >= 65536 || end - pos < header) return false;
        for (unsigned i = 0; i < id; ++i)
        {
            const auto c = in.at(pos + i);
            if (!(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9')) return false;
        }
        // Offset-bearing and specially transformed tag frames are opaque here.
        // A byte-identical relocated tag alone would not prove its references.
        if (version != 2 && (in.be(pos + 8, 2) != 0 || in.match(pos, "CHAP") || in.match(pos, "CTOC") ||
            in.match(pos, "SEEK") || in.match(pos, "ASPI") || in.match(pos, "RBUF"))) return false;
        if (version == 2 && in.match(pos, "BUF")) return false;
        const std::optional<U64> length = version == 4 ? synchsafe(in, pos + id) : std::optional{in.be(pos + id, id)};
        if (!length || !*length || *length > end - pos - header) return false;
        pos += header + *length;
    }
    return true;
}

Metadata metadata(Input& in, Inspection& result)
{
    U64 pos = 0;
    for (unsigned count = 0; in.match(pos, "ID3"); ++count)
    {
        if (count >= 16 || in.size - pos < 10) unsupported("Incomplete or excessive leading ID3 tags", pos);
        const unsigned version = in.at(pos + 3), flags = in.at(pos + 5);
        if (version < 2 || version > 4 || in.at(pos + 4) || (flags & ~(version == 4 ? 16u : 0u)))
            unsupported("Unsupported leading ID3 header", pos);
        const auto payload = synchsafe(in, pos + 6);
        if (!payload) unsupported("Invalid leading ID3 size", pos + 6);
        U64 size = 10 + *payload;
        if (size > in.size - pos) unsupported("Leading ID3 tag extends past EOF", pos);
        if (!id3Frames(in, pos + 10, pos + size, version, flags != 0))
            unsupported("Unverified leading ID3 frame layout or references", pos);
        if (version == 4 && (flags & 16))
        {
            if (!in.match(pos + size, "3DI") || in.size - pos - size < 10 ||
                in.be(pos + 3, 7) != in.be(pos + size + 3, 7)) unsupported("Invalid leading ID3 footer", pos);
            size += 10;
        }
        pos += size;
    }
    if (!in.match(pos, "fLaC")) unsupported("No native FLAC marker at the established boundary", pos);
    pos += 4;
    Metadata m;
    for (unsigned count = 0;; ++count)
    {
        if (count >= 65536 || in.size - pos < 4) unsupported("Incomplete or excessive FLAC metadata blocks", pos);
        const auto header = in.at(pos);
        const unsigned type = header & 127;
        const U64 size = in.be(pos + 1, 3);
        pos += 4;
        if (size > in.size - pos || type >= 7) unsupported("Unsupported or incomplete FLAC metadata block", pos - 4);
        if ((count == 0) != (type == 0)) unsupported("First and only STREAMINFO is required", pos - 4);
        if (type == 0)
        {
            if (size != 34) unsupported("Invalid STREAMINFO length", pos - 4);
            m.info = pos;
            const U64 packed = in.be(pos + 10, 8);
            m.format = {static_cast<unsigned>(packed >> 44), static_cast<unsigned>((packed >> 41) & 7) + 1,
                static_cast<unsigned>((packed >> 36) & 31) + 1};
            if (!m.format.rate || m.format.bits < 4) unsupported("Invalid STREAMINFO audio interpretation", pos + 10);
            result.bits = static_cast<int>(m.format.bits);
            result.declaredSamples = packed & MaxSamples;
            if (!in.zeros(pos + 18, pos + 34))
                for (U64 i = pos + 18; i < pos + 34; ++i) result.md5 += hex(in.at(i), 2);
        }
        if (type == 3)
        {
            if (m.seek || size % 18)
            {
                if (!m.malformedSeek)
                    result.issues.push_back({"flac.seek-target", "Duplicate or malformed SEEKTABLE; targets cannot be verified",
                        pos - 4, size + 4, "one table of 18-byte records", "invalid table", false, {}});
                m.malformedSeek = true;
            }
            m.seek = pos;
            m.seekLength = size;
        }
        if ((type == 1 && !in.zeros(pos, pos + size)) || (type == 2 && size < 4))
            unsupported("Malformed FLAC padding or application block", pos - 4);
        pos += size;
        if (header & 128) break;
    }
    m.audio = pos;
    return m;
}

struct Walk
{
    Frame last{};
    U64 frames = 0, lastStart = 0;
    unsigned nominal = 0, minNonfinal = 65536, maxBlock = 0;
    U64 minFrame = std::numeric_limits<U64>::max(), maxFrame = 0;
};

std::string witnesses(const Walk& w, const Inspection& result)
{
    return "frames=" + std::to_string(w.frames) + "; samples/channel=" + std::to_string(result.samples)
        + "; last frame=[" + std::to_string(w.lastStart) + "," + std::to_string(w.last.end)
        + "); CRC-8@" + std::to_string(w.last.headerCrcAt) + "=0x" + hex(w.last.headerCrc, 2)
        + "; CRC-16@" + std::to_string(w.last.end - 2) + "=0x" + hex(w.last.crc, 4);
}

unsigned le32(Input& in, U64 pos)
{
    unsigned n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= unsigned(in.at(pos + i)) << (8 * i);
    return n;
}

std::optional<U64> decimal(Input& in, U64 pos, unsigned digits)
{
    U64 value = 0;
    while (digits--)
    {
        const auto c = in.at(pos++);
        if (c < '0' || c > '9') return {};
        value = value * 10 + c - '0';
    }
    return value;
}

// External metadata is never part of a fill edit. Determine its start backwards only
// after the forward walk established a frame end, not to decide where a frame ends.
std::optional<U64> trailerStart(Input& in, U64 afterFrame)
{
    U64 end = in.size;
    if (end >= 128 && in.match(end - 128, "TAG"))
    {
        if (end - 128 < afterFrame || in.match(end - 32, "APETAGEX") || in.match(end - 9, "LYRICS200") ||
            in.match(end - 10, "3DI"))
            return {};
        end -= 128;
    }
    if (end >= 10 && in.match(end - 10, "3DI"))
    {
        const auto size = synchsafe(in, end - 4);
        if (!size || *size + 20 > end - afterFrame || in.be(end - 7, 3) != 0x040010) return {};
        const U64 start = end - 20 - *size;
        if (!in.match(start, "ID3") || in.be(start + 3, 7) != in.be(end - 7, 7) ||
            !id3Frames(in, start + 10, end - 10, 4, true)) return {};
        end = start;
    }
    if (end >= 15 && in.match(end - 9, "LYRICS200"))
    {
        const auto size = decimal(in, end - 15, 6);
        if (!size || *size < 11 || end - afterFrame < *size + 15) return {};
        const U64 start = end - 15 - *size;
        if (!in.match(start, "LYRICSBEGIN")) return {};
        for (U64 p = start + 11; p < end - 15;)
        {
            if (end - 15 - p < 8) return {};
            for (unsigned i = 0; i < 3; ++i) if (in.at(p + i) < 'A' || in.at(p + i) > 'Z') return {};
            const auto field = decimal(in, p + 3, 5);
            if (!field || *field > end - 15 - p - 8) return {};
            p += 8 + *field;
        }
        end = start;
    }
    if (end >= 32 && in.match(end - 32, "APETAGEX"))
    {
        const U64 foot = end - 32, size = le32(in, foot + 12);
        const unsigned flags = le32(in, foot + 20), count = le32(in, foot + 16);
        const bool header = (flags & 0x80000000u) != 0;
        if (le32(in, foot + 8) != 2000 || (flags & 0x7fffffffu) || !in.zeros(foot + 24, end) ||
            size < 32 || size > (64u << 20) || size + (header ? 32 : 0) > end - afterFrame || count > 65536) return {};
        U64 start = end - size, pos = start;
        for (unsigned i = 0; i < count; ++i)
        {
            if (foot - pos < 11) return {};
            const U64 length = le32(in, pos);
            const unsigned itemFlags = le32(in, pos + 4);
            if (itemFlags > 7 || (itemFlags & 6) == 6) return {};
            pos += 8;
            U64 key = pos;
            while (pos < foot && in.at(pos))
            {
                if (pos - key >= 255 || in.at(pos) < 32 || in.at(pos) > 126 || in.at(pos) == '=') return {};
                ++pos;
            }
            if (pos == foot || pos - key < 2) return {};
            ++pos;
            if (length > foot - pos) return {};
            pos += length;
        }
        if (pos != foot) return {};
        if (header)
        {
            start -= 32;
            if (!in.match(start, "APETAGEX") || le32(in, start + 8) != 2000 || le32(in, start + 12) != size ||
                le32(in, start + 16) != count || le32(in, start + 20) != (flags | 0x20000000u) ||
                !in.zeros(start + 24, start + 32)) return {};
        }
        end = start;
    }
    return end;
}

Frame physicalFrame(Input& in, U64 pos, const Format& format)
{
    try { return readFrame(in, pos, format, in.size); }
    catch (const Problem&)
    {
        // Never shorten a valid frame on the basis of tag magic inside its payload.
        // Only on failure, distinguish a frame cut off by a recognizable trailer.
        const auto end = trailerStart(in, pos);
        if (end && *end < in.size && pos < *end)
        {
            try { readFrame(in, pos, format, *end); }
            catch (const Problem& bounded)
            {
                if (bounded.issue.rule == "flac.truncated-frame")
                    stop("flac.truncated-frame", "Recognized trailer boundary before a complete FLAC frame",
                        pos, *end - pos, "complete frame before trailer", "trailer at " + std::to_string(*end));
            }
        }
        throw;
    }
}

std::optional<U64> laterFrame(Input& in, U64 start, U64 end, const Format& format, bool variable, U64 expected)
{
    // Diagnostic only. Neither this offset nor any candidate's samples ever
    // re-enter the contiguous walk or authorize edits, even if both CRCs match.
    const U64 last = start + std::min<U64>(end - start, 65536);
    unsigned attempts = 0;
    for (U64 p = start + 1; p < last && end - p >= 2 && attempts < 8; ++p)
    {
        if (in.at(p) != 0xff || in.at(p + 1) != (variable ? 0xf9 : 0xf8)) continue;
        ++attempts;
        try { if (readFrame(in, p, format, end, 256u << 10).number >= expected) return p; }
        catch (const Problem&) {}
    }
    return {};
}

Bytes bigEndian(U64 value, unsigned n)
{
    Bytes bytes(n);
    while (n) { bytes[--n] = static_cast<Byte>(value); value >>= 8; }
    return bytes;
}

void add(Inspection& result, Issue issue)
{
    if (result.issues.size() >= 1024) unsupported("Too many individual findings for bounded inspection", issue.offset);
    result.issues.push_back(std::move(issue));
}

void compareField(Inspection& result, std::string rule, std::string detail, U64 at, unsigned n,
    U64 stored, U64 measured)
{
    if (stored == measured) return;
    Issue issue{std::move(rule), std::move(detail), at, n, std::to_string(measured), std::to_string(stored), false, {}};
    if (n == 8 || measured < (U64{1} << (8 * n)))
        issue.patch = Patch{at, bigEndian(stored, n), bigEndian(measured, n)};
    add(result, std::move(issue));
}

// Sorted seek points are matched in a single pass alongside the frames; placeholders
// intentionally do not impose any constraints on their offset or frame-length fields.
class SeekTargets
{
    U64 _pos, _end;
    std::optional<U64> _sample;
    void advance(Input& in)
    {
        if (_pos == _end) { _sample.reset(); return; }
        _sample = in.be(_pos, 8);
    }
    void unmatched(Inspection& result)
    {
        add(result, {"flac.seek-target", "Seek sample does not identify any frame start; cannot infer a new target",
            _pos, 8, "first sample of a complete frame", std::to_string(*_sample), false, {}});
    }
public:
    SeekTargets(Input& in, const Metadata& m, Inspection& result) : _pos(m.seek), _end(m.seek + m.seekLength)
    {
        if (m.malformedSeek) return;
        std::optional<U64> previous;
        U64 realEnd = _end;
        for (U64 p = _pos; p < _end; p += 18)
        {
            const U64 value = in.be(p, 8);
            if (value == std::numeric_limits<U64>::max()) realEnd = std::min(realEnd, p);
            else if (p >= realEnd || (previous && value <= *previous))
            {
                add(result, {"flac.seek-target", "Malformed seek sample ordering; no targets can be safely rewritten",
                    p, 8, "strictly increasing unique real samples before placeholders", std::to_string(value), false, {}});
                return;
            }
            previous = value;
        }
        _end = realEnd;
        advance(in);
    }
    void observe(Input& in, Inspection& result, U64 firstSample, U64 relative, unsigned block)
    {
        while (_sample && *_sample < firstSample + block)
        {
            if (*_sample != firstSample) unmatched(result);
            else
            {
                compareField(result, "flac.seek-target", "Seek offset relative to first frame", _pos + 8, 8,
                    in.be(_pos + 8, 8), relative);
                compareField(result, "flac.seek-target", "Seek point frame sample count", _pos + 16, 2,
                    in.be(_pos + 16, 2), block);
            }
            _pos += 18;
            advance(in);
        }
    }
    void finish(Input& in, Inspection& result)
    {
        while (_sample)
        {
            unmatched(result);
            _pos += 18;
            advance(in);
        }
    }
};

void blockBounds(Input& in, Inspection& result, const Metadata& m, const Walk& w)
{
    const U64 min = in.be(m.info, 2), max = in.be(m.info + 2, 2);
    if (!w.last.variable && w.frames > 1)
    {
        compareField(result, "flac.block-bounds", "Minimum block size vs fixed nonfinal block", m.info, 2, min, w.nominal);
        compareField(result, "flac.block-bounds", "Maximum block size vs fixed nonfinal block", m.info + 2, 2, max, w.nominal);
        return;
    }
    if (!w.last.variable)
    {
        if (min != max || min < 16 || max < w.maxBlock)
            add(result, {"flac.block-bounds", "A lone final block does not prove a nominal fixed block size", m.info, 4,
                "equal legal bounds enclosing final block " + std::to_string(w.maxBlock),
                std::to_string(min) + ".." + std::to_string(max), false, {}});
        return;
    }
    // Conservative legal declarations need not equal the observed extrema. The
    // last block never constrains the minimum, even when it is the only block.
    const bool minBad = min < 16 || (w.frames > 1 && min > w.minNonfinal);
    const bool maxBad = max < 16 || max < w.maxBlock;
    if (min > max && !minBad && !maxBad)
    {
        add(result, {"flac.block-bounds", "Inverted variable block bounds with no unique contradicted field", m.info, 4,
            "minimum <= maximum", std::to_string(min) + ".." + std::to_string(max), false, {}});
        return;
    }
    U64 newMin = minBad ? (w.frames > 1 ? w.minNonfinal : 16) : min;
    U64 newMax = maxBad ? std::max<U64>({newMin, 16, w.maxBlock}) : max;
    if ((minBad || maxBad) && newMin == newMax)
    {
        // Do not turn a variable declaration into a fixed-size declaration for
        // readers that derive the distinction from STREAMINFO (RFC 9639 B.1).
        if (minBad && newMin > 16) --newMin;
        else if (maxBad && newMax < 65535) ++newMax;
        else
        {
            add(result, {"flac.block-bounds", "No distinct enclosing bounds without replacing a valid field",
                m.info, 4, "legal variable-block bounds", std::to_string(min) + ".." + std::to_string(max), false, {}});
            return;
        }
    }
    if (minBad) compareField(result, "flac.block-bounds", "Minimum must enclose all nonfinal blocks",
        m.info, 2, min, newMin);
    if (maxBad) compareField(result, "flac.block-bounds", "Maximum must enclose every block",
        m.info + 2, 2, max, newMax);
}

void streamFields(Input& in, Inspection& result, const Metadata& m, const Walk& w)
{
    if (result.declaredSamples && result.declaredSamples != result.samples)
    {
        Issue issue{"flac.total-samples", "STREAMINFO total differs from the continuous frame walk",
            m.info + 13, 5, std::to_string(result.samples), std::to_string(result.declaredSamples), false, {}};
        if (!result.md5.empty())
        {
            const U64 old = in.be(issue.offset, 5);
            issue.patch = Patch{issue.offset, bigEndian(old, 5), bigEndian((old & ~MaxSamples) | result.samples, 5)};
            issue.detail += "; correction requires the original MD5 to authenticate these samples";
        }
        else issue.detail += "; no original MD5 to distinguish a stale count from lost whole frames";
        add(result, std::move(issue));
    }
    blockBounds(in, result, m, w);
    const U64 minFrame = in.be(m.info + 4, 3), maxFrame = in.be(m.info + 7, 3);
    if (minFrame) compareField(result, "flac.frame-sizes", "Minimum complete frame length in bytes",
        m.info + 4, 3, minFrame, w.minFrame);
    if (maxFrame) compareField(result, "flac.frame-sizes", "Maximum complete frame length in bytes",
        m.info + 7, 3, maxFrame, w.maxFrame);
}

void inspect(Input& in, Inspection& result)
{
    const Metadata m = metadata(in, result);
    if (result.md5.empty())
    {
        Issue issue{"flac.fill-md5", "STREAMINFO MD5 is absent (permitted); a proven fill describes the current PCM, not its provenance",
            m.info + 18, 16, "new MD5 from a complete strict decode", std::string(32, '0'), false, {}};
        issue.patch = Patch{issue.offset, Bytes(16), Bytes(16)};
        add(result, std::move(issue));
    }
    SeekTargets targets(in, m, result);
    U64 pos = m.audio;
    Walk w;
    while (pos < in.size)
    {
        const bool syncPrefix = in.at(pos) == 0xff && (pos + 1 == in.size || (in.at(pos + 1) & 0xfe) == 0xf8);
        if (!syncPrefix)
        {
            if (!w.frames) stop("flac.lost-sync", "No frame at first audio boundary", pos, 1);
            const auto trailer = trailerStart(in, pos);
            if (!trailer)
                stop("flac.trailing-data", "Conflicting or malformed trailer after a verified frame", pos, in.size - pos,
                    "unambiguous confirmed external tag layout", witnesses(w, result));
            const U64 end = *trailer;
            if (end != pos)
            {
                const bool zeros = in.zeros(pos, end);
                const bool countMatches = result.declaredSamples && result.declaredSamples == result.samples;
                const auto next = zeros ? std::optional<U64>{} :
                    laterFrame(in, pos, end, m.format, w.last.variable, w.last.variable ? result.samples : w.frames);
                const bool internal = next || (!zeros && result.declaredSamples > result.samples);
                Issue issue{internal ? "flac.lost-sync" : "flac.trailing-data",
                    internal ? "Expected another frame before the declared end; remainder unverified"
                             : "Data after the last syntax- and CRC-verified frame",
                    pos, next.value_or(end) - pos, "no fill; declared samples/channel=" +
                        std::to_string(result.declaredSamples) + "; STREAMINFO MD5=" +
                        (result.md5.empty() ? "unknown" : result.md5),
                    witnesses(w, result) + "; remainder=" + (zeros ? "all zero" : "nonzero/unknown") +
                        "; retained trailer bytes=" + std::to_string(in.size - end), false, {}};
                if (next)
                {
                    issue.detail = "Gap at expected boundary; later CRC-valid frame is diagnostic only";
                    issue.observed += "; candidate frame at " + std::to_string(*next);
                }
                if (zeros && countMatches) issue.patch = Patch{pos, {}, {}, end - pos};
                result.issues.push_back(std::move(issue));
                if (!zeros || !countMatches) return;
            }
            break;
        }
        Frame frame = physicalFrame(in, pos, m.format);
        const U64 expected = frame.variable ? result.samples : w.frames;
        if (w.frames && frame.variable != w.last.variable)
            stop("flac.number-discontinuity", "Frame blocking strategy changed", pos + 1, 1,
                w.last.variable ? "variable" : "fixed", frame.variable ? "variable" : "fixed");
        if (frame.number != expected)
            stop("flac.number-discontinuity", frame.variable ? "Nonconsecutive sample number" : "Nonconsecutive frame number",
                pos + 4, frame.numberEnd - pos - 4, std::to_string(expected), std::to_string(frame.number));
        if (w.frames)
        {
            if (w.last.block < 16 || (!frame.variable && w.last.block != w.nominal))
                stop("flac.block-bounds", "Nonfinal frame violates blocking strategy", w.lastStart, w.last.end - w.lastStart);
            w.minNonfinal = std::min(w.minNonfinal, w.last.block);
        }
        else w.nominal = frame.block;
        if (!frame.variable && frame.block > w.nominal)
            stop("flac.block-bounds", "Fixed-strategy block exceeds the observed nominal size", pos, frame.end - pos);
        if (result.samples > MaxSamples - frame.block) unsupported("Sample count exceeds 36-bit inspection limit", pos);
        targets.observe(in, result, result.samples, pos - m.audio, frame.block);
        result.samples += frame.block;
        ++w.frames;
        w.maxBlock = std::max(w.maxBlock, frame.block);
        w.minFrame = std::min(w.minFrame, frame.end - pos);
        w.maxFrame = std::max(w.maxFrame, frame.end - pos);
        w.last = frame;
        w.lastStart = pos;
        pos = frame.end;
    }
    if (!w.frames) stop("flac.truncated-frame", "No FLAC frame follows metadata", pos);
    targets.finish(in, result);
    streamFields(in, result, m, w);
    result.complete = true;
}
}

Inspection ScanFlac(const std::filesystem::path& path)
{
    Inspection result;
    try
    {
        Input input(path);
        inspect(input, result);
    }
    catch (Problem& p)
    {
        result.issues.push_back(std::move(p.issue));
    }
    if (!result.complete || std::any_of(result.issues.begin(), result.issues.end(),
        [](const Issue& issue) { return !issue.informational && !issue.patch; }))
    {
        result.complete = false;
        for (auto& issue : result.issues) issue.patch.reset();
    }
    return result;
}
}
