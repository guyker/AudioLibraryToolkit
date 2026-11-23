#include "Mp3TagMetadata.h"

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>
#include <taglib/flac/flacfile.h>
#include <taglib/mpeg/mpegfile.h>
#include <taglib/mpeg/id3v2/id3v2tag.h>
#include <taglib/mpeg/id3v2/id3v2frame.h>
#include <taglib/mpeg/id3v2/id3v2framefactory.h>
#include <taglib/mpeg/id3v2/frames/commentsframe.h>
#include <taglib/mpeg/id3v2/frames/textidentificationframe.h>
#include <taglib/mpeg/id3v2/frames/uniquefileidentifierframe.h>
#include <taglib/mpeg/id3v2/frames/unsynchronizedlyricsframe.h>
#include <taglib/mpeg/id3v2/frames/urllinkframe.h>
#include <taglib/ogg/xiphcomment.h>
#include <taglib/toolkit/tfilestream.h>
#include <taglib/toolkit/tpropertymap.h>
#include <taglib/toolkit/tdebuglistener.h>

#include "CommonUtils.h"

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

std::string frameIdentity(const TagLib::ID3v2::Frame& frame)
{
    const auto id = frame.frameID();
    std::string identity(id.data(), id.size());
    if (const auto* text = dynamic_cast<const TagLib::ID3v2::UserTextIdentificationFrame*>(&frame)) {
        return identity + ":" + toUtf8(text->description());
    }
    if (const auto* comments = dynamic_cast<const TagLib::ID3v2::CommentsFrame*>(&frame)) {
        return identity + ":" + toUtf8(comments->description());
    }
    if (const auto* url = dynamic_cast<const TagLib::ID3v2::UserUrlLinkFrame*>(&frame)) {
        return identity + ":" + toUtf8(url->description());
    }
    if (const auto* lyrics = dynamic_cast<const TagLib::ID3v2::UnsynchronizedLyricsFrame*>(&frame)) {
        return identity + ":" + toUtf8(lyrics->description());
    }
    if (const auto* identifier = dynamic_cast<const TagLib::ID3v2::UniqueFileIdentifierFrame*>(&frame)) {
        return identity + ":" + toUtf8(identifier->owner());
    }
    return identity;
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

struct MappedProperty
{
    TagLib::String key;
    TagLib::StringList values;
    TagLib::ByteVector frameId;
    std::string identity;
};

std::set<std::string> equivalentFrameIdentities(const MappedProperty& property)
{
    std::set<std::string> identities{ property.identity };
    auto key = toUtf8(property.key);
    for (char& character : key) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    if (key == "comment" || key == "description") {
        identities.insert("COMM:");
        identities.insert("TXXX:DESCRIPTION");
    }
    else if (key == "date" || key == "year") {
        identities.insert("TDRC");
        identities.insert("TXXX:DATE");
        identities.insert("TXXX:YEAR");
    }
    return identities;
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
    const std::filesystem::path& temporaryMp3, std::string& error)
{
    ensureTagLibLogListener();
    const ScopedTagLibSource sourceLog(sourceFlac);
    std::string sourceUtf8;
    std::string outputUtf8;
    const auto sourceName = tagLibFileName(sourceFlac, sourceUtf8);
    const auto outputName = tagLibFileName(temporaryMp3, outputUtf8);
    TagLib::FileStream sourceStream(sourceName, true);
    if (!sourceStream.isOpen()) {
        error = "TagLib could not open the FLAC source read-only.";
        return false;
    }
    TagLib::FLAC::File sourceFile(&sourceStream, false);
    if (!sourceFile.isValid()) {
        error = "TagLib could not parse the FLAC source.";
        return false;
    }
    auto* comments = sourceFile.xiphComment();
    if (comments == nullptr || comments->fieldListMap().isEmpty()) {
        return true;
    }

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

    std::map<std::string, MappedProperty> mappedProperties;
    auto* factory = TagLib::ID3v2::FrameFactory::instance();
    for (const auto& [sourceKey, sourceValues] : comments->fieldListMap()) {
        const auto propertyKey = normalizedPropertyKey(sourceKey);
        std::unique_ptr<TagLib::ID3v2::Frame> candidate(
            factory->createFrameForProperty(propertyKey, sourceValues));
        if (!candidate) {
            error = "TagLib could not map FLAC comment '" + toUtf8(sourceKey) +
                "' to an ID3 frame.";
            return false;
        }
        const auto identity = frameIdentity(*candidate);
        const auto [it, inserted] = mappedProperties.try_emplace(identity,
            MappedProperty{ propertyKey, sourceValues, candidate->frameID(), identity });
        if (!inserted) {
            it->second.values.append(sourceValues);
        }
    }

    std::set<std::string> identitiesToReplace;
    for (const auto& [identity, property] : mappedProperties) {
        (void)identity;
        const auto aliases = equivalentFrameIdentities(property);
        identitiesToReplace.insert(aliases.begin(), aliases.end());
    }
    std::vector<TagLib::ID3v2::Frame*> replacedFrames;
    for (auto* existing : outputTag->frameList()) {
        if (identitiesToReplace.contains(frameIdentity(*existing))) {
            replacedFrames.push_back(existing);
        }
    }
    for (auto* existing : replacedFrames) {
        outputTag->removeFrame(existing);
    }

    for (auto& [identity, property] : mappedProperties) {
        (void)identity;
        std::unique_ptr<TagLib::ID3v2::Frame> frame(
            factory->createFrameForProperty(property.key, property.values));
        if (!frame || frame->frameID() != property.frameId) {
            error = "TagLib could not create the mapped ID3 frame.";
            return false;
        }
        if (frameIdentity(*frame) != property.identity) {
            error = "TagLib mapped a FLAC comment to an inconsistent ID3 frame.";
            return false;
        }

        outputTag->addFrame(frame.release());
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
    return true;
}
