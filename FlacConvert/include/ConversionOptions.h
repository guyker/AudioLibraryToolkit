#pragma once

#include "Mp3TagIssues.h"

enum class AudioTarget { Flac, Mp3 };

// Effective sidecar format for one conversion run, resolved from MetadataSidecars settings.
enum class MetadataSidecarFormat { None, Binary, Json };

struct ConversionOptions
{
    AudioTarget target{ AudioTarget::Flac };
    int mp3Quality{ 0 };
    bool downmixToStereo{ false };
    MetadataSidecarFormat sidecarFormat{ MetadataSidecarFormat::Binary };
    // MP3 only; chosen before execution from the plan's detected tag issues.
    Mp3TagIssues::Resolutions tagIssues{};
};
