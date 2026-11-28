#include "AudioRepair.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace
{
using namespace RepairIntegrity;
using U64 = std::uint64_t;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void put(Bytes& bytes, size_t offset, size_t width, U64 value)
{
    while (width) { bytes[offset + --width] = static_cast<std::uint8_t>(value); value >>= 8; }
}

unsigned crc(const Bytes& data, unsigned width, unsigned poly)
{
    unsigned value = 0;
    for (auto byte : data)
        for (int bit = 7; bit >= 0; --bit)
        {
            const auto carry = ((value >> (width - 1)) ^ (byte >> bit)) & 1;
            value = (value << 1) ^ (carry ? poly : 0);
        }
    return value & ((1u << width) - 1);
}

Bytes frame(unsigned block, unsigned number, bool variable, Bytes payload, unsigned encoding = 0x0c)
{
    Bytes data{0xff, static_cast<std::uint8_t>(variable ? 0xf9 : 0xf8), 0x6b, static_cast<std::uint8_t>(encoding),
        static_cast<std::uint8_t>(number), static_cast<std::uint8_t>(block - 1)};
    data.push_back(static_cast<std::uint8_t>(crc(data, 8, 7)));
    data.insert(data.end(), payload.begin(), payload.end());
    unsigned footer = crc(data, 16, 0x8005);
    data.push_back(static_cast<std::uint8_t>(footer >> 8));
    data.push_back(static_cast<std::uint8_t>(footer));
    return data;
}

Bytes stream(const std::vector<Bytes>& frames, unsigned samples, unsigned minimum, unsigned maximum)
{
    Bytes data{'f','L','a','C',0x80,0,0,34};
    data.resize(42);
    put(data, 8, 2, minimum);
    put(data, 10, 2, maximum);
    put(data, 18, 8, (U64{96000} << 44) | (U64{23} << 36) | samples);
    // Opaque nonzero original witness: the structural scanner must never attempt to decode/authenticate it.
    data[26] = 0x80;
    for (const auto& f : frames) data.insert(data.end(), f.begin(), f.end());
    return data;
}

struct Fixture
{
    std::filesystem::path dir = "flac-scanner-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    Fixture() { require(std::filesystem::create_directory(dir), "Fixture path collision"); }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
    Inspection scan(const Bytes& data)
    {
        const auto file = dir / "generated.flac";
        {
            std::ofstream out(file, std::ios::binary);
            require(static_cast<bool>(out.write(reinterpret_cast<const char*>(data.data()), data.size())), "Fixture write");
        }
        auto result = ScanFlac(file);
        std::ifstream check(file, std::ios::binary);
        Bytes actual{std::istreambuf_iterator<char>(check), {}};
        require(actual == data && std::distance(std::filesystem::directory_iterator(dir), {}) == 1, "Source tree changed");
        return result;
    }
};

const Issue& find(const Inspection& result, std::string_view rule)
{
    auto it = std::find_if(result.issues.begin(), result.issues.end(), [&](const auto& f) {return f.rule == rule;});
    if (it == result.issues.end()) throw std::runtime_error("Missing finding: " + std::string(rule));
    return *it;
}

void trailing(Fixture& fixture)
{
    auto first = frame(32, 0, false, {0, 0xff, 0xf8, 0});
    Bytes last;
    for (unsigned sample = 0; sample <= 65535; ++sample)
    {
        last = frame(5, 1, false, {0, 0, static_cast<std::uint8_t>(sample >> 8), static_cast<std::uint8_t>(sample)});
        if (last[last.size() - 2] == 0 && last.back() == 0) break;
    }
    require(last[last.size() - 2] == 0 && last.back() == 0, "Test needs frame ending in zero CRC bytes");
    const auto pristine = stream({first, last}, 37, 32, 32);
    auto clean = fixture.scan(pristine);
    require(clean.complete && clean.samples == 37 && clean.issues.empty(), "Valid final frame must be retained");
    for (bool tagged : {false, true})
    {
        auto data = pristine;
        data.resize(data.size() + 4186);
        if (tagged)
        {
            Bytes tag(128, 0); tag[0] = 'T'; tag[1] = 'A'; tag[2] = 'G'; tag[3] = 'X';
            data.insert(data.end(), tag.begin(), tag.end());
        }
        auto result = fixture.scan(data);
        auto issue = find(result, "flac.trailing-data");
        require(result.complete && result.samples == 37 && issue.patch && issue.offset == pristine.size() &&
            issue.patch->offset == issue.offset && issue.length == 4186 && issue.patch->removeSize == 4186 &&
            issue.patch->before.empty() && issue.patch->after.empty(), "Wrong suffix removal interval");
        require(issue.observed.find("=0x0000") != std::string::npos, "Missing final CRC evidence");
        data[pristine.size() + 1] = 'X';
        auto unknown = fixture.scan(data);
        require(!unknown.complete && !find(unknown, "flac.trailing-data").patch, "Nonzero trailer was trusted");
    }
    auto noCount = pristine; noCount[25] = 0; noCount.resize(noCount.size() + 2);
    auto ambiguous = fixture.scan(noCount);
    require(!ambiguous.complete && !find(ambiguous, "flac.trailing-data").patch, "Missing sample-count witness");

    // A headerless APE item with a zero-length value starts with eight zero bytes.
    // They belong to the tag, not to the immediately preceding fill.
    auto ape = pristine;
    ape.resize(ape.size() + 10);
    Bytes item(8); item.insert(item.end(), {'A','A',0});
    ape.insert(ape.end(), item.begin(), item.end());
    Bytes footer{'A','P','E','T','A','G','E','X',0xd0,7,0,0,43,0,0,0,1};
    footer.resize(32);
    ape.insert(ape.end(), footer.begin(), footer.end());
    auto tagged = fixture.scan(ape);
    const auto& apeFill = find(tagged, "flac.trailing-data").patch;
    require(tagged.complete && apeFill && apeFill->removeSize == 10, "APE item mistaken for fill");
    ape[ape.size() - 20] = 42;
    auto malformed = fixture.scan(ape);
    require(!malformed.complete && !find(malformed, "flac.trailing-data").patch, "Malformed APE layout trusted");

    Bytes id3{'I','D','3',4,0,16,0,0,0,12, 'T','I','T','2',0,0,0,2,0,0,3,'X',
        '3','D','I',4,0,16,0,0,0,12};
    auto appended = pristine;
    appended.resize(appended.size() + 10);
    appended.insert(appended.end(), id3.begin(), id3.end());
    auto retained = fixture.scan(appended);
    const auto& id3Fill = find(retained, "flac.trailing-data").patch;
    require(retained.complete && id3Fill && id3Fill->removeSize == 10, "Appended ID3v2 footer lost");
    auto prefixed = pristine;
    prefixed.insert(prefixed.begin(), id3.begin(), id3.end());
    require(fixture.scan(prefixed).issues.empty(), "Fully delimited leading ID3 refused");
    prefixed[17] = 127;
    require(!fixture.scan(prefixed).complete, "Unverified leading tag framing accepted");
}

void fields(Fixture& fixture)
{
    const auto a = frame(32, 0, false, {0, 0, 0, 0}), b = frame(7, 1, false, {0, 0, 0, 0});
    auto pristine = stream({a, b}, 39, 32, 32);
    pristine[4] = 0;
    Bytes seek{0x83,0,0,54}; seek.resize(58);
    put(seek, 20, 2, 32);
    put(seek, 22, 8, 32); put(seek, 30, 8, a.size()); put(seek, 38, 2, 7);
    std::fill(seek.begin() + 40, seek.end(), 0xff);
    pristine.insert(pristine.begin() + 42, seek.begin(), seek.end());
    put(pristine, 12, 3, a.size()); put(pristine, 15, 3, b.size());
    require(fixture.scan(pristine).issues.empty(), "Valid exact STREAMINFO/seek targets rejected");
    auto data = pristine;
    put(data, 8, 2, 16); put(data, 10, 2, 128); put(data, 12, 3, 10); put(data, 15, 3, 999);
    put(data, 42 + 30, 8, a.size() - 1); put(data, 42 + 38, 2, 32);
    data[25] = 48;
    auto result = fixture.scan(data);
    require(result.complete && result.samples == 39 && result.declaredSamples == 48 &&
        result.issues.size() == 7, "Wrong number of independent field repairs");
    for (const auto& issue : result.issues)
    {
        require(issue.patch && !issue.patch->removeSize && issue.patch->before.size() == issue.length &&
            issue.patch->before.size() == issue.patch->after.size(), "Invalid fixed-width repair");
        const auto& p = *issue.patch;
        require(std::equal(p.before.begin(), p.before.end(), data.begin() + p.offset), "Incorrect before-image");
        std::copy(p.after.begin(), p.after.end(), data.begin() + p.offset);
    }
    require(data == pristine, "Field patches changed bytes outside declared corrections");

    auto missing = pristine;
    missing[26] = 0;
    auto fill = fixture.scan(missing);
    auto candidate = find(fill, "flac.fill-md5");
    require(fill.complete && candidate.patch && candidate.offset == 26 && candidate.patch->before == Bytes(16) &&
        candidate.patch->after == Bytes(16), "Missing digest did not yield an exact placeholder");
    missing[25] = 50;
    auto untrusted = fixture.scan(missing);
    require(!find(untrusted, "flac.total-samples").patch && !find(untrusted, "flac.fill-md5").patch,
        "Manufactured witnesses for possible whole-frame truncation");
    put(pristine, 42 + 22, 8, 34);
    require(!find(fixture.scan(pristine), "flac.seek-target").patch, "Inferred a new seek sample");
    put(pristine, 42 + 22, 8, 0);
    require(!find(fixture.scan(pristine), "flac.seek-target").patch, "Edited a malformed table");

    auto var = stream({frame(32, 0, true, {0,0,0,0}), frame(48, 32, true, {0,0,0,0}),
        frame(3, 80, true, {0,0,0,0})}, 0, 16, 512);
    require(fixture.scan(var).issues.empty(), "Legal conservative bounds, unknown count or short last block rejected");
    put(var, 8, 2, 40);
    auto corrected = fixture.scan(var);
    require(corrected.issues.size() == 1 && corrected.issues[0].patch &&
        corrected.issues[0].patch->after == Bytes({0,32}), "Rewrote a legal variable bound");
}

void brokenFrames(Fixture& fixture)
{
    const auto a = frame(16, 0, false, {0, 0xff, 0xf8, 0});
    const auto b = frame(11, 1, false, {0, 0, 0, 0});
    auto data = stream({a,b}, 27, 16, 16);
    data[42 + 6] ^= 0x80;
    auto header = fixture.scan(data);
    require(!header.complete && find(header, "flac.header-crc").offset == 48, "Header CRC mismatch missed");
    data[48] ^= 0x80;
    data.back() ^= 0x80;
    auto footer = fixture.scan(data);
    require(!footer.complete && find(footer, "flac.frame-crc").offset == data.size() - 2, "Frame CRC mismatch missed");
    data.pop_back();
    auto truncated = fixture.scan(data);
    require(!truncated.complete && find(truncated, "flac.truncated-frame").offset == 42 + a.size(), "Truncated footer accepted");
    Bytes tag(128); tag[0] = 'T'; tag[1] = 'A'; tag[2] = 'G';
    data.insert(data.end(), tag.begin(), tag.end());
    auto cutByTag = fixture.scan(data);
    require(!cutByTag.complete && find(cutByTag, "flac.truncated-frame").length == b.size() - 1, "ID3 bytes supplied a missing footer");
    auto gap = stream({a, Bytes{0,0,0}, b}, 27, 16, 16);
    auto lost = fixture.scan(gap);
    require(!lost.complete && find(lost, "flac.lost-sync").offset == 42 + a.size() &&
        find(lost, "flac.lost-sync").length == 3, "Internal gap treated as fill");
    auto number = fixture.scan(stream({a,frame(11, 0, false, {0,0,0,0})}, 27, 16, 16));
    require(!number.complete && find(number, "flac.number-discontinuity").expected == "1", "Repeated frame number missed");
    auto strategy = fixture.scan(stream({a,frame(11, 16, true, {0,0,0,0})}, 27, 16, 16));
    require(!strategy.complete && find(strategy, "flac.number-discontinuity").observed == "variable", "Strategy change missed");
}

struct BitWriter
{
    Bytes bytes;
    unsigned used = 0;
    void put(unsigned width, unsigned value)
    {
        while (width)
        {
            if (!used) bytes.push_back(0);
            unsigned bit = --width >= 32 ? 0 : (value >> width) & 1;
            bytes.back() |= static_cast<std::uint8_t>(bit << (7 - used));
            used = (used + 1) % 8;
        }
    }
};

void subframes(Fixture& fixture)
{
    // Left/side, odd source depth: non-byte-aligned LPC(1), two Rice2 partitions
    // (ordinary then escaped), and a verbatim channel with one wasted bit.
    BitWriter p;
    p.put(8, 64); p.put(13, 1); // LPC order 1, one warm-up
    p.put(4, 1); p.put(5, 0); p.put(2, 1); // coefficient precision 2, shift 0
    p.put(2, 1); p.put(4, 1); // Rice2, 2 partitions
    p.put(5, 1);
    for (int i = 0; i < 7; ++i) { p.put(3, 1); p.put(1, i & 1); }
    p.put(5, 31); p.put(5, 0); // escaped eight zero-width residuals
    p.put(8, 3); p.put(1, 1); // verbatim with one wasted bit (14 - 1 effective bits)
    for (int i = 0; i < 16; ++i) p.put(13, (i & 1) ? 0x1ff8 : 0x1fff);
    require(p.used != 0, "Test needs alignment padding");
    auto make = [&](Bytes payload) {
        auto data = stream({frame(16, 0, false, payload, 0x80)}, 16, 16, 16);
        put(data, 18, 8, (U64{96000} << 44) | (U64{1} << 41) | (U64{12} << 36) | 16);
        return data;
    };
    auto good = fixture.scan(make(p.bytes));
    require(good.complete && good.issues.empty() && good.bits == 13, "LPC/Rice2/escaped/side/wasted syntax rejected");
    p.bytes.back() |= 1;
    auto badPad = fixture.scan(make(p.bytes));
    require(!badPad.complete && !find(badPad, "flac.unsupported-stream").patch, "Nonzero alignment padding accepted");

    BitWriter f;
    f.put(8, 16); f.put(2, 0); f.put(4, 0); f.put(4, 2); // fixed predictor 0, Rice parameter 2
    for (int i = 0; i < 16; ++i) { f.put(1, 1); f.put(2, i & 3); }
    require(fixture.scan(stream({frame(16, 0, false, f.bytes)}, 16, 16, 16)).issues.empty(), "Fixed predictor rejected");
}

void extendedHeaders(Fixture& fixture)
{
    auto coded = [](Bytes number, bool variable, Bytes extra) {
        Bytes frame{0xff, static_cast<std::uint8_t>(variable ? 0xf9 : 0xf8), 0x7e, 0x0c};
        frame.insert(frame.end(), number.begin(), number.end());
        frame.insert(frame.end(), extra.begin(), extra.end());
        frame.push_back(static_cast<std::uint8_t>(crc(frame, 8, 7)));
        frame.insert(frame.end(), {0,0,0,0});
        auto sum = crc(frame, 16, 0x8005);
        frame.push_back(static_cast<std::uint8_t>(sum >> 8)); frame.push_back(static_cast<std::uint8_t>(sum));
        return frame;
    };
    const Bytes extra{0,127,0x25,0x80}; // both 16-bit fields: 128 samples, 9600 tens of Hz
    auto first = coded({0}, true, extra), second = coded({0xc2,0x80}, true, {0,0,0x25,0x80});
    auto clean = fixture.scan(stream({first,second}, 129, 16, 256));
    require(clean.complete && clean.issues.empty(), "Extended sample number, block or rate rejected");
    auto overlong = fixture.scan(stream({coded({0xc0,0x80}, true, extra)}, 128, 16, 256));
    require(!overlong.complete && !find(overlong, "flac.unsupported-stream").patch, "Overlong zero accepted");
    auto wide = fixture.scan(stream({coded({0xfe,0xbf,0xbf,0xbf,0xbf,0xbf,0xbf}, true, extra)}, 128, 16, 256));
    require(find(wide, "flac.number-discontinuity").observed == "68719476735", "36-bit sample number truncated");
    auto forbidden = fixture.scan(stream({coded({0xfe,0xbf,0xbf,0xbf,0xbf,0xbf,0xbf}, false, extra)}, 128, 128, 128));
    require(!forbidden.complete && !find(forbidden, "flac.unsupported-stream").patch, "Seven-byte fixed frame number accepted");
}

void save(const std::filesystem::path& path, const Bytes& data)
{
    std::ofstream out(path, std::ios::binary);
    require(static_cast<bool>(out.write(reinterpret_cast<const char*>(data.data()), data.size())), "Cannot save test file");
}

Bytes load(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    require(static_cast<bool>(in), "Missing test file");
    return {std::istreambuf_iterator<char>(in), {}};
}

void publicationGuards(Fixture& fixture)
{
    namespace fs = std::filesystem;
    const auto root = fs::absolute(fixture.dir), library = root / "library", source = library / "a.flac";
    fs::create_directory(library);
    const Bytes companion{'a','r','t'};
    save(library / "z.txt", companion);
    auto unchanged = [&](const Bytes& expected) {
        require(load(source) == expected && load(library / "z.txt") == companion &&
            std::distance(fs::recursive_directory_iterator(library), {}) == 2, "Repair modified generated source tree");
    };
    const auto a = frame(16, 0, false, {0,0,0,0}), b = frame(5, 1, false, {0,0,0,0});
    auto base = stream({a,b}, 21, 16, 16);
    // MD5 of 63 zero bytes: 21 mono samples at 24 bits.
    put(base, 26, 8, 0x65cecfb980d72fdeULL); put(base, 34, 8, 0x57d175d6ec1c3f64ULL);
    put(base, 12, 3, 9); // deliberately incorrect minimum frame length
    base[4] = 0;
    base.insert(base.begin() + 42, {0x82,0,0,5,'T','E','S','T',0x11});
    constexpr size_t appPayload = 50;
    auto padded = base;
    padded.resize(padded.size() + 16);
    Bytes tag(128); tag[0] = 'T'; tag[1] = 'A'; tag[2] = 'G'; tag[3] = 'X';
    padded.insert(padded.end(), tag.begin(), tag.end());
    save(source, padded);
    Categories categories;
    categories.metadata = Mode::Off; categories.integrity = Mode::Fix;
    const std::set<std::string> both{"flac.frame-sizes","flac.trailing-data"};
    auto analysis = RepairRules::AnalyzeSelected(source, both, categories);
    require(analysis.edit && analysis.edit->stream && analysis.edit->stream->patches.size() == 2,
        "Expected a real two-span integrity edit");
    Bytes written;
    RepairRules::Write(source, *analysis.edit, [&](const char* chunk, size_t n) {
        written.insert(written.end(), chunk, chunk + n);
    });
    auto expected = base;
    put(expected, 12, 3, a.size()); expected.insert(expected.end(), tag.begin(), tag.end());
    require(written == expected, "Writer did not preserve the mapped non-edit spans");
    auto staged = root / "staged.flac";
    save(staged, written);
    std::string error;
    require(RepairRules::Verify(source, staged, *analysis.edit, both, error), "Correct staged layout failed verification");
    for (size_t byte : {appPayload, size_t{14}, written.size() - 1})
    {
        auto tampered = written;
        if (byte == 14) tampered[byte] = 0; // legal unknown minimum, but not the authorized replacement
        else tampered[byte] ^= 1;
        save(staged, tampered);
        auto walk = ScanFlac(staged);
        require(walk.complete && walk.issues.empty(), "Tamper case must pass frame reinspection by itself");
        error.clear();
        require(!RepairRules::Verify(source, staged, *analysis.edit, both, error) && !error.empty(),
            "Verifier accepted an undeclared changed byte");
        unchanged(padded);
    }

    // Replace opaque application metadata between prepare and execution. Both versions decode
    // identically and need the same one-field repair; length, mtime and edit bytes do not help.
    save(source, base);
    const auto dest = root / "stale-output";
    fs::create_directory(dest);
    const Bytes sentinel{'p','r','i','o','r'};
    save(dest / "a.flac", sentinel);
    const std::set<std::string> sizeOnly{"flac.frame-sizes"};
    AudioRepair::Plan old;
    require(AudioRepair::Prepare(library, dest, true, sizeOnly, old, 1, false, categories) == 0 &&
        old.conflicts.empty() && old.findings.size() == 1 && old.files.size() == 2 &&
        old.files.front().repair, "Expected one prepared repair and one companion");
    unchanged(base);
    const auto mtime = fs::last_write_time(source);
    auto changed = base; changed[appPayload] ^= 1;
    save(source, changed); fs::last_write_time(source, mtime);
    auto current = RepairRules::AnalyzeSelected(source, sizeOnly, categories);
    require(fs::file_size(source) == old.files.front().size && fs::last_write_time(source) == old.files.front().modified &&
        current.findings.size() == 1 && current.findings.front().rule == old.findings.front().rule &&
        current.findings.front().count == old.findings.front().count && current.edit && current.edit->stream &&
        RepairRules::EditDigest(*current.edit->stream) == old.files.front().editDigest,
        "Adversarial replacement unexpectedly changed preview evidence other than source content");
    size_t repaired = 0, copied = 0;
    require(AudioRepair::Execute(old, &repaired, &copied) != 0 && !repaired && !copied &&
        old.files.front().status == "failed" && old.files.front().proof.error.find("changed") != std::string::npos &&
        load(dest / "a.flac") == sentinel && !fs::exists(dest / "z.txt"), "Stale plan replaced an existing destination");
    unchanged(changed);
    const auto freshDest = root / "fresh-output";
    AudioRepair::Plan fresh;
    require(AudioRepair::Prepare(library, freshDest, false, sizeOnly, fresh, 1, false, categories) == 0 &&
        AudioRepair::Execute(fresh, &repaired, &copied) == 0 && repaired == 1 && copied == 1,
        "Fresh plan for the same valid replacement did not install");
    auto fixed = changed; put(fixed, 12, 3, a.size());
    require(load(freshDest / "a.flac") == fixed && load(freshDest / "z.txt") == companion,
        "Freshly verified publication differs from its declared edit");
    unchanged(changed);
}
}

int main()
{
    try { Fixture fixture; trailing(fixture); fields(fixture); brokenFrames(fixture); subframes(fixture); extendedHeaders(fixture); publicationGuards(fixture); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
