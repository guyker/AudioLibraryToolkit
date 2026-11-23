#include "AppSettings.h"
#include "AlbumCollection.h"
#include "PlatformUtils.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <format>
#include <string>
#include <algorithm>
#include <cctype>
#include <set>
#include <system_error>
#include <iterator>
#include <regex>

#include <spdlog/spdlog.h>

std::shared_ptr<AppSettingsJson> AppSettingsJson::AppSettingsInstance = nullptr;

namespace fs = std::filesystem;

namespace {
fs::path pathForInspection(const fs::path& path)
{
#ifdef _WIN32
    return path.empty() ? path : fs::path(PlatformUtils::WindowsLongPath(path));
#else
    return path;
#endif
}

const std::regex& outDirectoryPattern()
{
    static const std::regex pattern(R"("OutDirectory"\s*:\s*"")");
    return pattern;
}

const std::regex& mediaDirectoryListPattern()
{
    static const std::regex pattern(R"("MediaDirectoryList"\s*:\s*\[\s*\])");
    return pattern;
}

std::optional<std::smatch> singleMatch(const std::string& text, const std::regex& pattern)
{
    auto it = std::sregex_iterator(text.begin(), text.end(), pattern);
    if (it == std::sregex_iterator()) return std::nullopt;
    std::smatch match = *it;
    if (++it != std::sregex_iterator()) return std::nullopt;
    return match;
}

// Reads a template that must parse like a configuration and leave the required paths empty for setup.
std::optional<std::string> readConfigTemplate(const fs::path& templatePath, std::string& problem)
{
    std::ifstream in(pathForInspection(templatePath), std::ios::binary);
    if (!in) {
        problem = "cannot be opened";
        return std::nullopt;
    }
    std::string text{ std::istreambuf_iterator<char>(in), {} };
    if (in.bad()) {
        problem = "cannot be read";
        return std::nullopt;
    }
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);

    auto parsed = AppSettingsJson::GetDefaultSettings();
    if (!parsed.loadFromJsonText(text)) {
        problem = "is not a valid configuration";
        return std::nullopt;
    }
    if (parsed.LogLevel < 0 || parsed.LogLevel > 6 || parsed.AlbumsSplitThreshold <= 0) {
        problem = "has an invalid LogLevel or AlbumsSplitThreshold";
        return std::nullopt;
    }
    if (!parsed.OutDirectory.empty() || !parsed.MediaDirectoryList.empty() ||
        !singleMatch(text, outDirectoryPattern()) || !singleMatch(text, mediaDirectoryListPattern())) {
        problem = "must contain exactly one empty OutDirectory and one empty MediaDirectoryList";
        return std::nullopt;
    }
    return text;
}

std::string replaceSingleMatch(const std::string& text, const std::regex& pattern, const std::string& replacement)
{
    const auto match = singleMatch(text, pattern);
    if (!match) return {};
    return text.substr(0, match->position(0)) + replacement + text.substr(match->position(0) + match->length(0));
}

// Fills the setup answers into the template so its comments and layout are preserved.
std::optional<std::string> fillConfigTemplate(const std::string& templateText,
    const std::string& outputPath, const std::string& inputPath)
{
    rapidjson::StringBuffer outBuffer;
    rapidjson::Writer<rapidjson::StringBuffer> outWriter(outBuffer);
    outWriter.String(outputPath.c_str(), static_cast<rapidjson::SizeType>(outputPath.size()));

    rapidjson::StringBuffer listBuffer;
    rapidjson::Writer<rapidjson::StringBuffer> listWriter(listBuffer);
    listWriter.StartArray();
    listWriter.StartObject();
    listWriter.Key("isActive");
    listWriter.Bool(true);
    listWriter.Key("mediaPath");
    listWriter.String(inputPath.c_str(), static_cast<rapidjson::SizeType>(inputPath.size()));
    listWriter.EndObject();
    listWriter.EndArray();

    auto text = replaceSingleMatch(templateText, outDirectoryPattern(),
        std::string("\"OutDirectory\": ") + outBuffer.GetString());
    if (text.empty()) return std::nullopt;
    text = replaceSingleMatch(text, mediaDirectoryListPattern(),
        std::string("\"MediaDirectoryList\": ") + listBuffer.GetString());
    if (text.empty()) return std::nullopt;

    auto check = AppSettingsJson::GetDefaultSettings();
    if (!check.loadFromJsonText(text) || check.OutDirectory != outputPath ||
        check.MediaDirectoryList.size() != 1 || !check.MediaDirectoryList[0].isActive ||
        check.MediaDirectoryList[0].mediaPath != inputPath) {
        return std::nullopt;
    }
    return text;
}
}

std::optional<fs::path> AppSettingsJson::FindDefaultConfigTemplate()
{
    const auto exeDir = PlatformUtils::ExecutableDirectory();
    if (exeDir.empty()) return std::nullopt;
    const fs::path candidates[] = {
        exeDir / DefaultConfigTemplateFileName,
        exeDir / ".." / "share" / "AudioLibraryToolkit" / DefaultConfigTemplateFileName,
    };
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(pathForInspection(candidate), ec) && !ec) {
            return candidate.lexically_normal();
        }
    }
    return std::nullopt;
}

fs::path userConfigPath(std::string& errorMessage) {
    try {
        return PlatformUtils::UserConfigDirectory() / AppSettingsJson::DefaultConfigFileName;
    }
    catch (const std::runtime_error& error) {
        errorMessage = error.what();
        return {};
    }
}

void save_string(const std::string& value, const fs::path& path) {
    std::ofstream out{ path, std::ios::binary };
    out << value;
}

std::string load_string(const fs::path& path) {
    std::ifstream in{ path, std::ios::binary };
    return std::string{ std::istreambuf_iterator<char>(in), {} };
}

std::shared_ptr<AppSettingsJson> AppSettingsJson::AppSetting() {
    if (AppSettingsInstance != nullptr) {
        return AppSettingsInstance;
    }

    std::string errorMessage;
    auto settings = LoadForCommandLine(std::nullopt, errorMessage);
    if (!settings) {
        std::cerr << "Error: " << errorMessage << std::endl;
        return nullptr;
    }
    const bool hasActiveLibrary = std::ranges::any_of(settings->MediaDirectoryList,
        [](const MediaDirectoryElement& library) { return library.isActive && !library.mediaPath.empty(); });
    if (settings->OutDirectory.empty() ||
        std::ranges::all_of(settings->OutDirectory, [](unsigned char ch) { return std::isspace(ch) != 0; }) ||
        !hasActiveLibrary) {
        AppSettingsInstance.reset();
        std::cerr << "Error: Edit the configuration file and set OutDirectory and at least one active "
            "MediaDirectoryList entry with a mediaPath before using the menu." << std::endl;
        return nullptr;
    }
    return settings;
}

bool AppSettingsJson::CreateInitialConfiguration(const fs::path& configPath,
    bool interactive, std::istream& input, std::ostream& output, std::string& errorMessage)
{
    return CreateInitialConfiguration(configPath, std::nullopt, interactive, input, output, errorMessage);
}

bool AppSettingsJson::CreateInitialConfiguration(const fs::path& configPath,
    const std::optional<fs::path>& templatePath,
    bool interactive, std::istream& input, std::ostream& output, std::string& errorMessage)
{
    std::optional<std::string> templateText;
    if (templatePath.has_value()) {
        std::string problem;
        templateText = readConfigTemplate(*templatePath, problem);
        if (!templateText) {
            output << "Warning: default configuration template '" << CommonUtils::path_to_utf8(*templatePath)
                << "' " << problem << "; using built-in defaults.\n";
        }
    }

    auto settings = GetDefaultSettings();
    bool completed = false;
    std::string outputPath;
    std::string inputPath;
    if (interactive) {
        output << "First-time setup. Enter an output directory and an existing audio library directory.\n"
            "The configuration can be opened and edited after setup.\n";
        const auto readPath = [&](const char* prompt, bool mustExist, std::string& result) {
            for (;;) {
                output << prompt << std::flush;
                std::string value;
                if (!PlatformUtils::ReadUtf8Line(input, value)) return false;
                if (value.empty() || std::ranges::all_of(value,
                        [](unsigned char ch) { return std::isspace(ch) != 0; })) {
                    output << "A path is required.\n";
                    continue;
                }
                std::error_code ec;
                auto path = fs::absolute(CommonUtils::path_from_utf8(value), ec).lexically_normal();
                if (ec) {
                    output << "Cannot resolve that path: " << ec.message() << '\n';
                    continue;
                }
                if (path.filename().empty() && path.has_relative_path()) {
                    path = path.parent_path();
                }
                const auto status = fs::status(pathForInspection(path), ec);
                if (ec && ec != std::errc::no_such_file_or_directory) {
                    output << "Cannot inspect that path: " << ec.message() << '\n';
                    continue;
                }
                if ((mustExist && !fs::is_directory(status)) ||
                    (!mustExist && fs::exists(status) && !fs::is_directory(status))) {
                    output << (mustExist ? "The input must be an existing directory.\n" :
                        "The output path must be a directory or a new directory path.\n");
                    continue;
                }
                result = CommonUtils::path_to_utf8(path);
                return true;
            }
        };
        completed = readPath("Output directory: ", false, outputPath) &&
            readPath("Input library directory: ", true, inputPath);
        if (completed) {
            settings.OutDirectory = outputPath;
            settings.MediaDirectoryList.push_back({ true, inputPath, std::nullopt });
        }
    }

    std::string content;
    if (templateText) {
        if (!completed) {
            content = *templateText;
        }
        else if (auto filled = fillConfigTemplate(*templateText, outputPath, inputPath)) {
            content = std::move(*filled);
        }
        else {
            output << "Warning: cannot fill the default configuration template; using built-in defaults.\n";
        }
    }
    if (content.empty()) {
        content = settings.toJsonString() + "\n";
    }

    std::error_code ec;
    fs::create_directories(pathForInspection(configPath).parent_path(), ec);
    if (ec) {
        errorMessage = "Cannot create configuration directory '" +
            CommonUtils::path_to_utf8(configPath.parent_path()) + "': " + ec.message();
        return false;
    }
    if (!PlatformUtils::WriteNewTextFile(configPath, content, ec)) {
        errorMessage = "Cannot create configuration file '" + CommonUtils::path_to_utf8(configPath) +
            "': " + ec.message() + ". Existing files are not overwritten; check for an incomplete file.";
        return false;
    }
    if (!completed) {
        errorMessage = "Initial configuration created at '" + CommonUtils::path_to_utf8(configPath) +
            "'. Open and edit it to set OutDirectory and at least one active MediaDirectoryList "
            "entry with a mediaPath, then run the application again.";
        return false;
    }
    output << "Configuration saved. You can open and edit it at "
        << CommonUtils::path_to_utf8(configPath) << ".\n";
    return true;
}

std::shared_ptr<AppSettingsJson> AppSettingsJson::LoadForCommandLine(
    const std::optional<fs::path>& explicitConfigPath,
    std::string& errorMessage)
{
    fs::path configPath;
    if (explicitConfigPath.has_value()) {
        configPath = explicitConfigPath.value();
        std::error_code ec;
        if (!fs::is_regular_file(pathForInspection(configPath), ec) || ec) {
            errorMessage = "Specified configuration file is not readable: " + CommonUtils::path_to_utf8(configPath);
            return nullptr;
        }
    }
    else {
        configPath = userConfigPath(errorMessage);
        if (configPath.empty()) {
            return nullptr;
        }
        std::error_code ec;
        const bool exists = fs::exists(pathForInspection(configPath), ec);
        if (ec) {
            errorMessage = "Cannot inspect configuration file '" +
                CommonUtils::path_to_utf8(configPath) + "': " + ec.message();
            return nullptr;
        }
        if (!exists) {
            const bool interactive = PlatformUtils::IsStdinInteractive() && PlatformUtils::IsStdoutInteractive();
            if (!CreateInitialConfiguration(configPath, FindDefaultConfigTemplate(), interactive,
                    std::cin, std::cout, errorMessage)) {
                return nullptr;
            }
        }
        else if (!fs::is_regular_file(pathForInspection(configPath), ec) || ec) {
            errorMessage = "Configuration path is not a readable regular file: " +
                CommonUtils::path_to_utf8(configPath);
            return nullptr;
        }
    }

    auto settings = LoadWithDefaults(configPath, errorMessage);
    if (settings) {
        AppSettingsInstance = settings;
    }
    return settings;
}

// Built-in defaults are overlaid by the configuration file, if any.
// Shared by the command line and the interactive menu so both resolve settings identically.
std::shared_ptr<AppSettingsJson> AppSettingsJson::LoadWithDefaults(
    const fs::path& configPath, std::string& errorMessage)
{
    auto settings = std::make_shared<AppSettingsJson>(GetDefaultSettings());
    if (!configPath.empty() && !settings->loadFromFile(configPath)) {
        errorMessage = "Failed to load configuration file: " + CommonUtils::path_to_utf8(configPath);
        return nullptr;
    }

    if (settings->LogLevel < 0 || settings->LogLevel > 6) {
        errorMessage = "LogLevel must be an integer from 0 through 6.";
        return nullptr;
    }
    if (settings->AlbumsSplitThreshold <= 0) {
        errorMessage = "AlbumsSplitThreshold must be greater than zero.";
        return nullptr;
    }
    return settings;
}

// Convert UTF-8 string to wide string
std::wstring UTF8ToWide(const std::string& str) {
    return CommonUtils::path_from_utf8(str).wstring();
}

// Convert object to JSON string
std::string AppSettingsJson::toJsonString() const {
    rapidjson::Document doc;
    doc.SetObject();
    auto& allocator = doc.GetAllocator();

    doc.AddMember("Version", rapidjson::Value(Version.c_str(), allocator), allocator);
    doc.AddMember("OutDirectory", rapidjson::Value(OutDirectory.c_str(), allocator), allocator);
    doc.AddMember("DatabaseFileName", rapidjson::Value(DatabaseFileName.c_str(), allocator), allocator);

    rapidjson::Value flacObj(rapidjson::kObjectType);
    flacObj.AddMember("ffmpeg_exe_name", rapidjson::Value(PlatformUtils::WideToUTF8(FLACSettings.ffmpeg_exe_name).c_str(), allocator), allocator);
    flacObj.AddMember("ffmpeg_convert_24bit_flac", rapidjson::Value(PlatformUtils::WideToUTF8(FLACSettings.ffmpeg_convert_24bit_flac).c_str(), allocator), allocator);
    flacObj.AddMember("ffmpeg_get_metadata_tags", rapidjson::Value(PlatformUtils::WideToUTF8(FLACSettings.ffmpeg_get_metadata_tags).c_str(), allocator), allocator);
    doc.AddMember("FLACSettings", flacObj, allocator);

    rapidjson::Value mediaArray(rapidjson::kArrayType);
    for (const auto& media : MediaDirectoryList) {
        rapidjson::Value mediaObj(rapidjson::kObjectType);
        mediaObj.AddMember("isActive", media.isActive, allocator);
        mediaObj.AddMember("mediaPath", rapidjson::Value(media.mediaPath.c_str(), allocator), allocator);
		if (media.mediaName.has_value()) {
			std::string mediaName = media.mediaName.value();
			mediaObj.AddMember("mediaName", rapidjson::Value(mediaName.c_str(), allocator), allocator);
		}
		else {
			mediaObj.AddMember("mediaName", rapidjson::Value().SetNull(), allocator);
		}
        mediaArray.PushBack(mediaObj, allocator);
    }
    doc.AddMember("MediaDirectoryList", mediaArray, allocator);

    const auto policyName = [](MetadataSidecarPolicy policy) {
        return policy == MetadataSidecarPolicy::Disabled ? "disabled" :
            policy == MetadataSidecarPolicy::Optional ? "optional" : "required";
    };
    rapidjson::Value sidecarObj(rapidjson::kObjectType);
    sidecarObj.AddMember("Format", rapidjson::StringRef(
        MetadataSidecars.Format == MetadataSidecarFormat::Json ? "json" : "binary"), allocator);
    sidecarObj.AddMember("ConvertToFlac", rapidjson::StringRef(policyName(MetadataSidecars.ConvertToFlac)), allocator);
    sidecarObj.AddMember("ConvertToMp3", rapidjson::StringRef(policyName(MetadataSidecars.ConvertToMp3)), allocator);
    doc.AddMember("MetadataSidecars", sidecarObj, allocator);

    doc.AddMember("LogLevel", LogLevel, allocator);
    doc.AddMember("UseAsyncMetadataReads", UseAsyncMetadataReads, allocator);
    doc.AddMember("Incremental", Incremental, allocator);
    doc.AddMember("ScanFlacMetadataArchives", ScanFlacMetadataArchives, allocator);
    doc.AddMember("ScanTier", rapidjson::Value(ScanTier.c_str(), allocator), allocator);
    doc.AddMember("TagIssueResolution", rapidjson::Value(TagIssueResolution.c_str(), allocator), allocator);
    doc.AddMember("MetadataScanMaxThreads", MetadataScanMaxThreads, allocator);
    doc.AddMember("DecodeWorkers", DecodeWorkers, allocator);
    rapidjson::Value catalogExtensions(rapidjson::kArrayType);
    for (const auto& extension : CatalogExtensions) {
        rapidjson::Value value;
        value.SetString(extension.c_str(), static_cast<rapidjson::SizeType>(extension.size()), allocator);
        catalogExtensions.PushBack(value, allocator);
    }
    doc.AddMember("CatalogExtensions", catalogExtensions, allocator);
    doc.AddMember("ExtraAudioQualityMetrics", ExtraAudioQualityMetrics, allocator);
    doc.AddMember("MinMatchingTracksForDuplicate", MinMatchingTracksForDuplicate, allocator);
    doc.AddMember("SizeMatchPercentageThreshold", SizeMatchPercentageThreshold, allocator);
    doc.AddMember("RecursionDirectorySearchDepth", RecursionDirectorySearchDepth, allocator);
    doc.AddMember("AlbumsSplitThreshold", AlbumsSplitThreshold, allocator);
    doc.AddMember("JsonBatchTargetBytes", JsonBatchTargetBytes, allocator);
    doc.AddMember("DuplicateDurationToleranceSeconds", DuplicateDurationToleranceSeconds, allocator);
    doc.AddMember("DuplicateFuzzyNames", DuplicateFuzzyNames, allocator);
    doc.AddMember("DuplicateStripEditionText", DuplicateStripEditionText, allocator);
    doc.AddMember("DuplicatePartialMatches", DuplicatePartialMatches, allocator);
    doc.AddMember("DuplicatePartialMinCoverage", DuplicatePartialMinCoverage, allocator);

    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    return buffer.GetString();
}

// Save JSON to a file
void AppSettingsJson::saveToFile(const std::filesystem::path& filename) const {
    std::ofstream ofs(pathForInspection(filename));
    if (!ofs) {
        std::cerr << "Error: Cannot open file for writing: " << filename << std::endl;
        return;
    }

    rapidjson::OStreamWrapper osw(ofs);
    rapidjson::PrettyWriter<rapidjson::OStreamWrapper> writer(osw);

    rapidjson::Document doc;
    doc.Parse(toJsonString().c_str());
    doc.Accept(writer);
}

// Load JSON from a file
bool AppSettingsJson::loadFromFile(const std::filesystem::path& filename) {
    std::ifstream ifs(pathForInspection(filename), std::ios::binary);
    if (!ifs) {
        std::cerr << "Error: Cannot open file for reading: " << filename << std::endl;
        return false;
    }
    const std::string text{ std::istreambuf_iterator<char>(ifs), {} };
    if (ifs.bad()) {
        std::cerr << "Error: Cannot read file: " << filename << std::endl;
        return false;
    }
    return loadFromJsonText(text);
}

bool AppSettingsJson::loadFromJsonText(const std::string& text) {
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseCommentsFlag>(text.c_str(), text.size());

    if (doc.HasParseError() || !doc.IsObject()) {
        std::cerr << "Error: Configuration must be a valid JSON object." << std::endl;
        return false;
    }
    if (doc.HasMember("AlbumsSplitThreshold") &&
        (!doc["AlbumsSplitThreshold"].IsInt() || doc["AlbumsSplitThreshold"].GetInt() <= 0)) {
        spdlog::error("AlbumsSplitThreshold must be a positive integer.");
        return false;
    }
    if (doc.HasMember("JsonBatchTargetBytes") && !doc["JsonBatchTargetBytes"].IsUint64()) {
        spdlog::error("JsonBatchTargetBytes must be a non-negative integer.");
        return false;
    }
    if (doc.HasMember("LogLevel") &&
        (!doc["LogLevel"].IsInt() || doc["LogLevel"].GetInt() < 0 ||
            doc["LogLevel"].GetInt() > 6)) {
        spdlog::error("LogLevel must be an integer from 0 through 6.");
        return false;
    }
    if (doc.HasMember("MediaDirectoryList") && !doc["MediaDirectoryList"].IsArray()) {
        spdlog::error("MediaDirectoryList must be an array.");
        return false;
    }
    if (doc.HasMember("OutDirectory") && !doc["OutDirectory"].IsString()) {
        spdlog::error("OutDirectory must be a string.");
        return false;
    }
    if (doc.HasMember("MetadataScanMaxThreads") &&
        (!doc["MetadataScanMaxThreads"].IsInt() || doc["MetadataScanMaxThreads"].GetInt() < 0)) {
        spdlog::error("MetadataScanMaxThreads must be a non-negative integer.");
        return false;
    }
    if (doc.HasMember("DecodeWorkers") &&
        (!doc["DecodeWorkers"].IsInt() || doc["DecodeWorkers"].GetInt() < 0)) {
        spdlog::error("DecodeWorkers must be a non-negative integer.");
        return false;
    }
    if (doc.HasMember("CatalogExtensions")) {
        const auto& extensions = doc["CatalogExtensions"];
        static const std::set<std::string> supportedExtensions{
            ".flac", ".mp3", ".m4a", ".mp4", ".ogg", ".oga", ".opus",
            ".wv", ".ape", ".wav", ".aif", ".aiff"
        };
        if (!extensions.IsArray() || extensions.Empty()) {
            spdlog::error("CatalogExtensions must be a non-empty array of supported extensions.");
            return false;
        }
        std::vector<std::string> parsedExtensions;
        std::set<std::string> seenExtensions;
        for (const auto& value : extensions.GetArray()) {
            if (!value.IsString()) {
                spdlog::error("CatalogExtensions entries must be strings.");
                return false;
            }
            std::string extension = value.GetString();
            std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            if (!supportedExtensions.contains(extension)) {
                spdlog::error("Unsupported CatalogExtensions entry '{}'.", extension);
                return false;
            }
            if (!seenExtensions.insert(extension).second) {
                spdlog::error("Duplicate CatalogExtensions entry '{}'.", extension);
                return false;
            }
            parsedExtensions.push_back(std::move(extension));
        }
        CatalogExtensions = std::move(parsedExtensions);
    }
    if (doc.HasMember("SizeMatchPercentageThreshold") &&
        (!doc["SizeMatchPercentageThreshold"].IsInt() || doc["SizeMatchPercentageThreshold"].GetInt() < 0)) {
        spdlog::error("SizeMatchPercentageThreshold must be a non-negative integer.");
        return false;
    }
    if (doc.HasMember("DuplicateDurationToleranceSeconds") &&
        (!doc["DuplicateDurationToleranceSeconds"].IsNumber() ||
            !(doc["DuplicateDurationToleranceSeconds"].GetDouble() >= 0.0))) {
        spdlog::error("DuplicateDurationToleranceSeconds must be a non-negative number.");
        return false;
    }
    for (const char* name : { "DuplicateFuzzyNames", "DuplicateStripEditionText", "DuplicatePartialMatches" }) {
        if (doc.HasMember(name) && !doc[name].IsBool()) {
            spdlog::error("{} must be a boolean.", name);
            return false;
        }
    }
    if (doc.HasMember("DuplicatePartialMinCoverage") &&
        (!doc["DuplicatePartialMinCoverage"].IsInt() || doc["DuplicatePartialMinCoverage"].GetInt() < 1 ||
            doc["DuplicatePartialMinCoverage"].GetInt() > 100)) {
        spdlog::error("DuplicatePartialMinCoverage must be an integer from 1 through 100.");
        return false;
    }
    if (doc.HasMember("ScanTier") &&
        (!doc["ScanTier"].IsString() ||
            (std::string_view(doc["ScanTier"].GetString()) != "catalog" &&
                std::string_view(doc["ScanTier"].GetString()) != "integrity" &&
                std::string_view(doc["ScanTier"].GetString()) != "analysis"))) {
        spdlog::error("ScanTier must be catalog, integrity, or analysis.");
        return false;
    }
    if (doc.HasMember("TagIssueResolution") &&
        (!doc["TagIssueResolution"].IsString() ||
            (std::string_view(doc["TagIssueResolution"].GetString()) != "ask" &&
                std::string_view(doc["TagIssueResolution"].GetString()) != "auto"))) {
        spdlog::error("TagIssueResolution must be ask or auto.");
        return false;
    }
    if (doc.HasMember("UseFFmpegLibraryAPI")) {
        spdlog::warn("UseFFmpegLibraryAPI is deprecated and ignored; metadata re-probes always use FFmpeg libraries.");
    }

    if (doc.HasMember("Version") && doc["Version"].IsString()) {
        Version = doc["Version"].GetString();
    }
    if (doc.HasMember("OutDirectory") && doc["OutDirectory"].IsString()) {
        OutDirectory = doc["OutDirectory"].GetString();
    }
    if (doc.HasMember("DatabaseFileName") && doc["DatabaseFileName"].IsString()) {
        DatabaseFileName = doc["DatabaseFileName"].GetString();
    }

    if (doc.HasMember("FLACSettings") && doc["FLACSettings"].IsObject()) {
        const auto& flacObj = doc["FLACSettings"];
        if (flacObj.HasMember("ffmpeg_exe_name") && flacObj["ffmpeg_exe_name"].IsString()) {
            FLACSettings.ffmpeg_exe_name = UTF8ToWide(flacObj["ffmpeg_exe_name"].GetString());
        }
        if (flacObj.HasMember("ffmpeg_convert_24bit_flac") && flacObj["ffmpeg_convert_24bit_flac"].IsString()) {
            FLACSettings.ffmpeg_convert_24bit_flac = UTF8ToWide(flacObj["ffmpeg_convert_24bit_flac"].GetString());
        }
        if (flacObj.HasMember("ffmpeg_get_metadata_tags") && flacObj["ffmpeg_get_metadata_tags"].IsString()) {
            FLACSettings.ffmpeg_get_metadata_tags = UTF8ToWide(flacObj["ffmpeg_get_metadata_tags"].GetString());
        }
    }

    if (doc.HasMember("MetadataSidecars")) {
        const auto& sidecarObj = doc["MetadataSidecars"];
        if (!sidecarObj.IsObject()) {
            std::cerr << "Error: MetadataSidecars must be a JSON object." << std::endl;
            return false;
        }
        if (sidecarObj.HasMember("Format")) {
            const auto& value = sidecarObj["Format"];
            const std::string text = value.IsString() ? value.GetString() : "";
            if (text == "binary") MetadataSidecars.Format = MetadataSidecarFormat::Binary;
            else if (text == "json") MetadataSidecars.Format = MetadataSidecarFormat::Json;
            else {
                std::cerr << "Error: MetadataSidecars.Format must be \"binary\" or \"json\"." << std::endl;
                return false;
            }
        }
        const auto readPolicy = [&](const char* key, MetadataSidecarPolicy& policy) {
            if (!sidecarObj.HasMember(key)) return true;
            const auto& value = sidecarObj[key];
            const std::string text = value.IsString() ? value.GetString() : "";
            if (text == "disabled") policy = MetadataSidecarPolicy::Disabled;
            else if (text == "optional") policy = MetadataSidecarPolicy::Optional;
            else if (text == "required") policy = MetadataSidecarPolicy::Required;
            else {
                std::cerr << "Error: MetadataSidecars." << key
                    << " must be \"disabled\", \"optional\" or \"required\"." << std::endl;
                return false;
            }
            return true;
        };
        if (!readPolicy("ConvertToFlac", MetadataSidecars.ConvertToFlac) ||
            !readPolicy("ConvertToMp3", MetadataSidecars.ConvertToMp3)) {
            return false;
        }
    }

    if (doc.HasMember("MediaDirectoryList") && doc["MediaDirectoryList"].IsArray()) {
        MediaDirectoryList.clear();
        for (const auto& mediaObj : doc["MediaDirectoryList"].GetArray()) {
            if (!mediaObj.IsObject()) {
                spdlog::error("MediaDirectoryList entries must be objects.");
                return false;
            }
            MediaDirectoryElement media{};
            if (mediaObj.HasMember("isActive")) {
                if (!mediaObj["isActive"].IsBool()) {
                    spdlog::error("MediaDirectoryList isActive must be a boolean.");
                    return false;
                }
                media.isActive = mediaObj["isActive"].GetBool();
            }
            if (mediaObj.HasMember("mediaPath")) {
                if (!mediaObj["mediaPath"].IsString()) {
                    spdlog::error("MediaDirectoryList mediaPath must be a string.");
                    return false;
                }
                media.mediaPath = mediaObj["mediaPath"].GetString();
            }
            if (media.isActive && (media.mediaPath.empty() ||
                std::ranges::all_of(media.mediaPath, [](unsigned char ch) { return std::isspace(ch) != 0; }))) {
                // An empty path would resolve to the process working directory.
                spdlog::error("Active MediaDirectoryList entries require a non-empty mediaPath.");
                return false;
            }
            if (mediaObj.HasMember("mediaName") && mediaObj["mediaName"].IsString()) {
                media.mediaName = mediaObj["mediaName"].GetString();
            }
			else {
                try
                {
                    std::filesystem::path path = CommonUtils::path_from_utf8(media.mediaPath);
                    if (std::filesystem::is_regular_file(pathForInspection(path))) {
                        path = path.parent_path();
                    }
                    if (path.filename().empty() && path.has_relative_path()) {
                        path = path.lexically_normal().parent_path();
                    }
                    media.mediaName = CommonUtils::path_to_utf8(path.filename());
                }
                catch (const std::filesystem::filesystem_error& e)
                {
                    spdlog::error("Error getting media name: {}", e.what());
                    media.mediaName = std::nullopt;
                }
			}


            MediaDirectoryList.push_back(media);
        }
    }

    if (doc.HasMember("LogLevel") && doc["LogLevel"].IsInt()) {
        LogLevel = doc["LogLevel"].GetInt();
    }
    if (doc.HasMember("UseAsyncMetadataReads") && doc["UseAsyncMetadataReads"].IsBool()) {
        UseAsyncMetadataReads = doc["UseAsyncMetadataReads"].GetBool();
    }
    else if (!doc.HasMember("UseAsyncMetadataReads") &&
        doc.HasMember("UseAsyncFFmpegCalls") && doc["UseAsyncFFmpegCalls"].IsBool()) {
        UseAsyncMetadataReads = doc["UseAsyncFFmpegCalls"].GetBool();
    }
    if (doc.HasMember("Incremental")) {
        if (!doc["Incremental"].IsBool()) {
            spdlog::error("Incremental must be a boolean.");
            return false;
        }
        Incremental = doc["Incremental"].GetBool();
    }
    if (doc.HasMember("ScanFlacMetadataArchives")) {
        if (!doc["ScanFlacMetadataArchives"].IsBool()) {
            spdlog::error("ScanFlacMetadataArchives must be a boolean.");
            return false;
        }
        ScanFlacMetadataArchives = doc["ScanFlacMetadataArchives"].GetBool();
    }
    if (doc.HasMember("ScanTier")) {
        ScanTier = doc["ScanTier"].GetString();
    }
    if (doc.HasMember("TagIssueResolution")) {
        TagIssueResolution = doc["TagIssueResolution"].GetString();
    }
    if (doc.HasMember("MetadataScanMaxThreads")) {
        MetadataScanMaxThreads = doc["MetadataScanMaxThreads"].GetInt();
    }
    if (doc.HasMember("DecodeWorkers")) {
        DecodeWorkers = doc["DecodeWorkers"].GetInt();
    }
    if (doc.HasMember("CatalogExtensions")) {
        CatalogExtensions.clear();
        for (const auto& extension : doc["CatalogExtensions"].GetArray()) {
            CatalogExtensions.emplace_back(extension.GetString());
            std::ranges::transform(CatalogExtensions.back(), CatalogExtensions.back().begin(),
                [](unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
        }
    }
    if (doc.HasMember("ExtraAudioQualityMetrics") && doc["ExtraAudioQualityMetrics"].IsBool()) {
        ExtraAudioQualityMetrics = doc["ExtraAudioQualityMetrics"].GetBool();
    }
    if (doc.HasMember("MinMatchingTracksForDuplicate") && doc["MinMatchingTracksForDuplicate"].IsInt()) {
        MinMatchingTracksForDuplicate = doc["MinMatchingTracksForDuplicate"].GetInt();
    }
    if (doc.HasMember("SizeMatchPercentageThreshold") && doc["SizeMatchPercentageThreshold"].IsInt()) {
        SizeMatchPercentageThreshold = doc["SizeMatchPercentageThreshold"].GetInt();
    }
    if (doc.HasMember("RecursionDirectorySearchDepth") && doc["RecursionDirectorySearchDepth"].IsInt()) {
        RecursionDirectorySearchDepth = doc["RecursionDirectorySearchDepth"].GetInt();
    }
    if (doc.HasMember("AlbumsSplitThreshold") && doc["AlbumsSplitThreshold"].IsInt()) {
        AlbumsSplitThreshold = doc["AlbumsSplitThreshold"].GetInt();
    }
    if (doc.HasMember("JsonBatchTargetBytes")) {
        JsonBatchTargetBytes = doc["JsonBatchTargetBytes"].GetUint64();
    }
    if (doc.HasMember("DuplicateDurationToleranceSeconds")) {
        DuplicateDurationToleranceSeconds = doc["DuplicateDurationToleranceSeconds"].GetDouble();
    }
    if (doc.HasMember("DuplicateFuzzyNames")) {
        DuplicateFuzzyNames = doc["DuplicateFuzzyNames"].GetBool();
    }
    if (doc.HasMember("DuplicateStripEditionText")) {
        DuplicateStripEditionText = doc["DuplicateStripEditionText"].GetBool();
    }
    if (doc.HasMember("DuplicatePartialMatches")) {
        DuplicatePartialMatches = doc["DuplicatePartialMatches"].GetBool();
    }
    if (doc.HasMember("DuplicatePartialMinCoverage")) {
        DuplicatePartialMinCoverage = doc["DuplicatePartialMinCoverage"].GetInt();
    }
    
    return true;
}
