#pragma once

#include "MediaRecord.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

struct RepairPcmProof
{
    bool clean = false;
    std::uint64_t samples = 0;
    int bits = 0;
    std::string format, pcmSha256, flacMd5, error;
    bool invalidBitstream = false;
    // Verified source range of a rejected packet, not necessarily the offending byte.
    // Both are absent when no exact packet is attributable (including drain errors).
    std::optional<std::uint64_t> errorOffset, errorPacketSize;
};

class DecodeAnalyzer
{
public:
    static void Analyze(const std::filesystem::path& filePath, MediaRecord& record,
        bool includeSignalAnalysis);
    static RepairPcmProof Prove(const std::filesystem::path& path, bool flac);
    static std::optional<std::string> FileSha256(const std::filesystem::path& path);
};
