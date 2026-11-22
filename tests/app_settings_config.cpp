#include "AppSettings.h"
#include "PlatformUtils.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

int main()
{
    const auto originalDirectory = fs::current_path();
    const auto testRoot = fs::temp_directory_path() /
        ("afc-user-config-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directories(testRoot / "working");
        setConfigEnvironment(testRoot / fs::path(u8"config-\u97f3"));
        fs::current_path(testRoot / "working");

        const auto expectedConfig =
            PlatformUtils::UserConfigDirectory() / AppSettingsJson::DefaultConfigFileName;
        const auto workingConfig = fs::current_path() / "config.json";
        writeConfig(workingConfig, 5);
        std::string errorMessage;
        auto defaults = AppSettingsJson::LoadForCommandLine(std::nullopt, errorMessage);
        require(defaults != nullptr && defaults->LogLevel == 1,
            "Working-directory config was used instead of built-in defaults");
        require(!fs::exists(expectedConfig), "Loading defaults unexpectedly created a config file");
        require(fs::is_regular_file(workingConfig), "Working-directory config was changed");

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

        fs::current_path(originalDirectory);
        fs::remove_all(testRoot);
        return 0;
    }
    catch (const std::exception& error) {
        fs::current_path(originalDirectory);
        std::error_code cleanupError;
        fs::remove_all(testRoot, cleanupError);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
