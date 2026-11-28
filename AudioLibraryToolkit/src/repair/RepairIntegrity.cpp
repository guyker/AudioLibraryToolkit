#include "RepairRules.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <stdexcept>
extern "C" {
#include <libavutil/hash.h>
}

namespace RepairIntegrity
{
bool Categories::Legacy() const
{
    return metadata == Mode::Fix && integrity == Mode::Off && container == Mode::Off && decode == Mode::Off;
}

bool ParseMode(std::string_view text, Mode& mode)
{
    if (text == "off") mode = Mode::Off;
    else if (text == "detect") mode = Mode::Detect;
    else if (text == "fix") mode = Mode::Fix;
    else return false;
    return true;
}

std::string_view ModeName(Mode mode)
{
    return mode == Mode::Off ? "off" : mode == Mode::Detect ? "detect" : "fix";
}

std::string Describe(const Categories& c)
{
    return "Metadata=" + std::string(ModeName(c.metadata)) + ", audio/stream integrity=" +
        std::string(ModeName(c.integrity)) + ", format/container=" + std::string(ModeName(c.container)) +
        ", strict full decode=" + std::string(ModeName(c.decode));
}

std::string_view CategoryOf(std::string_view rule)
{
    static constexpr std::string_view legacy[]{
        "flac.leading-id3", "flac.trailing-tags", "flac.multiple-comment-blocks", "flac.duplicate-comments",
        "flac.empty-comments", "flac.invalid-comments", "flac.picture-descriptor", "flac.invalid-seektable",
        "flac.missing-md5", "mp3.duplicate-id3v2", "mp3.duplicate-frames", "mp3.empty-frames",
        "mp3.text-encoding", "mp3.picture-mime", "mp3.redundant-id3v1", "mp3.ape-lyrics3",
        "mp3.stream-garbage", "mp3.unsupported-id3-layout"
    };
    if (std::find(std::begin(legacy), std::end(legacy), rule) != std::end(legacy)) return "metadata";
    if (rule.starts_with("container.") || rule == "flac.leading-data") return "container";
    if (rule == "decode.strict" || rule == "flac.md5-mismatch") return "decode";
    if (rule == "unreadable") return "io";
    return "integrity";
}

std::set<std::string> EffectiveRules(const Categories& c, const std::set<std::string>& filter, bool id3)
{
    if (c.container == Mode::Fix || c.decode == Mode::Fix)
        throw std::invalid_argument("Container and strict decode support off or detect, not fix.");
    if (id3 && c.metadata != Mode::Fix)
        throw std::invalid_argument("Removing ID3v1 requires metadata fix mode.");
    const auto enabled = [&](const std::string& id) {
        const auto cat = CategoryOf(id);
        return (cat == "metadata" ? c.metadata : cat == "container" ? c.container :
            cat == "decode" ? c.decode : c.integrity) != Mode::Off;
    };
    std::set<std::string> result;
    if (filter.empty()) {
        for (const auto& rule : RepairRules::Catalog())
            if (enabled(rule.id) && rule.id != "mp3.redundant-id3v1") result.insert(rule.id);
    } else {
        for (const auto& idValue : filter) {
            if (!RepairRules::IsKnownRule(idValue)) throw std::invalid_argument("Unknown repair rule: " + idValue);
            if (!enabled(idValue)) throw std::invalid_argument("Repair rule belongs to a disabled category: " + idValue);
            result.insert(idValue);
        }
    }
    if (id3) result.insert("mp3.redundant-id3v1");
    if (result.empty()) throw std::invalid_argument("Enable at least one repair category.");
    return result;
}
}

namespace RepairRules
{
namespace
{
using namespace RepairIntegrity;
namespace fs = std::filesystem;

bool presentMd5(const std::string& md5)
{
    return md5.size() == 32 && md5.find_first_not_of('0') != std::string::npos;
}

Bytes readSpan(std::ifstream& in, std::uint64_t offset, std::uint64_t size)
{
    if (size > Bytes().max_size()) throw std::runtime_error("Metadata range is too large.");
    Bytes bytes(static_cast<std::size_t>(size));
    in.seekg(static_cast<std::streamoff>(offset));
    if (!in || (!bytes.empty() && !in.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()))))
        throw std::runtime_error("Cannot read metadata before-image.");
    return bytes;
}

void addDifferences(std::vector<Patch>& patches, std::uint64_t base, const Bytes& old, const Bytes& replacement)
{
    const auto common = (std::min)(old.size(), replacement.size());
    std::size_t pos = 0;
    while (pos < common) {
        if (old[pos] == replacement[pos]) { ++pos; continue; }
        const auto start = pos++;
        if (old.size() != replacement.size()) { pos = start; break; }
        while (pos < common && old[pos] != replacement[pos]) ++pos;
        patches.push_back({base + start, Bytes(old.begin() + start, old.begin() + pos),
            Bytes(replacement.begin() + start, replacement.begin() + pos)});
    }
    if (old.size() == replacement.size()) return;
    auto a = old.size(), b = replacement.size();
    while (a > pos && b > pos && old[a-1] == replacement[b-1]) { --a; --b; }
    patches.push_back({base + pos, Bytes(old.begin() + pos, old.begin() + a),
        Bytes(replacement.begin() + pos, replacement.begin() + b)});
}

Finding observation(Issue issue, bool fix)
{
    Finding finding{issue.rule, fix, 1, issue.detail};
    issue.patch.reset();
    finding.evidence = std::move(issue);
    return finding;
}
}

std::string FileDigest(const std::filesystem::path& path)
{
    auto digest = DecodeAnalyzer::FileSha256(path);
    if (!digest) throw std::runtime_error("Cannot verify complete file SHA-256.");
    return *digest;
}

std::string ProposedWording(std::string_view detail)
{
    static constexpr std::pair<std::string_view, std::string_view> verbs[]{
        {"Removed ", "Remove "}, {"Corrected ", "Correct "}, {"Replaced ", "Replace "}, {"Changed ", "Change "}};
    for (const auto& [past, present] : verbs)
        if (detail.starts_with(past)) return std::string(present) + std::string(detail.substr(past.size()));
    return std::string(detail);
}

std::string EditDigest(const Edit::StreamEdit& edit)
{
    AVHashContext* raw{};
    if (av_hash_alloc(&raw, "sha256") < 0) throw std::runtime_error("Cannot initialize edit-map digest.");
    auto freeHash = [](AVHashContext* hash) { av_hash_freep(&hash); };
    std::unique_ptr<AVHashContext, decltype(freeHash)> hash(raw, freeHash);
    av_hash_init(raw);
    const auto number = [raw](std::uint64_t value) {
        std::array<std::uint8_t, 8> bytes{};
        for (auto& byte : bytes) {byte = static_cast<std::uint8_t>(value); value >>= 8;}
        av_hash_update(raw, bytes.data(), bytes.size());
    };
    number(edit.samples); number(edit.declaredSamples); number(edit.bits);
    number(edit.needsMd5); number(edit.suffixOnly); number(edit.md5.size());
    av_hash_update(raw, reinterpret_cast<const std::uint8_t*>(edit.md5.data()), edit.md5.size());
    for (const auto& patch : edit.patches) {
        number(patch.offset); number(patch.removeSize);
        for (const auto* bytes : {&patch.before, &patch.after}) {
            number(bytes->size());
            if (!bytes->empty()) av_hash_update(raw, bytes->data(), bytes->size());
        }
    }
    std::array<std::uint8_t, 65> hex{};
    av_hash_final_hex(raw, hex.data(), static_cast<int>(hex.size()));
    return reinterpret_cast<const char*>(hex.data());
}

Analysis AnalyzeSelected(const std::filesystem::path& path, const std::set<std::string>& rules,
    const RepairIntegrity::Categories& c)
{
    if (c.Legacy()) return Analyze(path, rules);
    const auto ext = path.extension().string();
    const bool flac = ext.size() == 5 && (ext[1] == 'f' || ext[1] == 'F');
    std::set<std::string> metadata;
    bool integrity = false, container = false;
    for (const auto& id : rules) {
        if ((flac && id.starts_with("mp3.")) || (!flac && id.starts_with("flac."))) continue;
        const auto cat = CategoryOf(id);
        if (cat == "metadata") metadata.insert(id);
        if (cat == "integrity") integrity = true;
        if (cat == "container") container = true;
    }
    const auto type = ScanContainer(path, flac);
    Analysis a;
    if (type.complete && !metadata.empty()) {
        a = Analyze(path, metadata);
        if (c.metadata != Mode::Fix) {
            a.edit.reset();
            for (auto& f : a.findings) if (f.fix) {
                f.fix = false;
                f.detectionOnly = true;
                f.detail = "Detection only. Proposed metadata edit: " + ProposedWording(f.detail);
            }
        }
    }
    const bool wrongContainer = std::any_of(type.issues.begin(), type.issues.end(), [](const auto& i) {
        return i.rule == "container.mismatch" || i.rule == "container.empty" || i.rule == "container.no-audio";
    });
    for (const auto& issue : type.issues) {
        if (!container || !rules.contains(issue.rule)) continue;
        if (issue.rule == "flac.leading-data") {
            const auto prior = std::find_if(a.findings.begin(), a.findings.end(), [](const auto& f) {
                return f.rule == "flac.leading-id3";
            });
            if (prior != a.findings.end()) {
                prior->evidence = issue;
                prior->detail += " Also identified by " + issue.rule + ".";
                continue;
            }
        }
        a.findings.push_back(observation(issue, false));
    }
    Inspection scan;
    if (!wrongContainer && (integrity || (flac && rules.contains("flac.md5-mismatch"))))
        scan = flac ? ScanFlac(path) : ScanMp3(path);
    const bool wantsDecode = rules.contains("decode.strict") || (flac && rules.contains("flac.md5-mismatch"));
    bool blocked = !type.complete;
    if (wantsDecode && type.complete) {
        a.decoded = DecodeAnalyzer::Prove(path, flac);
        const auto& pcm = *a.decoded;
        if (!pcm.clean) {
            blocked = true;
            a.findings.push_back(observation({rules.contains("decode.strict") ? "decode.strict" : "flac.md5-mismatch",
                "Cannot verify complete PCM: " + pcm.error, pcm.errorOffset.value_or(0), 0,
                "clean complete decode", "unverified"}, false));
        } else if (flac && presentMd5(scan.md5) && pcm.flacMd5 != scan.md5) {
            blocked = true;
            if (rules.contains("flac.md5-mismatch"))
                a.findings.push_back(observation({"flac.md5-mismatch", "Decoded PCM differs from the original STREAMINFO MD5.",
                    0, 0, scan.md5, pcm.flacMd5}, false));
        }
    }
    Edit::StreamEdit edits;
    edits.samples = scan.samples;
    edits.bits = scan.bits;
    edits.declaredSamples = scan.declaredSamples;
    edits.md5 = scan.md5;
    const bool unsafeStream = !scan.complete || std::any_of(scan.issues.begin(), scan.issues.end(),
        [](const Issue& i) {return !i.patch && !i.informational;});
    for (const auto& issue : scan.issues) {
        if (!integrity || !rules.contains(issue.rule)) continue;
        std::string relatedRule;
        if (issue.rule == "flac.fill-md5" || issue.rule == "mp3.leading-data") {
            const auto oldId = issue.rule == "flac.fill-md5" ? "flac.missing-md5" : "mp3.stream-garbage";
            if (std::erase_if(a.findings, [&](const auto& f) {return f.rule == oldId;})) relatedRule = oldId;
        }
        const bool candidate = c.integrity == Mode::Fix && issue.patch.has_value();
        const bool suffix = issue.rule == "flac.trailing-data";
        const bool exceptional = suffix && type.complete && a.decoded && !a.decoded->clean &&
            scan.declaredSamples == scan.samples && presentMd5(scan.md5);
        const bool fix = candidate && !unsafeStream && (!blocked || exceptional);
        auto f = observation(issue, fix);
        f.detectionOnly = c.integrity == Mode::Detect && issue.patch.has_value();
        if (!relatedRule.empty()) f.detail += " Also identified by " + relatedRule + ".";
        if (candidate && !fix) f.proofStatus = "refused";
        a.findings.push_back(std::move(f));
        if (fix) {
            edits.patches.push_back(*issue.patch);
            edits.needsMd5 |= issue.rule == "flac.fill-md5";
            edits.suffixOnly = suffix;
        }
    }
    const auto refuse = [&](std::string_view reason) {
        a.edit.reset();
        for (auto& f : a.findings) {
            if (f.fix) {
                f.fix = false;
                f.proofStatus = "refused";
                f.detail = ProposedWording(f.detail) + " " + std::string(reason);
            }
        }
    };
    if (edits.patches.empty()) {
        if (blocked || (flac && integrity && c.integrity == Mode::Fix && unsafeStream) || std::any_of(a.findings.begin(), a.findings.end(),
            [](const auto& f) {return f.proofStatus == "refused";}))
            refuse("The file was not changed because preservation could not be proved.");
        if (a.findings.empty() && ((!type.complete && (!metadata.empty() || integrity || wantsDecode)) ||
                (integrity && !scan.complete))) {
            a.findings.push_back({"unreadable", false, 1,
                "The requested checks could not verify the native stream: " +
                (!scan.issues.empty() ? scan.issues.front().detail : !type.issues.empty() ? type.issues.front().detail : "incomplete frame walk.")});
        }
        return a;
    }
    edits.suffixOnly = edits.suffixOnly && edits.patches.size() == 1 && !a.edit;
    if (blocked && !edits.suffixOnly) {
        refuse("A strict source error prevents a combined repair.");
        return a;
    }
    if (a.edit) {
        for (const auto& f : a.findings)
            if (f.fix && CategoryOf(f.rule) == "metadata") edits.metadataFixes.insert(f.rule);
        std::ifstream in(path, std::ios::binary);
        const auto end = a.edit->audioOffset + a.edit->audioSize;
        addDifferences(edits.patches, 0, readSpan(in, 0, a.edit->audioOffset), a.edit->head);
        addDifferences(edits.patches, end, readSpan(in, end, fs::file_size(path) - end), a.edit->tail);
    }
    std::sort(edits.patches.begin(), edits.patches.end(), [](const auto& x, const auto& y) {return x.offset < y.offset;});
    const auto size = fs::file_size(path);
    std::uint64_t previousEnd = 0;
    std::optional<std::uint64_t> previousOffset;
    for (const auto& patch : edits.patches) {
        const auto len = patch.removeSize ? patch.removeSize : patch.before.size();
        if (previousOffset == patch.offset || patch.offset < previousEnd || patch.offset > size || len > size - patch.offset) {
            refuse("Proposed edit ranges overlap or are out of bounds.");
            return a;
        }
        previousEnd = patch.offset + len;
        previousOffset = patch.offset;
    }
    a.edit = Edit{};
    a.edit->stream = std::move(edits);
    return a;
}

bool PrepareProof(const std::filesystem::path& source, Edit::StreamEdit& e, Proof& p)
{
    p.status = "refused";
    p.source = DecodeAnalyzer::Prove(source, true);
    const auto& decoded = *p.source;
    if (!decoded.clean) {
        // A demuxed packet may contain the valid last frame AND the fill. The
        // packet localizes the failure; only the independent frame walk defines the cut.
        const auto intersects = [](std::uint64_t a, std::uint64_t n, std::uint64_t b, std::uint64_t m) {
            return n && m && (a <= b ? b - a < n : a - b < m);
        };
        const bool suffixPacket = e.suffixOnly && e.patches.size() == 1 && decoded.invalidBitstream &&
            decoded.errorOffset && decoded.errorPacketSize && intersects(*decoded.errorOffset, *decoded.errorPacketSize,
                e.patches.front().offset, e.patches.front().removeSize);
        if (!suffixPacket || !presentMd5(e.md5) || e.samples == 0 || e.declaredSamples != e.samples) {
            p.error = "Source strict decode failed: " + decoded.error;
            return false;
        }
        p.method = "original-streaminfo-md5-and-complete-frame-prefix";
    } else {
        if (decoded.samples != e.samples || decoded.bits != e.bits ||
            (presentMd5(e.md5) && decoded.flacMd5 != e.md5)) {
            p.error = "Strict source PCM, count or precision disagrees with independent frame evidence.";
            return false;
        }
        p.method = "strict-source-and-output";
        if (e.needsMd5) {
            if (!presentMd5(decoded.flacMd5)) { p.error = "No complete source PCM MD5."; return false; }
            auto it = std::find_if(e.patches.begin(), e.patches.end(), [](const auto& patch) {
                return patch.before == Bytes(16, 0) && patch.after == Bytes(16, 0);
            });
            if (it == e.patches.end()) { p.error = "Missing MD5 edit region."; return false; }
            for (std::size_t i = 0; i < 16; ++i)
                it->after[i] = static_cast<std::uint8_t>(std::stoul(decoded.flacMd5.substr(2*i, 2), nullptr, 16));
        }
    }
    p.status = "pending";
    return true;
}

bool VerifyProof(const std::filesystem::path& output, const Edit::StreamEdit& e, Proof& p)
{
    p.output = DecodeAnalyzer::Prove(output, true);
    const auto& decoded = *p.output;
    if (!decoded.clean || !p.source || decoded.samples != e.samples || decoded.bits != e.bits ||
        decoded.flacMd5.empty() || (presentMd5(e.md5) && decoded.flacMd5 != e.md5) ||
        (p.source->clean && (p.source->pcmSha256 != decoded.pcmSha256 ||
            p.source->samples != decoded.samples || p.source->format != decoded.format))) {
        p.status = "refused";
        p.error = decoded.clean ? "Staged PCM differs from the independent source evidence." : decoded.error;
        return false;
    }
    p.status = "passed";
    return true;
}
}
