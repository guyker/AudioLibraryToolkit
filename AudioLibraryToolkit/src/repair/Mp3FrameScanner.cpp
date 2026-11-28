#include "RepairIntegrity.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <span>
#include <stdexcept>

namespace RepairIntegrity
{
namespace
{
using Offset = std::uint64_t;
using View = std::span<const std::uint8_t>;

bool fits(Offset pos, Offset n, Offset end) { return pos <= end && n <= end - pos; }
bool magic(View v, std::size_t p, std::string_view s)
{
    return fits(p, s.size(), v.size()) && std::equal(s.begin(), s.end(), v.begin() + p);
}
unsigned be(View v, std::size_t p, unsigned n)
{
    unsigned x = 0;
    while (n--) x = (x << 8) | v[p++];
    return x;
}
unsigned le(View v, std::size_t p, unsigned n)
{
    unsigned x = 0;
    while (n) x = (x << 8) | v[p + --n];
    return x;
}
std::optional<unsigned> synchsafe(View v, std::size_t pos)
{
    if (!fits(pos, 4, v.size())) return {};
    unsigned x = 0;
    for (auto b : v.subspan(pos, 4)) {
        if (b & 128) return {};
        x = (x << 7) | b;
    }
    return x;
}

class Reader
{
public:
    explicit Reader(const std::filesystem::path& path) : _file(path, std::ios::binary | std::ios::ate)
    {
        auto end = _file.tellg();
        if (end < 0) throw std::runtime_error("Cannot open or measure input.");
        size = static_cast<Offset>(end);
    }
    // The returned view expires on the next read. Large tag values are skipped, not loaded.
    View Get(Offset p, std::size_t n)
    {
        if (!fits(p, n, size) || n > _buffer.size()) return {};
        if (p < _start || !fits(p - _start, n, _used)) {
            _start = p;
            _used = static_cast<std::size_t>(std::min<Offset>(_buffer.size(), size - p));
            _file.seekg(static_cast<std::streamoff>(p));
            if (!_file.read(reinterpret_cast<char*>(_buffer.data()), _used)) throw std::runtime_error("Input read failed.");
        }
        return View(_buffer).subspan(static_cast<std::size_t>(p - _start), n);
    }
    bool Is(Offset p, std::string_view text) { return magic(Get(p, text.size()), 0, text); }
    std::optional<Offset> Find(Offset p, Offset end, std::string_view text)
    {
        while (fits(p, text.size(), end)) {
            const auto v = Get(p, static_cast<std::size_t>(std::min<Offset>(end - p, 8192)));
            const auto match = std::search(v.begin(), v.end(), text.begin(), text.end());
            if (match != v.end()) return p + (match - v.begin());
            p += v.size() - text.size() + 1;
        }
        return {};
    }
    bool Zeros(Offset p, Offset end)
    {
        while (p < end) {
            const auto v = Get(p, static_cast<std::size_t>(std::min<Offset>(end - p, _buffer.size())));
            if (std::any_of(v.begin(), v.end(), [](auto b) { return b != 0; })) return false;
            p += v.size();
        }
        return true;
    }
    Offset size;
private:
    std::ifstream _file;
    std::array<std::uint8_t, 65536> _buffer{};
    Offset _start = 0;
    std::size_t _used = 0;
};

struct Indeterminate : std::runtime_error
{
    explicit Indeterminate(const char* detail, Offset at = 0, Offset n = 0)
        : std::runtime_error(detail), offset(at), length(n) {}
    Offset offset, length;
};
void add(Inspection& r, std::string rule, std::string detail, Offset off, Offset len,
    std::string expected = {}, std::string observed = {}, bool info = false)
{
    if (r.issues.size() >= 128) throw Indeterminate("Too many structural findings; inspection stopped.");
    r.issues.push_back({std::move(rule), std::move(detail), off, len, std::move(expected), std::move(observed), info, {}});
}
bool idChar(unsigned b) { return (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9'); }

Offset id3(Reader& f, Offset p, Offset end)
{
    auto invalid = [p] { throw Indeterminate("ID3v2 layout is malformed or unsupported.", p); };
    if (!fits(p, 10, end)) invalid();
    const auto h = f.Get(p, 10);
    const auto version = h[3], flags = h[5];
    const auto size = synchsafe(h, 6);
    if (!magic(h, 0, "ID3") || version < 2 || version > 4 || h[4] == 255 || !size ||
        (flags & (version == 2 ? 0x3f : version == 3 ? 0x1f : 0x0f)) ||
        (version < 4 && (flags & 128)) || (version == 2 && (flags & 64))) invalid();
    const bool footer = version == 4 && (flags & 16);
    Offset body = p + 10;
    if (!fits(body, *size + (footer ? 10 : 0), end)) invalid();
    const Offset stop = body + *size;
    if (footer) {
        std::array<std::uint8_t, 7> fields;
        std::copy(h.begin() + 3, h.end(), fields.begin());
        const auto foot = f.Get(stop, 10);
        if (!magic(foot, 0, "3DI") || !std::equal(fields.begin(), fields.end(), foot.begin() + 3)) invalid();
    }
    if (flags & 64) {
        if (!fits(body, 6, stop)) invalid();
        const auto ext = f.Get(body, 6);
        if (version == 3) {
            const auto n = be(ext, 0, 4), extFlags = be(ext, 4, 2);
            if ((extFlags & 0x7fff) || n != (extFlags ? 10u : 6u) || !fits(body + 4, n, stop)) invalid();
            body += 4 + n;
        } else {
            const auto n = synchsafe(ext, 0);
            const unsigned extFlags = ext[5];
            if (!n || *n < 6 || !fits(body, *n, stop) || ext[4] != 1 || (extFlags & 0x8f)) invalid();
            const Offset after = body + *n;
            body += 6;
            for (auto [mask, bytes] : {std::pair{64u, 0u}, {32u, 5u}, {16u, 1u}}) {
                if (!(extFlags & mask)) continue;
                if (!fits(body, 1 + bytes, after) || f.Get(body, 1)[0] != bytes) invalid();
                body += 1 + bytes;
            }
            if (body != after) invalid();
        }
    }
    const unsigned header = version == 2 ? 6 : 10;
    for (unsigned count = 0; body < stop; ++count) {
        if (count > 65536) throw Indeterminate("ID3v2 frame inventory limit exceeded.");
        if (!f.Get(body, 1)[0]) {
            if (footer || !f.Zeros(body, stop)) invalid();
            break;
        }
        if (!fits(body, header, stop)) invalid();
        const auto fr = f.Get(body, header);
        const unsigned id = version == 2 ? 3 : 4;
        if (!std::all_of(fr.begin(), fr.begin() + id, idChar)) invalid();
        const auto n = version == 4 ? synchsafe(fr, id) : std::optional{be(fr, id, id)};
        if (!n || !fits(body + header, *n, stop)) invalid();
        if ((version == 3 && ((fr[8] | fr[9]) & 31)) ||
            (version == 4 && ((fr[8] & 0x8f) || (fr[9] & 0xb0)))) invalid();
        body += header + *n;
    }
    return stop + (footer ? 10 : 0);
}

Offset apeStart(Reader& f, Offset end)
{
    const auto foot = f.Get(end - 32, 32);
    const unsigned size = le(foot, 12, 4), items = le(foot, 16, 4), flags = le(foot, 20, 4);
    const bool header = (flags & 0x80000000u) != 0;
    auto invalid = [] { throw Indeterminate("Invalid or unsupported APEv2 boundary tag."); };
    if (le(foot, 8, 4) != 2000 || (flags & ~0x80000001u) || be(foot, 24, 4) || be(foot, 28, 4) ||
        size < 32 || size + Offset(header ? 32 : 0) > end || items > 65536) invalid();
    const Offset start = end - size - (header ? 32 : 0);
    if (header) {
        auto h = f.Get(start, 32);
        if (!magic(h, 0, "APETAGEX") || le(h, 8, 4) != 2000 || le(h, 12, 4) != size ||
            le(h, 16, 4) != items || le(h, 20, 4) != (flags | 0x20000000u) || be(h, 24, 4) || be(h, 28, 4)) invalid();
    }
    Offset pos = end - size;
    for (unsigned i = 0; i < items; ++i) {
        if (!fits(pos, 11, end - 32)) invalid();
        auto h = f.Get(pos, 8);
        const auto n = le(h, 0, 4);
        if (le(h, 4, 4) & ~7u) invalid();
        pos += 8;
        unsigned keys = 0;
        while (pos < end - 32 && keys <= 255) {
            const auto b = f.Get(pos++, 1)[0];
            if (!b) break;
            if (b < 32 || b > 126 || b == '=') invalid();
            ++keys;
        }
        if (keys < 2 || keys > 255 || f.Get(pos - 1, 1)[0] || !fits(pos, n, end - 32)) invalid();
        pos += n;
    }
    if (pos != end - 32) invalid();
    return start;
}

unsigned decimal(View v)
{
    unsigned n = 0;
    for (auto b : v) {
        if (b < '0' || b > '9') throw Indeterminate("Malformed Lyrics3 size.");
        n = n * 10 + b - '0';
    }
    return n;
}

struct Boundaries
{
    Offset start, end;
    std::vector<Offset> tailEnds;
    bool ambiguousTail = false;
    bool strongTail = false;
};
Boundaries tags(Reader& f)
{
    Offset p = 0, end = f.size;
    for (unsigned i = 0; f.Is(p, "ID3") || f.Is(p, "APETAGEX"); ++i) {
        if (i == 64) throw Indeterminate("Leading tag inventory limit exceeded.");
        if (f.Is(p, "ID3")) p = id3(f, p, end);
        else {
            const auto h = f.Get(p, 32);
            if (h.empty()) throw Indeterminate("Incomplete APE boundary.");
            const unsigned n = le(h, 12, 4);
            if (!(le(h, 20, 4) & 0x20000000u)) break; // An empty footer-only tag may be the entire file.
            if (n < 32 || !fits(p + 32, n, end) || !f.Is(p + n, "APETAGEX") || apeStart(f, p + 32 + n) != p)
                throw Indeterminate("Unknown leading APE layout.");
            p += 32 + Offset(n);
        }
    }
    auto tagStart = [&] {
        Offset next = end;
        if (end - p >= 128 && f.Is(end - 128, "TAG")) next = end - 128;
        else if (end - p >= 32 && f.Is(end - 32, "APETAGEX")) next = apeStart(f, end);
        else if (end - p >= 10 && f.Is(end - 10, "3DI")) {
            const auto foot = f.Get(end - 10, 10);
            const auto n = synchsafe(foot, 6);
            if (foot[3] != 4 || !(foot[5] & 16) || !n || *n + 20u > end - p ||
                id3(f, end - 20 - *n, end) != end) throw Indeterminate("Unmatched appended ID3v2 footer.");
            next = end - 20 - *n;
        } else if (end - p >= 15 && f.Is(end - 9, "LYRICS200")) {
            const auto n = decimal(f.Get(end - 15, 6));
            if (n < 11 || n > end - p - 15 || !f.Is(end - 15 - n, "LYRICSBEGIN"))
                throw Indeterminate("Unmatched Lyrics3v2 footer.");
            next = end - 15 - n;
            for (Offset field = next + 11; field < end - 15;) {
                if (!fits(field, 8, end - 15)) throw Indeterminate("Incomplete Lyrics3 field.");
                auto v = f.Get(field, 8);
                if (!std::all_of(v.begin(), v.begin() + 3, [](auto c) { return c >= 'A' && c <= 'Z'; }))
                    throw Indeterminate("Invalid Lyrics3 field ID.");
                const auto bytes = decimal(v.subspan(3, 5));
                if (!fits(field + 8, bytes, end - 15)) throw Indeterminate("Invalid Lyrics3 field size.");
                field += 8 + bytes;
            }
        } else if (end - p >= 20 && f.Is(end - 9, "LYRICSEND")) {
            auto n = static_cast<std::size_t>(std::min<Offset>(end - p, 5120));
            auto v = f.Get(end - n, n - 9);
            constexpr std::string_view marker = "LYRICSBEGIN";
            auto m = std::search(v.begin(), v.end(), marker.begin(), marker.end());
            if (m == v.end()) throw Indeterminate("Unmatched Lyrics3v1 footer.");
            next = end - n + static_cast<Offset>(m - v.begin());
        }
        if (next < p) throw Indeterminate("Overlapping boundary tags.");
        return next;
    };
    Boundaries bounds{p, end, {end}};
    try {
        for (unsigned i = 0; end > p; ++i) {
            if (i == 64) throw Indeterminate("Trailing tag inventory limit exceeded.", end);
            const auto next = tagStart();
            if (next == end) break;
            bounds.strongTail = end - next != 128 || !f.Is(next, "TAG");
            end = next;
            bounds.tailEnds.push_back(next);
        }
    } catch (const Indeterminate&) {
        // A footer signature in the last frame's payload is not evidence of a malformed tag.
        // Defer this verdict until the forward walk actually reaches unexplained bytes.
        bounds.ambiguousTail = true;
    }
    bounds.end = end;
    return bounds;
}

struct Header
{
    unsigned version, rate, bitrate, mode, bytes, side;
    bool crc;
    unsigned Prefix() const { return (crc ? 6 : 4) + side; }
    bool V1() const { return version == 3; }
};

std::optional<Header> header(View v)
{
    if (v.size() < 4 || v[0] != 255 || (v[1] & 0xe6) != 0xe2) return {};
    unsigned version = (v[1] >> 3) & 3, index = v[2] >> 4, sr = (v[2] >> 2) & 3, mode = v[3] >> 6;
    if (version == 1 || index == 15 || sr == 3 || (v[3] & 3) == 2) return {};
    constexpr unsigned rates[] = {44100, 48000, 32000};
    constexpr unsigned bitrates[][15] = {{0,8,16,24,32,40,48,56,64,80,96,112,128,144,160},
        {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320}};
    const unsigned rate = rates[sr] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    const unsigned bitrate = bitrates[version == 3][index];
    return Header{version, rate, bitrate, mode, index ? (version == 3 ? 144000u : 72000u) * bitrate / rate +
        ((v[2] >> 1) & 1) : 0u, version == 3 ? (mode == 3 ? 17u : 32u) : (mode == 3 ? 9u : 17u), !(v[1] & 1)};
}

class SideBits
{
public:
    explicit SideBits(View v) : _v(v) {}
    unsigned Take(unsigned n)
    {
        unsigned x = 0;
        while (n--) {
            x = (x << 1) | ((_v[_pos / 8] >> (7 - _pos % 8)) & 1);
            ++_pos;
        }
        return x;
    }
private:
    View _v;
    unsigned _pos = 0;
};
struct SideInfo { unsigned back = 0, codedBits = 0; bool legal = true; };
SideInfo sideInfo(View frame, Header h)
{
    const auto channels = h.mode == 3 ? 1u : 2u;
    SideBits bits(frame.subspan(h.crc ? 6 : 4, h.side));
    SideInfo s;
    s.back = bits.Take(h.V1() ? 9 : 8);
    bits.Take(h.V1() ? (channels == 1 ? 5 : 3) + 4 * channels : channels);
    for (unsigned i = 0; i < channels * (h.V1() ? 2 : 1); ++i) {
        s.codedBits += bits.Take(12);
        if (bits.Take(9) > 288) s.legal = false;
        bits.Take(8 + (h.V1() ? 4 : 9));
        if (bits.Take(1)) {
            if (!bits.Take(2)) s.legal = false;
            bits.Take(1 + 10 + 9);
        } else bits.Take(15 + 4 + 3);
        bits.Take(h.V1() ? 3 : 2);
    }
    return s;
}

bool candidate(Reader& f, Offset p, Offset end, bool anchored)
{
    if (!fits(p, 4, end)) return false;
    const auto h = header(f.Get(p, 4));
    if (!h || h->bytes < h->Prefix() || !fits(p, h->bytes, end)) return false;
    if (!sideInfo(f.Get(p, h->bytes), *h).legal) return false;
    const Offset next = p + h->bytes;
    if (anchored && next == end) return true;
    if (!fits(next, 4, end)) return false;
    const auto second = header(f.Get(next, 4));
    return second && second->bytes >= second->Prefix() && (anchored || fits(next, second->bytes, end));
}

constexpr Offset searchLimit = 16 * 1024 * 1024;
std::optional<Offset> findRun(Reader& f, Offset p, Offset end)
{
    const auto limit = p + std::min(searchLimit, end - p);
    while (p < limit) {
        const auto n = static_cast<std::size_t>(std::min<Offset>(4096, limit - p));
        auto v = f.Get(p, n);
        auto it = std::find(v.begin(), v.end(), 255);
        if (it == v.end()) { p += n; continue; }
        p += it - v.begin();
        if (candidate(f, p, end, false)) return p;
        ++p;
    }
    if (limit < end) throw Indeterminate("No confirmed frame run within the diagnostic search limit.");
    return {};
}

unsigned mpegCrc(View v, unsigned crc)
{
    for (auto b : v) {
        crc ^= b << 8;
        for (int i = 0; i < 8; ++i) crc = ((crc << 1) ^ ((crc & 0x8000) ? 0x8005 : 0)) & 0xffff;
    }
    return crc;
}
constexpr auto lameTable = [] {
    std::array<unsigned, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        unsigned x = i;
        for (int j = 0; j < 8; ++j) x = (x >> 1) ^ ((x & 1) ? 0xa001 : 0);
        table[i] = x;
    }
    return table;
}();
unsigned lameCrc(View v, unsigned crc)
{
    for (auto b : v) crc = (crc >> 8) ^ lameTable[(crc ^ b) & 255];
    return crc;
}
std::string crcText(unsigned crc)
{
    std::string s = "0x0000";
    for (int i = 5; i >= 2; --i) { s[i] = "0123456789ABCDEF"[crc & 15]; crc >>= 4; }
    return s;
}
struct Field { Offset pos; unsigned value; };
void compare(Inspection& r, std::string rule, std::string detail, Field stored, Offset measured, bool crc = false)
{
    if (stored.value != measured) add(r, std::move(rule), std::move(detail), stored.pos, crc ? 2 : 4,
        crc ? crcText(static_cast<unsigned>(measured)) : std::to_string(measured),
        crc ? crcText(stored.value) : std::to_string(stored.value));
}

struct Carrier
{
    bool present = false, vbri = false, knownCounts = false;
    unsigned size = 0, musicCrc = 0, scale = 0, perEntry = 0;
    Offset tablePos = 0;
    unsigned width = 0;
    std::optional<Field> frames, bytes, checksum;
    std::vector<Offset> cumulative;
    bool spansMatch[3] = {true, true, true};
};

Carrier carrier(View v, Header h, SideInfo side, Offset p, Inspection& r)
{
    Carrier c;
    unsigned x = 4 + h.side;
    if (h.crc && !magic(v, x, "Xing") && !magic(v, x, "Info")) x += 2;
    const bool xing = magic(v, x, "Xing") || magic(v, x, "Info");
    const bool vbri = magic(v, 36, "VBRI");
    // LAME's protected carrier places Xing at the usual offset, overlapping two side-info bytes.
    if ((!xing && !vbri) || side.back || (side.codedBits && !(xing && h.crc && x == 4 + h.side))) return c;
    c.present = true;
    c.size = h.bytes;
    if (xing) {
        if (!fits(x + 4, 4, v.size())) throw Indeterminate("Incomplete Xing/Info flags.");
        const auto flags = be(v, x + 4, 4);
        if (flags & ~15u) throw Indeterminate("Unsupported Xing/Info flags.");
        x += 8;
        for (auto [mask, count] : {std::pair{1u,4u}, {2u,4u}, {4u,100u}, {8u,4u}}) {
            if (!(flags & mask)) continue;
            if (!fits(x, count, v.size())) {
                add(r, mask == 4 ? "mp3.xing-toc" : "mp3.xing-counts", "Flagged Xing/Info field exceeds its carrier.",
                    p + x, v.size() - x, std::to_string(count), std::to_string(v.size() - x));
                return c;
            }
            if (mask == 1) c.frames = Field{p + x, be(v, x, 4)};
            if (mask == 2) c.bytes = Field{p + x, be(v, x, 4)};
            if (mask == 4 && !std::is_sorted(v.begin() + x, v.begin() + x + count))
                add(r, "mp3.xing-toc", "Approximate Xing seek positions decrease.", p + x, count);
            x += count;
        }
        const bool lame = fits(x, 9, v.size()) && magic(v, x, "LAME") && v[x + 4] == '3' && v[x + 5] == '.' &&
            v[x + 6] >= '0' && v[x + 6] <= '9' && v[x + 7] >= '0' && v[x + 7] <= '9';
        c.knownCounts = lame || magic(v, x, "Lavf") || magic(v, x, "Lavc");
        if (lame) {
            if (flags != 15 || !fits(x, 36, v.size()) || (v[x + 9] >> 4) != 0)
                throw Indeterminate("LAME extension layout or revision is unsupported.", p + x);
            c.checksum = Field{p + x + 32, be(v, x + 32, 2)};
            compare(r, "mp3.lame-tag-crc", "LAME tag CRC-16 disagrees with the carrier through the music CRC.",
                {p + x + 34, be(v, x + 34, 2)}, lameCrc(v.first(x + 34), 0), true);
        }
    } else {
        c.vbri = true;
        if (!fits(36, 26, v.size()) || be(v, 40, 2) != 1) throw Indeterminate("Unsupported or incomplete VBRI layout.");
        c.frames = Field{p + 50, be(v, 50, 4)};
        c.bytes = Field{p + 46, be(v, 46, 4)};
        const auto n = be(v, 54, 2);
        c.scale = be(v, 56, 2);
        c.width = be(v, 58, 2);
        c.perEntry = be(v, 60, 2);
        c.tablePos = p + 62;
        if (c.width == 0 || c.width > 4 || !c.scale || !c.perEntry || !n || !fits(62, n * c.width, v.size())) {
            add(r, "mp3.vbri-toc", "VBRI table dimensions are invalid or exceed the carrier.", p + 54, 8);
        } else {
            Offset sum = 0;
            for (unsigned i = 0; i < n; ++i) {
                sum += Offset(be(v, 62 + i * c.width, c.width)) * c.scale;
                c.cumulative.push_back(sum);
            }
        }
    }
    return c;
}

void group(Carrier& c, unsigned layout, Offset count, Offset bytes, bool last)
{
    if (c.cumulative.empty() || !count || (!last && count % c.perEntry)) return;
    const Offset index = (count - 1) / c.perEntry;
    if (index >= c.cumulative.size() || (last && index + 1 != c.cumulative.size())) { c.spansMatch[layout] = false; return; }
    // Quantization may round each segment. Frame grouping and byte offsets can use different carrier conventions.
    const auto stored = c.cumulative[static_cast<std::size_t>(index)];
    const auto tolerance = (index + 1) * (c.scale - 1);
    if ((stored > bytes ? stored - bytes : bytes - stored) > tolerance) c.spansMatch[layout] = false;
}

void finishCarrier(Carrier& c, Inspection& r, Offset musicFrames, Offset musicBytes, bool varied, Offset start)
{
    if (c.vbri) {
        if (c.frames->value != musicFrames && c.frames->value != musicFrames + 1)
            compare(r, "mp3.vbri-counts", "VBRI frame count matches neither carrier convention.", *c.frames, musicFrames);
        if (c.bytes->value != musicBytes && c.bytes->value != musicBytes + c.size)
            compare(r, "mp3.vbri-counts", "VBRI byte count matches neither carrier convention.", *c.bytes, musicBytes);
        group(c, 0, musicFrames, musicBytes, true);
        group(c, 1, musicFrames + 1, musicBytes + c.size, true);
        group(c, 2, musicFrames, musicBytes + c.size, true);
        if (!c.spansMatch[0] && !c.spansMatch[1] && !c.spansMatch[2])
            add(r, "mp3.vbri-toc", "Scaled VBRI group spans disagree with measured carrier conventions.",
                c.tablePos, c.width * c.cumulative.size());
    } else if (c.present) {
        if (c.knownCounts) {
            if (c.frames) compare(r, "mp3.xing-counts", "Music-frame count excludes the Xing/Info carrier.", *c.frames, musicFrames);
            if (c.bytes) compare(r, "mp3.xing-counts", "MPEG byte count includes the carrier but excludes external tags.",
                *c.bytes, musicBytes + c.size);
        } else if ((c.frames && c.frames->value != musicFrames) || (c.bytes && c.bytes->value != musicBytes + c.size))
            add(r, "mp3.unsupported-stream", "Xing/Info count convention is unknown; no count mismatch asserted.", start, c.size);
        if (c.checksum) compare(r, "mp3.lame-music-crc", "LAME CRC-16 over encoded music frames (not PCM).",
            *c.checksum, c.musicCrc, true);
    } else if (varied) add(r, "mp3.no-vbr-header", "Music bitrates vary without a seek header; this is legal.", start, 4, {}, {}, true);
}

std::string parameters(Header h)
{
    return std::string(h.V1() ? "MPEG-1" : h.version == 2 ? "MPEG-2" : "MPEG-2.5") + ", " +
        std::to_string(h.rate) + " Hz, " + std::array{"stereo", "joint stereo", "dual channel", "mono"}[h.mode];
}

void walk(Reader& f, Inspection& r)
{
    const auto bounds = tags(f);
    auto p = bounds.start, end = bounds.end;
    if (f.Is(p, "RIFF") || f.Is(p, "OggS") || f.Is(p, "fLaC") || f.Is(p, "FORM") ||
        (fits(p, 8, end) && f.Is(p + 4, "ftyp")))
        throw Indeterminate("Possible outer container; not walking its payload as elementary MP3.", p);
    auto h = header(f.Get(p, 4));
    if (!h) {
        const auto first = findRun(f, p, end);
        if (!first) throw Indeterminate("No confirmed indexed Layer III frame run.");
        add(r, "mp3.leading-data", "Unidentified bytes before a corroborated Layer III frame run.", p, *first - p);
        p = *first;
    }
    const Offset start = p;
    Offset musicFrames = 0, musicBytes = 0;
    unsigned reservoir = 0;
    std::optional<Header> prev;
    bool varied = false, consistent = true, cleanRun = start == bounds.start, notedMode = false, notedFormat = false;
    Carrier c;
    while (p < end) {
        h = header(f.Get(p, 4));
        if (!h) {
            if (f.Is(p, "ID3")) {
                const auto next = id3(f, p, end);
                if (!candidate(f, next, end, true)) throw Indeterminate("ID3 at a frame boundary without a confirmed following run.", p, next - p);
                add(r, "mp3.embedded-id3", "ID3v2 at an inter-frame boundary; timing or concatenation may be intentional.", p, next - p);
                cleanRun = false;
                p = next;
                continue;
            }
            if (end - p < 4 && f.Get(p, 1)[0] == 255)
                throw Indeterminate("Possible partial MPEG header at the last frame boundary.", p, end - p);
            const auto next = findRun(f, p + 1, end);
            if (next) add(r, "mp3.lost-sync", "Expected boundary is not a header; a later run is only a recovery candidate.", p, *next - p);
            else if (bounds.ambiguousTail) throw Indeterminate("Unvalidated trailing tag layout.", p, end - p);
            else add(r, "mp3.trailing-data", "Unidentified bytes after the last complete frame, excluding recognized tags.", p, end - p);
            return; // A candidate is not a basis for a count, CRC, or reservoir verdict.
        }
        if (!h->bytes) throw Indeterminate("Free-format Layer III frame; its length is not indexed.", p, 4);
        if (h->bytes < h->Prefix()) throw Indeterminate("Layer III side information does not fit the indexed frame.", p, 4);
        if (!fits(p, h->bytes, end)) {
            // An ID3v1 signature can occur by chance 128 bytes from EOF inside the last frame.
            if (std::find(bounds.tailEnds.begin(), bounds.tailEnds.end(), p + h->bytes) != bounds.tailEnds.end()) {
                if (bounds.strongTail) throw Indeterminate("Indexed frame and validated metadata claim overlapping bytes.", p, h->bytes);
                end = p + h->bytes;
            } else if (fits(p, h->bytes, f.size) && !bounds.strongTail)
                throw Indeterminate("An indexed MPEG frame overlaps a possible external tag.", p, h->bytes);
            else {
                add(r, "mp3.truncated-frame", "Final indexed Layer III frame is incomplete.", p, end - p,
                    std::to_string(h->bytes), std::to_string(end - p));
                return;
            }
        }
        const auto v = f.Get(p, h->bytes);
        const auto s = sideInfo(v, *h);
        if (p == start) c = carrier(v, *h, s, p, r);
        else if (!s.back && !s.codedBits && (magic(v, 4 + h->side, "Xing") || magic(v, 4 + h->side, "Info") ||
            magic(v, h->Prefix(), "Xing") || magic(v, h->Prefix(), "Info") || magic(v, 36, "VBRI")))
            throw Indeterminate("Another potential encoder carrier interrupts the music run; totals are indeterminate.", p, h->bytes);
        const bool tagFrame = p == start && c.present;
        if (h->crc) {
            const auto crc = mpegCrc(v.subspan(6, h->side), mpegCrc(v.subspan(2, 2), 0xffff));
            compare(r, "mp3.frame-crc", "MPEG protection CRC-16 over header bits and side information.",
                {p + 4, be(v, 4, 2)}, crc, true);
        }
        if (!tagFrame) {
            const unsigned payload = h->bytes - h->Prefix();
            if (!s.legal || s.back > reservoir || s.codedBits > (s.back + payload) * 8) {
                add(r, "mp3.lost-sync", !s.legal ? "Invalid Layer III side information." :
                    s.back > reservoir ? "Reservoir reference exceeds available unused main-data bytes." :
                    "Coded granule bits exceed available main data.", p + (h->crc ? 6 : 4), h->side,
                    s.legal ? std::to_string(s.back > reservoir ? reservoir : (s.back + payload) * 8) : "",
                    s.legal ? std::to_string(s.back > reservoir ? s.back : s.codedBits) : "");
                cleanRun = false;
            }
            const auto consumed = (s.codedBits + 7) / 8;
            reservoir = s.back + payload >= consumed ? s.back + payload - consumed : 0;
            if (prev) {
                const bool format = prev->rate != h->rate || prev->version != h->version || (prev->mode == 3) != (h->mode == 3);
                if ((format && !notedFormat) || (!format && prev->mode != h->mode && !notedMode)) {
                    add(r, "mp3.stream-parameters", format ? "Music stream parameters change at this frame." :
                        "A channel-mode switch with unchanged channel count is legal.", p, 4, parameters(*prev), parameters(*h), !format);
                    (format ? notedFormat : notedMode) = true;
                }
                consistent &= !format;
                varied |= prev->bitrate != h->bitrate;
            }
            prev = h;
            ++musicFrames;
            musicBytes += h->bytes;
            if (h->V1() ? r.samples > UINT64_MAX - 1152 : r.samples > UINT64_MAX - 576)
                throw Indeterminate("Nominal sample count exceeds reporting range.");
            r.samples += h->V1() ? 1152 : 576;
            if (c.checksum) c.musicCrc = lameCrc(v, c.musicCrc);
            group(c, 0, musicFrames, musicBytes, false);
            group(c, 2, musicFrames, musicBytes + c.size, false);
        }
        group(c, 1, musicFrames + 1, musicBytes + c.size, false);
        p += h->bytes;
    }
    if (!musicFrames) throw Indeterminate("No complete indexed music frames.");
    if (cleanRun && consistent) finishCarrier(c, r, musicFrames, musicBytes, varied, start);
    r.complete = cleanRun;
}

// For container identification only; the FLAC integrity scanner owns the complete subframe walk.
unsigned flacHeader(Reader& f, Offset p, Offset end)
{
    const auto v = f.Get(p, static_cast<std::size_t>(std::min<Offset>(16, end - p)));
    if (v.size() < 6 || v[0] != 255 || (v[1] & 0xfe) != 0xf8 || (v[2] >> 4) == 0 || (v[2] & 15) == 15 ||
        (v[3] >> 4) > 10 || (v[3] & 1) || (v[3] & 14) == 6) return 0;
    unsigned n = 0;
    for (unsigned mask = 128; mask && (v[4] & mask); mask >>= 1) ++n;
    if (n == 1 || n > (v[1] & 1 ? 7u : 6u)) return 0;
    const unsigned numberBytes = std::max(n, 1u);
    unsigned size = 4 + numberBytes + ((v[2] >> 4) == 7 ? 2 : (v[2] >> 4) == 6 ? 1 : 0) +
        ((v[2] & 15) >= 13 ? 2 : (v[2] & 15) == 12 ? 1 : 0) + 1;
    if (size > v.size()) return 0;
    for (unsigned i = 1; i < numberBytes; ++i) if ((v[4 + i] & 0xc0) != 0x80) return 0;
    unsigned crc = 0;
    for (auto b : v.first(size)) {
        crc ^= b;
        for (int i = 0; i < 8; ++i) crc = ((crc << 1) ^ ((crc & 128) ? 7 : 0)) & 255;
    }
    return crc ? 0 : size;
}

std::optional<Offset> flacAudio(Reader& f, Offset p, Offset end)
{
    if (!fits(p, 42, end) || !f.Is(p, "fLaC")) return {};
    p += 4;
    for (unsigned i = 0; fits(p, 4, end); ++i) {
        if (i == 65536) throw Indeterminate("FLAC metadata inventory limit exceeded.");
        const auto v = f.Get(p, 4);
        const unsigned type = v[0] & 127, n = be(v, 1, 3);
        const bool last = (v[0] & 128) != 0;
        if ((i == 0 && (type != 0 || n != 34)) || (i && !type) || type == 127 || !fits(p + 4, n, end)) return {};
        if (!i) {
            const auto format = be(f.Get(p + 14, 4), 0, 4);
            if (!(format >> 12) || (((format >> 4) & 31) + 1) < 4) return {};
        }
        p += 4 + n;
        if (last) return p;
    }
    return {};
}

bool flacRun(Reader& f, Offset p, Offset end)
{
    const auto head = flacHeader(f, p, end);
    if (!head) return false;
    const auto limit = p + std::min<Offset>(end - p, 1024 * 1024);
    unsigned crc = 0;
    for (Offset i = p; i < limit; ++i) {
        if (i > p + head + 2 && !crc && f.Get(i, 1)[0] == 255 && flacHeader(f, i, end)) return true;
        crc = mpegCrc(f.Get(i, 1), crc); // Same MSB-first polynomial; FLAC starts at zero.
    }
    return limit == end && end - p > head + 2 && crc == 0;
}

std::optional<std::string> wave(Reader& f, Offset p, Offset end)
{
    if (!fits(p, 12, end) || !f.Is(p, "RIFF") || !f.Is(p + 8, "WAVE")) return {};
    const auto size = le(f.Get(p + 4, 4), 0, 4);
    if (size < 4 || !fits(p + 8, size, end)) return {};
    end = p + 8 + size;
    p += 12;
    std::optional<unsigned> format;
    bool data = false;
    for (unsigned i = 0; p < end; ++i) {
        if (i == 65536) throw Indeterminate("RIFF chunk inventory limit exceeded.");
        if (!fits(p, 8, end)) return {};
        const auto v = f.Get(p, 8);
        const Offset n = le(v, 4, 4);
        if (!fits(p + 8, n + n % 2, end)) return {};
        if (magic(v, 0, "data")) data = true;
        if (magic(v, 0, "fmt ")) {
            if (n < 16 || format) return {};
            auto fmt = f.Get(p + 8, 16);
            if (!le(fmt, 2, 2) || !le(fmt, 4, 4)) return {};
            format = le(fmt, 0, 2);
        }
        p += 8 + n + n % 2;
    }
    if (!format || !data) return {};
    return *format == 0x55 ? "RIFF/WAVE (MPEG Layer III)" : "RIFF/WAVE (format " + std::to_string(*format) + ")";
}

std::optional<std::string> ogg(Reader& f, Offset p, Offset end)
{
    if (!fits(p, 27, end)) return {};
    auto h = f.Get(p, 27);
    if (!magic(h, 0, "OggS") || h[4] != 0 || !(h[5] & 2) || (h[5] & ~6u) || le(h, 18, 4) || !h[26]) return {};
    const unsigned n = h[26], checksum = le(h, 22, 4);
    if (!fits(p + 27, n, end)) return {};
    const auto lace = f.Get(p + 27, n);
    unsigned first = 0, total = 0;
    bool found = false;
    for (auto b : lace) {
        total += b;
        if (!found) { first += b; found = b != 255; }
    }
    if (!found || !first || !fits(p + 27 + n, total, end)) return {};
    const auto page = f.Get(p, 27 + n + total);
    unsigned crc = 0;
    for (unsigned i = 0; i < page.size(); ++i) {
        crc ^= unsigned(i >= 22 && i < 26 ? 0 : page[i]) << 24;
        for (int j = 0; j < 8; ++j) crc = (crc << 1) ^ ((crc >> 31) ? 0x04c11db7u : 0);
    }
    if (crc != checksum) return {};
    const auto packet = page.subspan(27 + n, first);
    if (packet.size() == 51 && packet[0] == 0x7f && magic(packet, 1, "FLAC") && packet[5] == 1 && !packet[6] &&
        magic(packet, 9, "fLaC") && (packet[13] & 127) == 0 && be(packet, 14, 3) == 34) return "Ogg FLAC";
    if (packet.size() >= 19 && magic(packet, 0, "OpusHead") && packet[8] <= 15 && packet[9]) return "Ogg Opus";
    if (packet.size() >= 30 && packet[0] == 1 && magic(packet, 1, "vorbis") && !le(packet, 7, 4)) return "Ogg Vorbis";
    return "Ogg (unidentified mapping)";
}

bool otherMpeg(Reader& f, Offset p, Offset end)
{
    // Two compatible non-Layer-III headers at computed boundaries are container evidence, not an integrity scan.
    auto len = [](View v) -> unsigned {
        if (v.size() < 4 || v[0] != 255 || (v[1] & 0xe0) != 0xe0) return 0;
        unsigned ver = (v[1] >> 3) & 3, layer = (v[1] >> 1) & 3, br = v[2] >> 4, sr = (v[2] >> 2) & 3;
        if (ver == 1 || layer < 2 || !br || br == 15 || sr == 3 || (v[3] & 3) == 2) return 0;
        constexpr unsigned table[][15] = {{0,32,64,96,128,160,192,224,256,288,320,352,384,416,448},
            {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384}, {0,32,48,56,64,80,96,112,128,144,160,176,192,224,256},
            {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160}};
        const auto rate = std::array{44100u,48000u,32000u}[sr] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
        const auto kb = table[(ver == 3 ? 0 : 2) + (layer == 3 ? 0 : 1)][br];
        const unsigned pad = (v[2] >> 1) & 1;
        return layer == 3 ? (12000 * kb / rate + pad) * 4 : 144000 * kb / rate + pad;
    };
    const auto first = f.Get(p, 4);
    const auto bytes = len(first);
    if (!bytes || !fits(p, bytes + 4, end)) return false;
    const auto params = be(first, 0, 3) & 0xfffe0c;
    const auto second = f.Get(p + bytes, 4);
    const auto more = len(second);
    return more && (be(second, 0, 3) & 0xfffe0c) == params && fits(p + bytes, more, end);
}

bool adts(Reader& f, Offset p, Offset end)
{
    unsigned config = 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (!fits(p, 7, end)) return false;
        const auto h = f.Get(p, 7);
        if (h[0] != 255 || (h[1] & 0xf6) != 0xf0 || ((h[2] >> 2) & 15) > 12) return false;
        const auto length = ((h[3] & 3) << 11) | (h[4] << 3) | (h[5] >> 5);
        const unsigned identity = ((h[1] & 8) << 16) | ((h[2] & 0xfd) << 8) | (h[3] & 0xc0);
        if (length <= (h[1] & 1 ? 7 : 9) || !fits(p, length, end) || (i && config != identity)) return false;
        config = identity;
        p += length;
    }
    return true;
}

bool aiff(Reader& f, Offset p, Offset end)
{
    if (!fits(p, 12, end) || !f.Is(p, "FORM") || (!f.Is(p + 8, "AIFF") && !f.Is(p + 8, "AIFC"))) return false;
    const unsigned size = be(f.Get(p + 4, 4), 0, 4);
    if (size < 4 || !fits(p + 8, size, end)) return false;
    end = p + 8 + size;
    bool comm = false, sound = false;
    p += 12;
    for (unsigned i = 0; p < end; ++i) {
        if (!fits(p, 8, end) || i > 65536) return false;
        const auto v = f.Get(p, 8);
        const Offset n = be(v, 4, 4);
        if (!fits(p + 8, n + n % 2, end)) return false;
        comm |= magic(v, 0, "COMM") && n >= 18;
        sound |= magic(v, 0, "SSND") && n >= 8;
        p += 8 + n + n % 2;
    }
    return comm && sound;
}

bool isoMedia(Reader& f, Offset p, Offset end)
{
    bool fileType = false, media = false;
    for (unsigned i = 0; p < end; ++i) {
        if (i > 65536 || !fits(p, 8, end)) return false;
        const auto v = f.Get(p, 8);
        Offset n = be(v, 0, 4);
        const bool type = magic(v, 4, "ftyp"), content = magic(v, 4, "moov") || magic(v, 4, "mdat");
        unsigned hdr = 8;
        if (n == 1) {
            if (!fits(p, 16, end)) return false;
            auto ext = f.Get(p + 8, 8);
            n = (Offset(be(ext, 0, 4)) << 32) | be(ext, 4, 4);
            hdr = 16;
        } else if (!n) n = end - p;
        if (n < hdr || !fits(p, n, end)) return false;
        if (type) fileType = i == 0 && n >= hdr + 8 && (n - hdr) % 4 == 0;
        media |= content && n > hdr;
        p += n;
    }
    return fileType && media;
}

void classify(Reader& f, bool namedFlac, Inspection& r)
{
    if (!f.size) { add(r, "container.empty", "File contains zero bytes.", 0, 0); return; }
    const auto bounds = tags(f);
    const auto p = bounds.start, end = bounds.end;
    if (p == end) { add(r, "container.no-audio", "Only validated boundary tags are present.", 0, f.size); return; }
    const std::string wanted = namedFlac ? "native FLAC" : "MPEG Layer III";
    auto mismatch = [&](const std::string& observed) {
        add(r, "container.mismatch", "Identified content does not match the filename.", p, end - p, wanted, observed);
    };
    if (f.Is(p, "RIFF") || f.Is(p, "OggS")) {
        const auto outer = f.Is(p, "RIFF") ? wave(f, p, end) : ogg(f, p, end);
        if (!outer) throw Indeterminate("Outer container signature without a validated layout.");
        mismatch(*outer);
        return;
    }
    if (f.Is(p, "FORM")) {
        if (!aiff(f, p, end)) throw Indeterminate("FORM signature without a verified AIFF layout.", p);
        mismatch("AIFF/AIFC");
        return;
    }
    if (fits(p, 8, end) && f.Is(p + 4, "ftyp")) {
        if (!isoMedia(f, p, end)) throw Indeterminate("File-type box without a verified media box layout.", p);
        mismatch("ISO base media");
        return;
    }
    if (f.Is(p, "fLaC")) {
        const auto audio = flacAudio(f, p, end);
        if (!audio) throw Indeterminate("FLAC marker without a complete metadata envelope.");
        if (*audio == end) { add(r, "container.no-audio", "Only native FLAC metadata and optional tags are present.", 0, f.size); return; }
        if (!flacHeader(f, *audio, end)) throw Indeterminate("Native FLAC metadata is not followed by a verified frame header.", *audio);
        if (!namedFlac) { mismatch("native FLAC"); return; }
        if (p) add(r, "flac.leading-data", "Validated external metadata precedes the native FLAC marker.", 0, p);
        r.complete = true;
        return;
    }
    if (candidate(f, p, end, true) || (end != f.size && candidate(f, p, f.size, true))) {
        if (namedFlac) mismatch("MPEG Layer III");
        else r.complete = true;
        return;
    }
    if (otherMpeg(f, p, end)) { mismatch("MPEG Layer I/II"); return; }
    if (adts(f, p, end)) { mismatch("ADTS AAC"); return; }
    if (const auto h = header(f.Get(p, 4)); h && !h->bytes)
        throw Indeterminate("Possible free-format Layer III; native boundaries cannot be independently confirmed.");
    if (f.Is(p, "PK"))
        throw Indeterminate("Possible outer container; embedded signatures cannot establish native audio.");
    // Never dispatch a writer on unidentified prefix bytes, even if a native stream is found inside.
    if (namedFlac) {
        const auto limit = p + std::min(searchLimit, end - p);
        unsigned candidates = 0;
        for (Offset pos = p; pos < limit;) {
            const auto marker = f.Find(pos, limit, "fLaC");
            if (!marker) break;
            const Offset at = *marker;
            pos = at + 4;
            if (++candidates > 16) throw Indeterminate("Too many candidate FLAC markers.");
            const auto audio = flacAudio(f, at, end);
            if (audio && *audio < end && flacRun(f, *audio, end)) {
                add(r, "flac.leading-data", "Unidentified prefix before native metadata and a corroborated FLAC frame.", 0, at);
                return;
            }
        }
    }
    throw Indeterminate("Native stream not established; unknown bytes do not prove absence of audio.");
}
}

Inspection ScanMp3(const std::filesystem::path& path)
{
    Reader f(path);
    Inspection r;
    try { walk(f, r); }
    catch (const Indeterminate& e) { r.issues.push_back({"mp3.unsupported-stream", e.what(), e.offset, e.length, {}, {}, false, {}}); }
    return r;
}

Inspection ScanContainer(const std::filesystem::path& path, bool namedFlac)
{
    Reader f(path);
    Inspection r;
    try { classify(f, namedFlac, r); }
    catch (const Indeterminate& e) { r.issues.push_back({"container.unsupported", e.what(), e.offset, e.length, {}, {}, false, {}}); }
    return r;
}
}
