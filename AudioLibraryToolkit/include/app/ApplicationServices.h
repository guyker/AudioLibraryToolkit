#pragma once

#include "CommandLine.h"

#include <filesystem>
#include <vector>

namespace AudioRepair { struct Plan; }
struct MediaDirectoryElement;

namespace ApplicationServices
{
int Run(const CommandLineOptions& options);
}

namespace ApplicationServiceOperations
{
int Scan(const std::filesystem::path& input, const std::filesystem::path& outputRoot,
    bool archiveFlacMetadata, bool fullRescan);
int FindDuplicates(const std::filesystem::path& jsonDirectory, bool openFolders,
    const std::filesystem::path& reportPath,
    const std::vector<std::filesystem::path>& quarantinePaths,
    const std::filesystem::path& outputRoot);
int ImportDatabase(const std::filesystem::path& jsonDirectory,
    const std::filesystem::path& outputRoot);
void PrintRepairFindings(const AudioRepair::Plan& plan);
bool ValidateRepairOutput(const std::filesystem::path& output,
    const std::vector<MediaDirectoryElement>& libraries);
}
