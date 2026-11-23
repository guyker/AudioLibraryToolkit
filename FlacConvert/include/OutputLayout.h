#pragma once

#include <filesystem>

#include "ConversionOptions.h"

namespace OutputLayout
{
inline std::filesystem::path Conversion(const std::filesystem::path& root,
    AudioTarget target, const std::filesystem::path& library)
{
    return root / (target == AudioTarget::Mp3 ? "converted-mp3" : "normalized-flac") / library;
}

inline std::filesystem::path Catalog(const std::filesystem::path& root,
    const std::filesystem::path& library)
{
    return root / "catalog-json" / library;
}

inline std::filesystem::path Database(const std::filesystem::path& root,
    const std::filesystem::path& library)
{
    return root / "catalog-sqlite" / library;
}

inline std::filesystem::path Repair(const std::filesystem::path& root,
    const std::filesystem::path& library)
{
    return root / "repaired-metadata" / library;
}

inline std::filesystem::path Quarantine(const std::filesystem::path& root)
{
    return root / "quarantine";
}
}
