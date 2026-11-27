#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include "ConversionOptions.h"

namespace FFmpegRuntime {
bool IsBundledSelection(std::wstring_view selection);
bool IsValidSelection(std::wstring_view selection);
bool ValidateLibraries();
bool ResolveEncoder(std::wstring_view selection, AudioTarget target, std::filesystem::path& executable);
#ifdef _WIN32
std::wstring QuoteWindowsArgument(const std::wstring& argument);
#endif
}
