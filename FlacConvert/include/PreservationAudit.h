#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "Mp3TagIssues.h"

namespace PreservationAudit
{
bool VerifyCopy(const std::filesystem::path& source, const std::filesystem::path& copy);
// Reads native FLAC comments and counts FLAC-only blocks; false with error when unreadable.
bool ReadFlacTagSource(const std::filesystem::path& flac, Mp3TagIssues::FlacTagSource& source,
    std::string& error);
enum class Status
{
    Preserved,
    Normalized,
    SidecarOnly,
    MissingOrChanged,
    NotVerified,
    SourceOnly
};

struct Entry
{
    Status status;
    std::string resource;
    std::string name;
    std::string detail;
    bool fatal{ false };
};

struct Result
{
    std::vector<Entry> entries;
    bool ok{ true };
};

// verifiedArchive must mean FlacMetadata::Verify succeeded for the archive that will be installed.
// Differences caused by the selected tag-issue resolutions are reported as non-fatal.
Result Compare(const std::filesystem::path& source,
    const std::filesystem::path& temporaryOutput, bool verifiedArchive,
    const Mp3TagIssues::Resolutions& resolutions = {});
const char* StatusName(Status status);
}
