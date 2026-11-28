#include "RepairIntegrity.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
using namespace RepairIntegrity;
namespace fs = std::filesystem;

struct Fixtures
{
    fs::path dir, path;
    Fixtures()
    {
        dir = fs::current_path() / ("mp3-scanner-fixtures-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!fs::create_directory(dir)) throw std::runtime_error("Cannot create unique fixture directory.");
        path = dir / "generated.audio";
    }
    ~Fixtures() { std::error_code ec; fs::remove_all(dir, ec); }
    Inspection Check(const Bytes& data, std::optional<bool> flac = {})
    {
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out.write(reinterpret_cast<const char*>(data.data()), data.size())) throw std::runtime_error("Fixture write failed.");
        }
        auto result = flac ? ScanContainer(path, *flac) : ScanMp3(path);
        std::ifstream in(path, std::ios::binary);
        if (Bytes(std::istreambuf_iterator<char>(in), {}) != data ||
            std::distance(fs::directory_iterator(dir), fs::directory_iterator{}) != 1)
            throw std::runtime_error("The scanner changed the fixture tree.");
        for (const auto& issue : result.issues) if (issue.patch) throw std::runtime_error("Unexpected patch.");
        return result;
    }
};

Bytes join(Bytes a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); return a; }
void put(Bytes& b, std::size_t p, std::string_view s) { std::copy(s.begin(), s.end(), b.begin() + p); }
void set(Bytes& b, std::size_t p, unsigned n, unsigned value)
{
    while (n) { b[p + --n] = static_cast<std::uint8_t>(value); value >>= 8; }
}
unsigned crc(const Bytes& b, std::size_t n, unsigned poly, unsigned width, bool reflected)
{
    unsigned sum = 0;
    for (std::size_t i = 0; i < n; ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            const auto v = (b[i] >> (reflected ? bit : 7 - bit)) & 1;
            if (reflected) sum = (sum >> 1) ^ (((sum ^ v) & 1) ? poly : 0);
            else sum = (sum << 1) ^ ((((sum >> (width - 1)) ^ v) & 1) ? poly : 0);
        }
    }
    return width == 32 ? sum : sum & ((1u << width) - 1);
}
void clean(const Inspection& r)
{
    if (!r.complete || !r.issues.empty()) throw std::runtime_error(r.issues.empty() ? "Scan incomplete." :
        "Unexpected " + r.issues.front().rule + ": " + r.issues.front().detail);
}
Issue expect(const Inspection& r, std::string_view rule, std::size_t offset = SIZE_MAX)
{
    for (const auto& i : r.issues) if (i.rule == rule && (offset == SIZE_MAX || i.offset == offset)) return i;
    throw std::runtime_error("Missing " + std::string(rule) + (r.issues.empty() ? " (no findings)" : " (first: " + r.issues.front().detail + ")"));
}
void absent(const Inspection& r, std::string_view rule)
{
    for (const auto& i : r.issues) if (i.rule == rule) throw std::runtime_error("Unexpected " + std::string(rule));
}
Bytes frame(unsigned version = 3, unsigned br = 9, unsigned mode = 0, bool padded = false)
{
    // At 44.1 kHz/22.05 kHz/11.025 kHz: index 9 is 128 (v1) or 80 (LSF) kb/s; index 8 is 112 or 64.
    unsigned size = version == 3 ? (br == 9 ? 417 : 365) : version == 2 ? (br == 9 ? 261 : 208) : (br == 9 ? 522 : 417);
    Bytes b(size + padded);
    b[0] = 255; b[1] = static_cast<std::uint8_t>(0xe3 | (version << 3));
    b[2] = static_cast<std::uint8_t>((br << 4) + (padded ? 2 : 0)); b[3] = static_cast<std::uint8_t>(mode << 6);
    return b;
}
Bytes id3(bool footer = false)
{
    Bytes b(20, 0);
    put(b, 0, "ID3"); b[3] = 4;
    if (footer) { b[5] = 16; put(b, 10, "3DI"); b[13] = 4; b[15] = 16; return b; }
    // A real CHAP frame envelope; chapter data, including absolute offsets, must never be treated as MPEG.
    b[9] = 29; put(b, 10, "CHAP"); b[17] = 19;
    b.resize(39); put(b, 20, "ch"); return b;
}
Bytes trailer()
{
    Bytes ape(32); put(ape, 0, "APETAGEX"); ape[8] = 208; ape[9] = 7; ape[12] = 32;
    Bytes v1(128); put(v1, 0, "TAG"); return join(join(id3(true), ape), v1);
}
Bytes flac()
{
    Bytes b(42); put(b, 0, "fLaC"); b[4] = 128; b[7] = 34;
    b[9] = 16; b[11] = 16; b[18] = 10; b[19] = 196; b[20] = 64; b[21] = 240; b[25] = 16;
    Bytes fr{255,248,0x69,8,0,15}; fr.push_back(static_cast<std::uint8_t>(crc(fr, fr.size(), 7, 8, false)));
    fr.insert(fr.end(), 3, 0); auto footer = crc(fr, fr.size(), 0x8005, 16, false);
    fr.resize(fr.size() + 2); set(fr, fr.size() - 2, 2, footer);
    return join(b, fr);
}

void frames(Fixtures& f)
{
    auto one = frame();
    const auto two = join(one, one);
    for (unsigned v : {3u,2u,0u}) for (unsigned m : {0u,3u}) {
        auto a = frame(v, 9, m), b = frame(v, 9, m, true);
        clean(f.Check(join(join(id3(), join(a, b)), trailer())));
    }
    auto legal = f.Check(join(one, frame(3, 8, 1)));
    if (!expect(legal, "mp3.stream-parameters", one.size()).informational ||
        !expect(legal, "mp3.no-vbr-header", 0).informational) throw std::runtime_error("Legal variation labeled corruption.");
    auto change = f.Check(join(one, frame(2, 9, 3)));
    if (expect(change, "mp3.stream-parameters").informational) throw std::runtime_error("Undisclosed parameter change.");
    auto prefix = Bytes(70000, 0x42);
    expect(f.Check(join(prefix, two)), "mp3.leading-data", 0);
    expect(f.Check(join(two, Bytes{1,2,3})), "mp3.trailing-data", two.size());
    auto trunc = one; trunc.resize(80);
    expect(f.Check(join(two, trunc)), "mp3.truncated-frame", two.size());
    auto gap = join(join(two, Bytes{1,2,3}), two);
    expect(f.Check(gap), "mp3.lost-sync", two.size());
    auto back = one; back[4] = 255; back[5] = 128;
    expect(f.Check(join(one, back)), "mp3.lost-sync", one.size() + 4);
    expect(f.Check(join(join(one, id3()), two)), "mp3.embedded-id3", one.size());
    auto free = one; free[2] = 0;
    expect(f.Check(free), "mp3.unsupported-stream");
    auto protectedFrame = one; protectedFrame[1] &= 0xfe;
    // Independent check value for header suffix 90 00 and 32 zero side-info bytes, init FFFF.
    unsigned a = 0xffff;
    for (unsigned value : join(Bytes{0x90,0}, Bytes(32))) for (int i = 7; i >= 0; --i) {
        const auto top = (a >> 15) ^ (value >> i); a = ((a << 1) ^ ((top & 1) ? 0x8005 : 0)) & 65535;
    }
    set(protectedFrame, 4, 2, a);
    clean(f.Check(join(protectedFrame, protectedFrame)));
    protectedFrame[50] = 99; clean(f.Check(join(protectedFrame, one)));
    protectedFrame[5] ^= 1;
    expect(f.Check(join(protectedFrame, one)), "mp3.frame-crc", 4);
    put(one, one.size() - 128, "TAG"); clean(f.Check(one));
    Bytes id3v1(128); put(id3v1, 0, "TAG"); clean(f.Check(join(one, id3v1)));
    put(one, one.size() - 10, "3DI"); clean(f.Check(one));
    Bytes lyrics(26); put(lyrics, 0, "LYRICSBEGIN000011LYRICS200");
    clean(f.Check(join(two, join(lyrics, id3v1))));
    Bytes ape(32); put(ape, 0, "APETAGEX"); ape[8] = 208; ape[9] = 7; ape[12] = 32; ape[23] = 128;
    auto head = ape; head[23] |= 32;
    clean(f.Check(join(join(head, ape), two))); clean(f.Check(join(two, join(head, ape))));
}

void seek(Fixtures& f)
{
    const auto music = join(frame(), frame(3, 8));
    auto c = frame();
    put(c, 36, "Xing"); c[43] = 15; set(c, 44, 4, 2); set(c, 48, 4, static_cast<unsigned>(c.size() + music.size()));
    for (unsigned i = 0; i < 100; ++i) c[52 + i] = static_cast<std::uint8_t>(i / 5 * 10);
    put(c, 156, "LAME3.100"); c[165] = 4;
    set(c, 188, 2, crc(music, music.size(), 0xa001, 16, true));
    set(c, 190, 2, crc(c, 190, 0xa001, 16, true));
    auto file = [&](const Bytes& tag) { return join(join(id3(), join(tag, music)), trailer()); };
    clean(f.Check(file(c)));
    expect(f.Check(join(join(c, music), join(c, music))), "mp3.unsupported-stream", c.size() + music.size());
    auto bad = c;
    bad[47]++; bad[51]++; bad[145] = 0; bad[188]++; bad[190]++;
    auto r = f.Check(file(bad));
    expect(r, "mp3.xing-counts", id3().size() + 44); expect(r, "mp3.xing-counts", id3().size() + 48);
    expect(r, "mp3.xing-toc", id3().size() + 52); expect(r, "mp3.lame-music-crc", id3().size() + 188);
    expect(r, "mp3.lame-tag-crc", id3().size() + 190);
    bad = c; put(bad, 156, "Lavf"); bad[188]++; bad[190]++;
    clean(f.Check(file(bad)));
    bad = c; put(bad, 156, "????"); bad[47]++;
    r = f.Check(file(bad)); absent(r, "mp3.xing-counts"); expect(r, "mp3.unsupported-stream");
    bad = frame(); put(bad, 36, "Info"); bad[43] = 1; bad[47] = 2;
    clean(f.Check(file(bad)));
    auto small = frame(3, 8); small[2] = 0x10; small.resize(104); put(small, 36, "Xing"); small[43] = 4;
    expect(f.Check(file(small)), "mp3.xing-toc");
    auto vb = frame(); put(vb, 36, "VBRI"); vb[41] = 1;
    set(vb, 46, 4, static_cast<unsigned>(music.size())); vb[53] = 2; vb[55] = 2; vb[57] = 1; vb[59] = 2; vb[61] = 1;
    set(vb, 62, 2, 417); set(vb, 64, 2, 365); clean(f.Check(file(vb)));
    vb[51]++; vb[47]++; vb[64]++; r = f.Check(file(vb));
    expect(r, "mp3.vbri-counts", id3().size() + 46); expect(r, "mp3.vbri-counts", id3().size() + 50);
    expect(r, "mp3.vbri-toc", id3().size() + 62);
    vb[59] = 5; expect(f.Check(file(vb)), "mp3.vbri-toc", id3().size() + 54);
}

void containers(Fixtures& f)
{
    const auto mp3 = join(frame(), frame()), native = flac(), prefix = id3();
    clean(f.Check(join(prefix, mp3), false)); clean(f.Check(native, true));
    expect(f.Check(mp3, true), "container.mismatch"); expect(f.Check(native, false), "container.mismatch");
    auto r = f.Check(join(prefix, native), true);
    if (!r.complete || expect(r, "flac.leading-data", 0).length != prefix.size()) throw std::runtime_error("Leading tag lost.");
    r = f.Check(join(Bytes{1,2,3}, native), true);
    if (r.complete || expect(r, "flac.leading-data", 0).length != 3) throw std::runtime_error("Opaque prefix accepted.");
    expect(f.Check({}, false), "container.empty"); expect(f.Check(join(prefix, trailer()), false), "container.no-audio");
    expect(f.Check(Bytes(native.begin(), native.begin() + 42), true), "container.no-audio");
    r = f.Check(Bytes{1,2,3}, true); expect(r, "container.unsupported"); absent(r, "container.no-audio");
    auto malformed = prefix; malformed[8] = 128;
    expect(f.Check(malformed, false), "container.unsupported");
    Bytes wav(46); put(wav, 0, "RIFF"); wav[4] = 38; put(wav, 8, "WAVEfmt "); wav[16] = 16; wav[20] = 1;
    wav[22] = 1; wav[24] = 0x44; wav[25] = 0xac; put(wav, 36, "data"); wav[40] = 2;
    expect(f.Check(wav, true), "container.mismatch");
    Bytes ogg(28 + 51); put(ogg, 0, "OggS"); ogg[5] = 2; ogg[26] = 1; ogg[27] = 51;
    ogg[28] = 0x7f; put(ogg, 29, "FLAC"); ogg[33] = 1; std::copy_n(native.begin(), 42, ogg.begin() + 37);
    const auto sum = crc(ogg, ogg.size(), 0x04c11db7, 32, false);
    for (int i = 0; i < 4; ++i) ogg[22 + i] = static_cast<std::uint8_t>(sum >> (8 * i));
    if (expect(f.Check(ogg, true), "container.mismatch").observed != "Ogg FLAC") throw std::runtime_error("Ogg FLAC not identified.");
    ogg[29] = 'Q'; expect(f.Check(ogg, false), "container.unsupported");
    Bytes iso(25); set(iso, 0, 4, 16); put(iso, 4, "ftypM4A "); set(iso, 16, 4, 9); put(iso, 20, "mdat");
    expect(f.Check(iso, false), "container.mismatch");
    Bytes aiff(54); put(aiff, 0, "FORM"); aiff[7] = 46; put(aiff, 8, "AIFFCOMM"); aiff[19] = 18;
    put(aiff, 38, "SSND"); aiff[45] = 8; expect(f.Check(aiff, false), "container.mismatch");
    Bytes aac{255,0xf1,0x50,0x80,1,0x1f,0xfc,0};
    expect(f.Check(join(aac, aac), false), "container.mismatch");
    auto falseFlac = native; std::fill(falseFlac.begin() + 8, falseFlac.end(), 0);
    expect(f.Check(falseFlac, true), "container.unsupported");
}
}

int main()
{
    try { Fixtures f; frames(f); seek(f); containers(f); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
