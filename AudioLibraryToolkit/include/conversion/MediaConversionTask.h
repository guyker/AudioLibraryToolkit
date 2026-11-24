#pragma once

#include <iostream>
#include <filesystem>
#include <array>
#include <algorithm>
#include <mutex>
#include <future>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

#include <chrono>
#include <thread>
#include "ConversionOptions.h"
#include "PreservationAudit.h"


namespace fs = std::filesystem;


class MediaConversionTask {
public:
    using ProgressCallback = std::function<void(std::optional<double>, std::string_view)>;
    MediaConversionTask() = delete;
    MediaConversionTask(const fs::path sourcePath, const fs::path targetPath, const fs::path targetTMPPath,
        bool overwriteTarget = true, ConversionOptions options = {}) :
        _sourcePath(sourcePath), _targetPath(targetPath), _targetTMPPath(targetTMPPath),
        _overwriteTarget(overwriteTarget), _options(options), _status(0) {}

    virtual int Run();
    virtual int PostRun();

    virtual int GetStatus() { return _status; }
    static bool InstallOutputFile(const fs::path& temporary, const fs::path& target, bool overwrite);
    const PreservationAudit::Result& GetPreservationAudit() const { return _audit; }
    const char* GetFailureType() const { return _failureType; }
    void SetProgressCallback(ProgressCallback callback) { _progressCallback = std::move(callback); }

    virtual ~MediaConversionTask() {}

protected:

    virtual int ConvertFile();
    virtual int InstallTemporaryOutput();

    const fs::path _sourcePath;
    const fs::path _targetPath;
    const fs::path _targetTMPPath;
    const bool _overwriteTarget;
    const ConversionOptions _options;
    bool _preserveOriginalMetadata{ false };
    PreservationAudit::Result _audit;
    const char* _failureType{ "AUDIO" };
    ProgressCallback _progressCallback;

    int _status;
};
