#pragma once

#include <array>
#include <string>
#include <string_view>
#include <cstddef>
#include <filesystem>
#include <vector>
#include "ConversionOptions.h"

namespace fs = std::filesystem;
namespace RunLogging { class PhaseProgress; }

struct ScanInfo {
    long folders_count;

    long not_convertable_file_count;
    long not_regular_file_count;
    long file_with_no_extension_count;
    long convertable_file_count;

    long long convertable_files_size;

    std::vector<fs::directory_entry> mediaList;
};

struct PlannedFileConversion {
    fs::path source;
    fs::path target;
    fs::path temporary;
};

// A source subtree omitted from FLAC output because it contains audio but nothing to convert.
struct SkippedDirectory {
    fs::path source;
    std::size_t files{ 0 };
    std::size_t audioFiles{ 0 };
};

// An MP3 source whose FLAC metadata has tag issues needing a resolution choice.
struct TagIssueFile {
    fs::path source;
    Mp3TagIssues::Found found;
};

struct FolderConversionPlan {
    fs::path input;
    fs::path output;
    bool overwrite{ false };
    ConversionOptions options;
    std::vector<PlannedFileConversion> audio;
    std::vector<PlannedFileConversion> companions;
    std::vector<fs::path> directories;
    std::vector<SkippedDirectory> skippedDirectories;
    std::vector<fs::path> skippedAudio;
    std::vector<fs::path> existingTargets;
    std::vector<fs::path> collisions;
    std::vector<TagIssueFile> tagIssueFiles;
    std::vector<fs::path> tagIssueSkipped;
};

struct ConversionProgress {
    std::size_t converted{ 0 };
    std::size_t copied{ 0 };
    fs::path actualOutput;
};

class FolderConvert
{
public:

    static int ScanAudioFiles(ScanInfo& scanInfo, const std::filesystem::path& directory, bool bAsync = false);
    static int ConvertAudioFolderToOutput(
        const std::filesystem::path& inputDirectory,
        const std::filesystem::path& outputDirectory,
        bool overwrite,
        ConversionOptions options = {},
        bool autoResolveTagIssues = false);
    static int PrepareConversion(
        const fs::path& input, const fs::path& output, bool overwrite,
        ConversionOptions options, FolderConversionPlan& plan);
    // Number of planned files affected by each tag-issue kind.
    static std::array<std::size_t, Mp3TagIssues::KindCount> CountTagIssues(const FolderConversionPlan& plan);
    // Stores the resolutions in the plan and removes files (and companions of folders left without audio)
    // affected by a kind resolved as SkipFiles. Stop must be handled by the caller before this call.
    static void ApplyTagIssueResolutions(FolderConversionPlan& plan, const Mp3TagIssues::Resolutions& resolutions);
    // Reports blocked plans with console summaries and file-only paths; returns whether execution is blocked.
    static bool ReportDestinationConflicts(const FolderConversionPlan& plan);
    // Optional counters accumulate across plans, only after successful installation.
    static int ExecuteConversion(const FolderConversionPlan& plan, ConversionProgress* progress = nullptr,
        RunLogging::PhaseProgress* overallProgress = nullptr);
    
    static int ConverAudioFolder(const std::filesystem::path& directory, const ScanInfo scanInfo, bool bAsync);
    //static int ConverAudioFolder(const std::filesystem::path& directory, const ScanInfo scanInfo, bool bAsync = false);
    static int ConverAudioFolder2(const std::filesystem::path& directory, const ScanInfo scanInfo, bool bAsync = false);

private:

    static constexpr std::string_view GetTargetFileType() {
        return ".flac";
    }   
};
