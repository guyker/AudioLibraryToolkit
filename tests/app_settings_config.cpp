#include "AppSettings.h"
#include "PlatformUtils.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void setConfigEnvironment(const fs::path& root)
{
#ifdef _WIN32
    const auto value = (root / "local-app-data").wstring();
    if (_wputenv_s(L"LOCALAPPDATA", value.c_str()) != 0) {
        throw std::runtime_error("Could not set LOCALAPPDATA for the test");
    }
#elif defined(__APPLE__)
    const auto value = (root / "home").string();
    if (setenv("HOME", value.c_str(), 1) != 0) {
        throw std::runtime_error("Could not set HOME for the test");
    }
#else
    const auto value = (root / "xdg-config").string();
    if (setenv("XDG_CONFIG_HOME", value.c_str(), 1) != 0) {
        throw std::runtime_error("Could not set XDG_CONFIG_HOME for the test");
    }
#endif
}

void writeConfig(const fs::path& path, int logLevel)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << "{\"LogLevel\":" << logLevel << "}\n";
    require(static_cast<bool>(output), "Could not write test config");
}

void testRepairCategories(const fs::path& shippedTemplate)
{
    using RepairIntegrity::Mode;
    auto settings = AppSettingsJson::GetDefaultSettings();
    require(settings.RepairCategories.Legacy(), "Built-in repair defaults are not metadata-only");
    // Load over non-default modes to check that the shipped template actually specifies every default.
    settings.RepairCategories = { Mode::Off, Mode::Fix, Mode::Detect, Mode::Detect };
    require(settings.loadFromFile(shippedTemplate) && settings.RepairCategories.Legacy(),
        "Shipped repair defaults differ from the built-in defaults");
    require(settings.loadFromJsonText(R"({"RepairCategories":{"integrity":"detect"}})") &&
            settings.RepairCategories.metadata == Mode::Fix && settings.RepairCategories.integrity == Mode::Detect,
        "Partial repair settings did not inherit metadata repair");
    require(settings.loadFromJsonText(R"({"RepairCategories":{"metadata":"off","container":"detect"}})"),
        "Could not overlay partial repair categories");
    auto loaded = AppSettingsJson::GetDefaultSettings();
    require(loaded.loadFromJsonText(settings.toJsonString()) && loaded.RepairCategories.metadata == Mode::Off &&
            loaded.RepairCategories.integrity == Mode::Detect && loaded.RepairCategories.container == Mode::Detect &&
            loaded.RepairCategories.decode == Mode::Off,
        "Repair categories were lost in serialization");

    for (const auto* text : {
             R"({"RepairCategories":[]})", R"({"RepairCategories":{"container":"fix"}})",
             R"({"RepairCategories":{"decode":"fix"}})", R"({"RepairCategories":{"unknown":"off"}})",
             R"({"RepairCategories":{"metadata":true}})", R"({"RepairCategories":{"metadata":"repair"}})",
             R"({"RepairCategories":{"metadata":"fix\u0000"}})", R"({"RepairCategories":{"metadata":"off"}})",
             R"({"RepairCategories":{"integrity":"detect","integrity":"fix"}})",
             R"({"RepairCategories":{},"RepairCategories":{"integrity":"fix"}})" }) {
        auto invalid = AppSettingsJson::GetDefaultSettings();
        require(!invalid.loadFromJsonText(text), "Invalid or all-off repair categories were accepted");
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "Usage: app_settings_config_test <config.default.json>\n";
        return 2;
    }
    const fs::path shippedTemplate = argv[1];
    const auto originalDirectory = fs::current_path();
    const auto testRoot = fs::temp_directory_path() /
        ("afc-user-config-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
#ifdef _WIN32
    const fs::path cleanupRoot = PlatformUtils::WindowsLongPath(testRoot, true);
#else
    const auto& cleanupRoot = testRoot;
#endif
    try {
        testRepairCategories(shippedTemplate);
        fs::create_directories(testRoot / "working");
        setConfigEnvironment(testRoot / fs::path(u8"config-\u97f3"));
        fs::current_path(testRoot / "working");

        const auto expectedConfig =
            PlatformUtils::UserConfigDirectory() / AppSettingsJson::DefaultConfigFileName;
        const auto workingConfig = fs::current_path() / "config.json";
        writeConfig(workingConfig, 5);
        std::string errorMessage;
        std::istringstream unusedInput;
        std::ostringstream setupOutput;
        require(!AppSettingsJson::CreateInitialConfiguration(
                    expectedConfig, false, unusedInput, setupOutput, errorMessage),
            "Non-interactive setup succeeded without required paths");
        require(fs::is_regular_file(expectedConfig) && errorMessage.find("Open and edit") != std::string::npos,
            "Non-interactive setup did not create an editable template");
        auto defaults = AppSettingsJson::LoadWithDefaults(expectedConfig, errorMessage);
        require(defaults != nullptr && defaults->LogLevel == 1 && defaults->OutDirectory.empty() &&
                defaults->MediaDirectoryList.empty(),
            "The default configuration contained a local output or library");
        require(fs::is_regular_file(workingConfig), "Working-directory config was changed");

        const auto encoderConfig = testRoot / "encoder.json";
        for (const auto& name : { L"", L"ffmpeg", L"ffmpeg.exe", L"bundled" }) {
            auto settings = AppSettingsJson::GetDefaultSettings();
            settings.FLACSettings.ffmpeg_exe_name = name;
            settings.saveToFile(encoderConfig);
            const auto loaded = AppSettingsJson::LoadWithDefaults(encoderConfig, errorMessage);
            require(loaded && loaded->FLACSettings.ffmpeg_exe_name == L"bundled",
                "A legacy FFmpeg default retained host executable lookup");
        }
        for (const auto& invalid : { R"("ffmpeg-custom")", R"("./ffmpeg")", "false", R"("ffmpeg\u0000.exe")" }) {
            {
                std::ofstream file(encoderConfig);
                file << "{\"FLACSettings\":{\"ffmpeg_exe_name\":" << invalid << "}}";
            }
            require(!AppSettingsJson::LoadWithDefaults(encoderConfig, errorMessage),
                "Invalid FFmpeg selection was accepted");
        }
        auto overrideSettings = AppSettingsJson::GetDefaultSettings();
        overrideSettings.FLACSettings.ffmpeg_exe_name =
            CommonUtils::utf8ToWstring(CommonUtils::path_to_utf8(testRoot / fs::path(u8"encoder-\u97f3")));
        overrideSettings.saveToFile(encoderConfig);
        const auto overrideLoaded = AppSettingsJson::LoadWithDefaults(encoderConfig, errorMessage);
        require(overrideLoaded && overrideLoaded->FLACSettings.ffmpeg_exe_name ==
                overrideSettings.FLACSettings.ffmpeg_exe_name,
            "An absolute Unicode encoder override was not preserved");

        writeConfig(expectedConfig, 4);
        auto preferred = AppSettingsJson::LoadForCommandLine(std::nullopt, errorMessage);
        require(preferred != nullptr && preferred->LogLevel == 4,
            "User config did not take precedence over the working-directory config");

        auto explicitSettings = AppSettingsJson::LoadForCommandLine(workingConfig, errorMessage);
        require(explicitSettings != nullptr && explicitSettings->LogLevel == 5,
            "--config did not take precedence over the user config");

        errorMessage.clear();
        auto missingExplicit = AppSettingsJson::LoadForCommandLine(testRoot / "missing.json", errorMessage);
        require(missingExplicit == nullptr && !errorMessage.empty(),
            "Missing explicit config did not report an error");

        fs::remove(expectedConfig);
        fs::create_directory(expectedConfig);
        errorMessage.clear();
        auto invalidConfig = AppSettingsJson::LoadForCommandLine(std::nullopt, errorMessage);
        require(invalidConfig == nullptr && !errorMessage.empty(),
            "A non-file user config was silently ignored");

        const auto library = testRoot / fs::path(u8"library-\u97f3");
        const auto destination = testRoot / fs::path(u8"output-\u00e9");
        fs::create_directory(library);
        const auto interactiveConfig = testRoot / "interactive" / "config.json";
        std::istringstream answers("\n" + CommonUtils::path_to_utf8(destination) + "\n" +
            CommonUtils::path_to_utf8(testRoot / "missing-library") + "\n" +
            CommonUtils::path_to_utf8(library) + "\n");
        std::ostringstream prompts;
        errorMessage.clear();
        require(AppSettingsJson::CreateInitialConfiguration(
                    interactiveConfig, true, answers, prompts, errorMessage),
            "Interactive first-time setup failed");
        auto configured = AppSettingsJson::LoadWithDefaults(interactiveConfig, errorMessage);
        require(configured != nullptr && configured->OutDirectory == CommonUtils::path_to_utf8(destination) &&
                configured->MediaDirectoryList.size() == 1 &&
                configured->MediaDirectoryList.front().mediaPath == CommonUtils::path_to_utf8(library),
            "Interactive setup did not persist the requested Unicode paths");
        require(prompts.str().find("A path is required") != std::string::npos &&
                prompts.str().find("existing directory") != std::string::npos,
            "Interactive setup did not retry invalid paths");
        const auto relativeConfig = testRoot / "relative" / "config.json";
        std::istringstream relativeAnswers(
            CommonUtils::path_to_utf8(fs::path("..") / destination.filename()) + "\n" +
            CommonUtils::path_to_utf8(fs::path("..") / library.filename()) + "\n");
        require(AppSettingsJson::CreateInitialConfiguration(
                    relativeConfig, true, relativeAnswers, prompts, errorMessage),
            "Interactive setup rejected relative paths");
        const auto relativeSettings = AppSettingsJson::LoadWithDefaults(relativeConfig, errorMessage);
        require(relativeSettings != nullptr &&
                relativeSettings->OutDirectory == CommonUtils::path_to_utf8(destination) &&
                relativeSettings->MediaDirectoryList.size() == 1 &&
                relativeSettings->MediaDirectoryList.front().mediaPath == CommonUtils::path_to_utf8(library),
            "Interactive setup did not make relative paths independent of the working directory");

        const auto trailingConfig = testRoot / "trailing" / "config.json";
        std::istringstream trailingAnswers(
            CommonUtils::path_to_utf8(destination / fs::path{}) + "\n" +
            CommonUtils::path_to_utf8(library / fs::path{}) + "\n");
        require(AppSettingsJson::CreateInitialConfiguration(
                    trailingConfig, true, trailingAnswers, prompts, errorMessage),
            "Interactive setup rejected paths with trailing separators");
        const auto trailingSettings = AppSettingsJson::LoadWithDefaults(trailingConfig, errorMessage);
        require(trailingSettings != nullptr &&
                trailingSettings->OutDirectory == CommonUtils::path_to_utf8(destination) &&
                trailingSettings->MediaDirectoryList.size() == 1 &&
                trailingSettings->MediaDirectoryList.front().mediaPath == CommonUtils::path_to_utf8(library) &&
                trailingSettings->MediaDirectoryList.front().mediaName == CommonUtils::path_to_utf8(library.filename()),
            "Interactive setup did not normalize trailing separators and retain the library name");

        const auto manuallyEditedConfig = testRoot / "manually-edited.json";
        auto manuallyEdited = AppSettingsJson::GetDefaultSettings();
        manuallyEdited.MediaDirectoryList.push_back(
            { true, CommonUtils::path_to_utf8(library / fs::path{}), std::nullopt });
        manuallyEdited.saveToFile(manuallyEditedConfig);
        const auto manuallyLoaded = AppSettingsJson::LoadWithDefaults(manuallyEditedConfig, errorMessage);
        require(manuallyLoaded != nullptr && manuallyLoaded->MediaDirectoryList.size() == 1 &&
                manuallyLoaded->MediaDirectoryList.front().mediaName == CommonUtils::path_to_utf8(library.filename()),
            "A manually configured library path with a trailing separator lost its default folder name");
#ifdef _WIN32
        const auto longLibrary = testRoot / std::string(100, 'a') / std::string(100, 'b') /
            fs::path(u8"library-\u97f3");
        fs::create_directories(PlatformUtils::WindowsLongPath(longLibrary, true));
        const auto longConfig = testRoot / std::string(100, 'c') / std::string(100, 'd') / "config.json";
        const auto longDestination = testRoot / std::string(100, 'e') / std::string(100, 'f') / "output";
        std::istringstream longAnswers(CommonUtils::path_to_utf8(longDestination) + "\n" +
            CommonUtils::path_to_utf8(longLibrary) + "\n");
        require(AppSettingsJson::CreateInitialConfiguration(
                    longConfig, true, longAnswers, prompts, errorMessage),
            "Interactive setup could not save long Windows paths");
        const auto longSettings = AppSettingsJson::LoadWithDefaults(longConfig, errorMessage);
        require(longSettings != nullptr && longSettings->MediaDirectoryList.size() == 1 &&
                longSettings->OutDirectory == CommonUtils::path_to_utf8(longDestination) &&
                longSettings->MediaDirectoryList.front().mediaPath == CommonUtils::path_to_utf8(longLibrary) &&
                longSettings->MediaDirectoryList.front().mediaName == CommonUtils::path_to_utf8(longLibrary.filename()),
            "Interactive setup did not persist a long Windows library path and its default name");
        require(AppSettingsJson::LoadForCommandLine(longConfig, errorMessage) != nullptr,
            "An explicitly selected long Windows configuration path could not be loaded");

        setConfigEnvironment(testRoot / std::string(100, 'g') / std::string(100, 'h'));
        const auto longUserConfig =
            PlatformUtils::UserConfigDirectory() / AppSettingsJson::DefaultConfigFileName;
        std::istringstream longUnusedInput;
        errorMessage.clear();
        require(!AppSettingsJson::CreateInitialConfiguration(
                    longUserConfig, false, longUnusedInput, prompts, errorMessage) &&
                fs::is_regular_file(PlatformUtils::WindowsLongPath(longUserConfig, true)) &&
                errorMessage.find("Open and edit") != std::string::npos,
            "First-time setup did not create an editable template at a long Windows user config path");
        const auto longUserSettings = AppSettingsJson::LoadForCommandLine(std::nullopt, errorMessage);
        require(longUserSettings != nullptr && longUserSettings->OutDirectory.empty() &&
                longUserSettings->MediaDirectoryList.empty(),
            "A long Windows user configuration path could not be discovered and loaded");
#endif
        const auto originalContent = configured->toJsonString();
        std::istringstream replacementAnswers(CommonUtils::path_to_utf8(destination) + "\n" +
            CommonUtils::path_to_utf8(library) + "\n");
        require(!AppSettingsJson::CreateInitialConfiguration(
                    interactiveConfig, true, replacementAnswers, prompts, errorMessage),
            "First-time setup overwrote an existing configuration");
        configured = AppSettingsJson::LoadWithDefaults(interactiveConfig, errorMessage);
        require(configured != nullptr && configured->toJsonString() == originalContent,
            "An existing configuration was changed");

        const auto interruptedConfig = testRoot / "interrupted" / "config.json";
        std::istringstream interruptedAnswers(CommonUtils::path_to_utf8(destination) + "\n");
        require(!AppSettingsJson::CreateInitialConfiguration(
                    interruptedConfig, true, interruptedAnswers, prompts, errorMessage),
            "Interrupted setup succeeded without an input library");
        const auto interrupted = AppSettingsJson::LoadWithDefaults(interruptedConfig, errorMessage);
        require(interrupted != nullptr && interrupted->OutDirectory.empty() &&
                interrupted->MediaDirectoryList.empty(),
            "Interrupted setup persisted incomplete paths");

        const auto unreadableConfig = testRoot / "unreadable" / "config.json";
        std::istringstream unreadableInput;
        unreadableInput.setstate(std::ios_base::badbit);
        bool readErrorReported = false;
        try {
            AppSettingsJson::CreateInitialConfiguration(
                unreadableConfig, true, unreadableInput, prompts, errorMessage);
        }
        catch (const std::ios_base::failure&) {
            readErrorReported = true;
        }
        require(readErrorReported && !fs::exists(unreadableConfig),
            "An input read error was treated as an interrupted setup");

        const auto readText = [](const fs::path& path) {
            std::ifstream in(path, std::ios::binary);
            return std::string{ std::istreambuf_iterator<char>(in), {} };
        };
        const auto templateText = readText(shippedTemplate);
        require(templateText.find("//") != std::string::npos, "The shipped template has no comments");

        const auto copiedConfig = testRoot / "template-copy" / "config.json";
        std::ostringstream templateOutput;
        require(!AppSettingsJson::CreateInitialConfiguration(copiedConfig, shippedTemplate, false,
                    unusedInput, templateOutput, errorMessage) &&
                errorMessage.find("Open and edit") != std::string::npos,
            "Non-interactive template setup did not ask the user to edit the configuration");
        require(readText(copiedConfig) == templateText && templateOutput.str().find("Warning") == std::string::npos,
            "The shipped template was not copied unchanged");

        const auto filledConfig = testRoot / "template-filled" / "config.json";
        std::istringstream templateAnswers(CommonUtils::path_to_utf8(destination) + "\n" +
            CommonUtils::path_to_utf8(library) + "\n");
        require(AppSettingsJson::CreateInitialConfiguration(filledConfig, shippedTemplate, true,
                    templateAnswers, templateOutput, errorMessage),
            "Interactive template setup failed");
        const auto filledText = readText(filledConfig);
        auto filled = AppSettingsJson::LoadWithDefaults(filledConfig, errorMessage);
        require(filled != nullptr && filledText.find("//") != std::string::npos &&
                templateOutput.str().find("Warning") == std::string::npos &&
                filled->OutDirectory == CommonUtils::path_to_utf8(destination) &&
                filled->MediaDirectoryList.size() == 1 && filled->MediaDirectoryList.front().isActive &&
                filled->MediaDirectoryList.front().mediaPath == CommonUtils::path_to_utf8(library),
            "Interactive template setup lost comments or the requested paths");

        const auto badTemplate = testRoot / "bad-template.json";
        {
            std::ofstream bad(badTemplate, std::ios::binary);
            bad << "{\n  // private\n  \"OutDirectory\": \"/private/out\",\n  \"MediaDirectoryList\": []\n}\n";
        }
        for (const auto& candidate : { badTemplate, testRoot / "missing-template.json" }) {
            const auto fallbackConfig = testRoot / ("fallback-" + candidate.stem().string()) / "config.json";
            std::ostringstream fallbackOutput;
            require(!AppSettingsJson::CreateInitialConfiguration(fallbackConfig, candidate, false,
                        unusedInput, fallbackOutput, errorMessage),
                "Fallback setup succeeded without required paths");
            auto fallback = AppSettingsJson::LoadWithDefaults(fallbackConfig, errorMessage);
            require(fallback != nullptr && fallback->OutDirectory.empty() &&
                    readText(fallbackConfig).find("//") == std::string::npos &&
                    fallbackOutput.str().find("using built-in defaults") != std::string::npos,
                "An unusable template did not fall back to built-in defaults with a warning");
        }

        fs::current_path(originalDirectory);
        fs::remove_all(cleanupRoot);
        return 0;
    }
    catch (const std::exception& error) {
        fs::current_path(originalDirectory);
        std::error_code cleanupError;
        fs::remove_all(cleanupRoot, cleanupError);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
