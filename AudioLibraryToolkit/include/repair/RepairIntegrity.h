#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace RepairIntegrity
{
enum class Mode { Off, Detect, Fix };
struct Categories
{
    Mode metadata = Mode::Fix, integrity = Mode::Off, container = Mode::Off, decode = Mode::Off;
    bool Legacy() const;
};
bool ParseMode(std::string_view text, Mode& mode);
std::string_view ModeName(Mode mode);
std::string Describe(const Categories& categories);
std::string_view CategoryOf(std::string_view rule);
std::set<std::string> EffectiveRules(const Categories& categories, const std::set<std::string>& filter,
    bool removeId3v1);

using Bytes = std::vector<std::uint8_t>;
struct Patch
{
    std::uint64_t offset;
    Bytes before, after;
    std::uint64_t removeSize = 0;
    bool operator==(const Patch&) const = default;
};
struct Issue
{
    std::string rule, detail;
    std::uint64_t offset = 0, length = 0;
    std::string expected, observed;
    bool informational = false;
    std::optional<Patch> patch;
};
struct Inspection
{
    std::vector<Issue> issues;
    std::uint64_t samples = 0;
    int bits = 0;
    std::string md5;
    std::uint64_t declaredSamples = 0;
    bool complete = false;
};
Inspection ScanFlac(const std::filesystem::path& path);
Inspection ScanMp3(const std::filesystem::path& path);
Inspection ScanContainer(const std::filesystem::path& path, bool namedFlac);
}
