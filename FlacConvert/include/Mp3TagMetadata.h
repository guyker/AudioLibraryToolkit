#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace Mp3TagMetadata
{
using TagMap = std::map<std::string, std::vector<std::string>>;

bool WriteFromFlac(const std::filesystem::path& sourceFlac,
    const std::filesystem::path& temporaryMp3, std::string& error);
bool Read(const std::filesystem::path& mp3, TagMap& tags, std::string& error);
}
