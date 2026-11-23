#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "Mp3TagIssues.h"

namespace Mp3TagMetadata
{
using TagMap = std::map<std::string, std::vector<std::string>>;

// Writes the source's native FLAC comments, after the selected tag-issue resolutions, as ID3v2.4.
bool WriteFromFlac(const std::filesystem::path& sourceFlac,
    const std::filesystem::path& temporaryMp3, const Mp3TagIssues::Resolutions& resolutions,
    std::string& error);
bool Read(const std::filesystem::path& mp3, TagMap& tags, std::string& error);
}
