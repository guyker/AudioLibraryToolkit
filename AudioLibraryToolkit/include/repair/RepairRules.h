#pragma once
#include "RepairIntegrity.h"
#include "DecodeAnalyzer.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Metadata edits preserve the audio byte range. Stream edits must additionally prove decoded PCM identity.
namespace RepairRules
{
struct RuleInfo
{
    std::string id;
    std::string format;
    bool canFix{ false };
    std::string description;
};

const std::vector<RuleInfo>& Catalog();
// The ID3v1 removal rule is excluded by default for compatibility with older players.
std::set<std::string> DefaultRules();
bool IsKnownRule(const std::string& id);
bool IsSupportedFile(const std::filesystem::path& path);
std::string ProposedWording(std::string_view detail);

struct Finding
{
    std::string rule;
    bool fix{ false };
    std::size_t count{ 0 };
    std::string detail;
    std::optional<RepairIntegrity::Issue> evidence;
    std::string proofStatus;
    bool detectionOnly{};
};

struct Edit
{
    std::vector<std::uint8_t> head;
    std::uint64_t audioOffset{ 0 };
    std::uint64_t audioSize{ 0 };
    std::vector<std::uint8_t> tail;
    struct StreamEdit
    {
        std::vector<RepairIntegrity::Patch> patches;
        std::uint64_t samples{}, declaredSamples{};
        int bits{};
        std::string md5;
        bool suffixOnly{}, needsMd5{};
        std::set<std::string> metadataFixes;
    };
    std::optional<StreamEdit> stream;
};

struct Analysis
{
    std::vector<Finding> findings;
    std::optional<Edit> edit;
    std::optional<RepairPcmProof> decoded;
};

// An empty rule set enables every rule. Throws std::runtime_error when the file cannot be parsed.
Analysis Analyze(const std::filesystem::path& path, const std::set<std::string>& enabled);
Analysis AnalyzeSelected(const std::filesystem::path& path, const std::set<std::string>& rules,
    const RepairIntegrity::Categories& categories);
struct Proof
{
    std::string status, method, error;
    std::optional<RepairPcmProof> source, output;
};
bool PrepareProof(const std::filesystem::path& source, Edit::StreamEdit& edit, Proof& proof);
bool VerifyProof(const std::filesystem::path& output, const Edit::StreamEdit& edit, Proof& proof);
std::string FileDigest(const std::filesystem::path& path);
std::string EditDigest(const Edit::StreamEdit& edit);
using WriteSink = std::function<void(const char*, std::size_t)>;
void Write(const std::filesystem::path& source, const Edit& edit, const WriteSink& sink);
// Confirms the output is exactly the planned edit, its audio range is byte-identical to the source and no enabled
// rule still finds anything to fix.
bool Verify(const std::filesystem::path& source, const std::filesystem::path& output, const Edit& edit,
    const std::set<std::string>& enabled, std::string& error);
}
