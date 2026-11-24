#include "Mp3TagMetadata.h"

#include <mutex>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>
#include <taglib/flac/flacfile.h>
#include <taglib/mpeg/mpegfile.h>
#include <taglib/mpeg/id3v2/id3v2tag.h>
#include <taglib/mpeg/id3v2/frames/textidentificationframe.h>
#include <taglib/ogg/xiphcomment.h>
#include <taglib/toolkit/tfilestream.h>
#include <taglib/toolkit/tpropertymap.h>
#include <taglib/toolkit/tdebuglistener.h>

#include "CommonUtils.h"
#include "PreservationAudit.h"

namespace
{
thread_local std::string tagLibSourcePath;

class TagLibLogListener final : public TagLib::DebugListener
{
public:
    void printMessage(const TagLib::String& message) override
    {
        auto text = message.to8Bit(true);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        spdlog::debug("TagLib [{}]: {}", tagLibSourcePath.empty() ? "unscoped" : tagLibSourcePath,
            text);
    }
};

class ScopedTagLibSource
{
public:
    explicit ScopedTagLibSource(const std::filesystem::path& path)
        : previous_(std::move(tagLibSourcePath))
    {
        tagLibSourcePath = CommonUtils::path_to_utf8(path);
    }

    ~ScopedTagLibSource()
    {
        tagLibSourcePath = std::move(previous_);
    }

private:
    std::string previous_;
};

void ensureTagLibLogListener()
{
    static TagLibLogListener listener;
    static std::once_flag registered;
    std::call_once(registered, [] { TagLib::setDebugListener(&listener); });
}

std::string toUtf8(const TagLib::String& value)
{
    return value.to8Bit(true);
}

TagLib::FileName tagLibFileName(const std::filesystem::path& path, std::string& utf8Path)
{
#ifdef _WIN32
    (void)utf8Path;
    return TagLib::FileName(path.c_str());
#else
    utf8Path = CommonUtils::path_to_utf8(path);
    return TagLib::FileName(utf8Path.c_str());
#endif
}

TagLib::String normalizedPropertyKey(const TagLib::String& key)
{
    auto normalized = toUtf8(key);
    for (char& character : normalized) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    if (normalized == "DESCRIPTION") {
        return "COMMENT";
    }
    if (normalized == "YEAR") {
        return "DATE";
    }
    return key;
}
}

bool Mp3TagMetadata::WriteFromFlac(const std::filesystem::path& sourceFlac,
    const std::filesystem::path& temporaryMp3, const Mp3TagIssues::Resolutions& resolutions,
    std::string& error)
{
    ensureTagLibLogListener();
    const ScopedTagLibSource sourceLog(sourceFlac);
    // Native comments keep empty values and file order, which TagLib's Xiph map does not expose.
    Mp3TagIssues::FlacTagSource source;
    if (!PreservationAudit::ReadFlacTagSource(sourceFlac, source, error)) {
        error = "Could not read the FLAC source comments: " + error;
        return false;
    }
    if (source.comments.empty()) {
        return true;
    }

    std::string outputUtf8;
    const auto outputName = tagLibFileName(temporaryMp3, outputUtf8);
    TagLib::MPEG::File outputFile(outputName, false, TagLib::MPEG::Properties::Fast);
    if (!outputFile.isValid()) {
        error = "TagLib could not open the temporary MP3 for tag writing.";
        return false;
    }
    auto* outputTag = outputFile.ID3v2Tag(true);
    if (outputTag == nullptr) {
        error = "TagLib could not create an ID3v2 tag in the temporary MP3.";
        return false;
    }

    TagLib::PropertyMap properties;
    for (const auto& [key, value] : Mp3TagIssues::Resolve(source.comments, resolutions)) {
        // ID3v2 cannot store empty text values; unresolved empties are reported by the audit.
        if (value.empty()) continue;
        properties.insert(normalizedPropertyKey(TagLib::String(key, TagLib::String::UTF8)),
            TagLib::StringList(TagLib::String(value, TagLib::String::UTF8)));
    }

    // Clear FFmpeg text properties first; TagLib retains non-property frames such as APIC and CHAP/CTOC.
    if (!outputTag->setProperties(TagLib::PropertyMap{}).isEmpty()) {
        error = "TagLib could not clear the encoded MP3 text properties.";
        return false;
    }
    if (!outputTag->setProperties(properties).isEmpty()) {
        error = "TagLib could not represent all FLAC comments as ID3 properties.";
        return false;
    }

    if (!outputFile.save(TagLib::MPEG::File::ID3v2, TagLib::MPEG::File::StripOthers,
        TagLib::ID3v2::v4, TagLib::MPEG::File::DoNotDuplicate)) {
        error = "TagLib could not save the ID3v2.4 tags to the temporary MP3.";
        return false;
    }
    return true;
}

bool Mp3TagMetadata::Read(const std::filesystem::path& mp3, TagMap& tags, std::string& error)
{
    ensureTagLibLogListener();
    const ScopedTagLibSource sourceLog(mp3);
    std::string utf8Path;
    const auto name = tagLibFileName(mp3, utf8Path);
    TagLib::MPEG::File file(name, false, TagLib::MPEG::Properties::Fast);
    if (!file.isValid()) {
        error = "TagLib could not read the MP3 output.";
        return false;
    }

    tags.clear();
    const auto* tag = file.ID3v2Tag();
    if (tag == nullptr) {
        return true;
    }
    const auto properties = tag->properties();
    for (auto property = properties.begin(); property != properties.end(); ++property) {
        auto& values = tags[toUtf8(property->first)];
        for (const auto& value : property->second) {
            values.push_back(toUtf8(value));
        }
    }
    // properties() turns numeric genres into names and rewrites date separators; audit the stored text.
    const auto& frames = tag->frameListMap();
    for (const auto& [frameId, key] : { std::pair<const char*, const char*>{ "TCON", "GENRE" },
             std::pair<const char*, const char*>{ "TDRC", "DATE" } }) {
        const auto found = frames.find(frameId);
        if (found == frames.end()) continue;
        std::vector<std::string> raw;
        for (const auto* frame : found->second) {
            const auto* text = dynamic_cast<const TagLib::ID3v2::TextIdentificationFrame*>(frame);
            if (text == nullptr) continue;
            for (const auto& value : text->fieldList()) raw.push_back(toUtf8(value));
        }
        if (!raw.empty()) tags[key] = std::move(raw);
    }
    return true;
}
