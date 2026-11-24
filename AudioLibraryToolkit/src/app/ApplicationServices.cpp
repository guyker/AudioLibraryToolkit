#include "ApplicationServices.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <optional>
#include <ranges>
#include <sstream>
#include <system_error>

#include <spdlog/spdlog.h>

#include "AppSettings.h"
#include "AudioRepair.h"
#include "CommonUtils.h"
#include "FFmpeg.h"
#include "FolderConvert.h"
#include "OutputLayout.h"
#include "PlatformUtils.h"
#include "RepairRules.h"
#include "RunLogging.h"

namespace
{
namespace fs = std::filesystem;

std::string pathToUtf8(const fs::path& path)
{
    return CommonUtils::path_to_utf8(path);
}

fs::path pathFromUtf8(const std::string& path)
{
    return CommonUtils::path_from_utf8(path);
}
}

int ApplicationServices::Run(const CommandLineOptions& options)
{
    try {
        if (options.action == CommandAction::InitializeConfig) {
            const auto configPath = options.configPathSpecified
                ? pathFromUtf8(options.configPath)
                : PlatformUtils::UserConfigDirectory() / AppSettingsJson::DefaultConfigFileName;
            std::istringstream input(options.outputDirectory + "\n" + options.inputPath + "\n");
            std::ostringstream output;
            std::string error;
            if (!AppSettingsJson::CreateInitialConfiguration(configPath,
                    AppSettingsJson::FindDefaultConfigTemplate(), true, input, output, error)) {
                std::cerr << "Configuration setup failed: " << error << '\n';
                return 1;
            }
            std::cout << output.str();
            return 0;
        }

        std::optional<fs::path> explicitConfigPath;
        if (options.configPathSpecified) {
            explicitConfigPath = pathFromUtf8(options.configPath);
        }
        std::string configurationError;
        const auto settings = AppSettingsJson::LoadForCommandLine(explicitConfigPath, configurationError);
        if (!settings) {
            std::cerr << "Configuration error: " << configurationError << '\n';
            RunLogging::RecordError("Configuration error: " + configurationError);
            return 1;
        }

        if (options.action != CommandAction::Duplicates && !options.outputDirectorySpecified &&
            (settings->OutDirectory.empty() || std::ranges::all_of(settings->OutDirectory,
                [](unsigned char ch) { return std::isspace(ch) != 0; }))) {
            const std::string error = "Set OutDirectory in the configuration file or pass --output-dir.";
            std::cerr << "Configuration error: " << error << '\n';
            RunLogging::RecordError("Configuration error: " + error);
            return 1;
        }

        auto fileLogLevel = static_cast<spdlog::level::level_enum>(settings->LogLevel);
        if (options.logLevel == "info") fileLogLevel = spdlog::level::info;
        else if (options.logLevel == "debug") fileLogLevel = spdlog::level::debug;
        else if (options.logLevel == "trace") fileLogLevel = spdlog::level::trace;
        RunLogging::SetFileLevel(fileLogLevel);
        FFmpeg::initialize_ffmpeg_logging();

        const fs::path inputPath = pathFromUtf8(options.inputPath);
        const fs::path outputRoot = options.outputDirectorySpecified
            ? pathFromUtf8(options.outputDirectory)
            : pathFromUtf8(settings->OutDirectory);
        const bool catalogInput = options.action == CommandAction::Duplicates ||
            options.action == CommandAction::DatabaseImport;
        spdlog::info("Source folder{}: '{}'.", catalogInput ? " (catalog JSON)" : "",
            pathToUtf8(fs::absolute(inputPath).lexically_normal()));

        switch (options.action) {
        case CommandAction::Convert: {
            std::error_code ec;
            const auto absoluteInput = fs::absolute(inputPath, ec).lexically_normal();
            const auto name = absoluteInput.filename().empty()
                ? absoluteInput.parent_path().filename() : absoluteInput.filename();
            if (ec || name.empty() || name == "." || name == "..") {
                spdlog::error("Conversion input must have a library folder name.");
                return 1;
            }
            auto conversion = options.conversion;
            std::string sidecarError;
            const auto sidecarFormat = settings->MetadataSidecars.Resolve(
                conversion.target, options.noMetadataSidecar, sidecarError);
            if (!sidecarFormat) {
                std::cerr << "Configuration error: " << sidecarError << '\n';
                RunLogging::RecordError("Configuration error: " + sidecarError);
                return 1;
            }
            conversion.sidecarFormat = *sidecarFormat;
            if (options.preview) {
                FolderConversionPlan plan;
                if (FolderConvert::PrepareConversion(
                        inputPath, OutputLayout::Conversion(outputRoot, conversion.target, name),
                        options.overwrite, conversion, plan) != 0) {
                    return 1;
                }
                std::cout << "Conversion preview:\n"
                    << "  Source: " << pathToUtf8(plan.input) << '\n'
                    << "  Output: " << pathToUtf8(plan.output) << '\n'
                    << "  Audio conversions: " << plan.audio.size() << '\n'
                    << "  Companion files copied: " << plan.companions.size() << '\n'
                    << "  Directories planned: " << plan.directories.size() << '\n'
                    << "  Unchanged audio skipped: " << plan.skippedAudio.size() << '\n';
                if (conversion.target == AudioTarget::Mp3) {
                    const auto counts = FolderConvert::CountTagIssues(plan);
                    for (const auto kind : Mp3TagIssues::AllKinds) {
                        const auto count = counts[static_cast<std::size_t>(kind)];
                        if (count != 0) {
                            std::cout << "  Tag issue: " << Mp3TagIssues::Name(kind)
                                << " (" << count << " file(s))";
                            const auto action = conversion.tagIssues.Get(kind);
                            if (action == Mp3TagIssues::Action::Unresolved &&
                                settings->TagIssueResolution == "auto") {
                                std::cout << " - configured automatic recommendation";
                            }
                            else if (action == Mp3TagIssues::Action::Unresolved) {
                                std::cout << " - requires a choice or configured automatic policy";
                            }
                            else {
                                std::cout << " - " << Mp3TagIssues::ActionLabel(kind, action);
                            }
                            std::cout << '\n';
                        }
                    }
                }
                if (FolderConvert::ReportDestinationConflicts(plan)) {
                    std::cerr << "Preview is blocked by destination conflicts; no work was started.\n";
                    return 1;
                }
                return 0;
            }
            return FolderConvert::ConvertAudioFolderToOutput(
                inputPath, OutputLayout::Conversion(outputRoot, conversion.target, name),
                options.overwrite, conversion, settings->TagIssueResolution == "auto") == 0 ? 0 : 1;
        }

        case CommandAction::Repair: {
            std::error_code ec;
            const auto absoluteInput = fs::absolute(inputPath, ec).lexically_normal();
            const auto name = absoluteInput.filename().empty()
                ? absoluteInput.parent_path().filename() : absoluteInput.filename();
            if (ec || name.empty() || name == "." || name == "..") {
                spdlog::error("Repair input must have a library folder name.");
                return 1;
            }
            AudioRepair::Plan plan;
            auto rules = options.repairRules.empty() ? RepairRules::DefaultRules() : options.repairRules;
            if (options.repairRemoveId3v1) rules.insert("mp3.redundant-id3v1");
            if (AudioRepair::Prepare(inputPath, OutputLayout::Repair(outputRoot, name),
                    options.overwrite, rules, plan, options.repairJobs, options.repairDryRun) != 0) {
                return 1;
            }
            if (options.preview) {
                const auto repairCount = std::count_if(plan.files.begin(), plan.files.end(),
                    [](const AudioRepair::PlannedFile& file) { return file.repair; });
                const auto reviewCount = std::count_if(plan.findings.begin(), plan.findings.end(),
                    [](const AudioRepair::Finding& finding) {
                        return finding.status == "manual-review" || finding.status == "unreadable";
                    });
                std::cout << "Repair preview:\n"
                    << "  Source: " << pathToUtf8(plan.input) << '\n'
                    << "  Output: " << pathToUtf8(plan.output) << '\n'
                    << "  Files inspected: " << plan.inspectedFiles << '\n'
                    << "  Files to repair: " << repairCount << '\n'
                    << "  Findings requiring review: " << reviewCount << '\n'
                    << "  Existing destination conflicts: " << plan.conflicts.size() << '\n';
                ApplicationServiceOperations::PrintRepairFindings(plan);
                return AudioRepair::ReportDestinationConflicts(plan) ? 1 : 0;
            }
            if (options.repairDryRun) {
                return AudioRepair::PrintDryRunReport(plan);
            }
            std::cout << "Repair preview: " << pathToUtf8(plan.input) << '\n'
                << "  Output: " << pathToUtf8(plan.output) << '\n'
                << "  FLAC/MP3 files inspected: " << plan.inspectedFiles << '\n'
                << "  Files to repair: " << std::count_if(plan.files.begin(), plan.files.end(),
                    [](const AudioRepair::PlannedFile& file) { return file.repair; }) << '\n'
                << "  Unresolved findings: " << std::count_if(plan.findings.begin(), plan.findings.end(),
                    [](const AudioRepair::Finding& finding) {
                        return finding.status == "manual-review" || finding.status == "unreadable";
                    }) << '\n'
                << "  Existing destination conflicts: " << plan.conflicts.size() << '\n';
            ApplicationServiceOperations::PrintRepairFindings(plan);
            return AudioRepair::Execute(plan) == 0 ? 0 : 1;
        }

        case CommandAction::Scan: {
            if (options.scanTierSpecified) {
                settings->ScanTier = options.scanTier;
            }
            const bool archiveFlacMetadata = options.archiveFlacMetadata ||
                (!options.noArchiveFlacMetadata && settings->ScanFlacMetadataArchives);
            return ApplicationServiceOperations::Scan(
                inputPath, outputRoot, archiveFlacMetadata, options.fullRescan);
        }

        case CommandAction::Duplicates:
            if (!fs::is_directory(inputPath)) {
                spdlog::error("JSON input path is not a directory: {}", pathToUtf8(inputPath));
                return 1;
            }
            if (!options.duplicateQuarantinePaths.empty() && outputRoot.empty()) {
                std::cerr << "Configuration error: duplicate quarantine requires an output directory.\n";
                return 1;
            }
            {
                std::vector<fs::path> quarantinePaths;
                quarantinePaths.reserve(options.duplicateQuarantinePaths.size());
                for (const auto& path : options.duplicateQuarantinePaths) {
                    quarantinePaths.push_back(pathFromUtf8(path));
                }
            return ApplicationServiceOperations::FindDuplicates(
                inputPath, options.openDuplicateFolders,
                options.reportPath.empty() ? fs::path{} : pathFromUtf8(options.reportPath),
                quarantinePaths, outputRoot);
            }

        case CommandAction::DatabaseImport:
            if (!fs::is_directory(inputPath)) {
                spdlog::error("JSON input path is not a directory: {}", pathToUtf8(inputPath));
                return 1;
            }
            return ApplicationServiceOperations::ImportDatabase(inputPath, outputRoot);

        default:
            return 2;
        }
    }
    catch (const fs::filesystem_error& error) {
        spdlog::error("Filesystem error: {}", error.what());
        return 1;
    }
    catch (const std::exception& error) {
        spdlog::error("Command failed: {}", error.what());
        return 1;
    }
}
