#include "Mp3TagIssues.h"

#include <algorithm>
#include <cctype>

#include <taglib/mpeg/id3v1/id3v1genres.h>

namespace
{
using namespace Mp3TagIssues;

std::string lowered(std::string value)
{
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

bool isNumericGenre(const Comment& comment)
{
    return lowered(comment.first) == "genre" && !comment.second.empty() && comment.second.size() <= 9 &&
        std::all_of(comment.second.begin(), comment.second.end(),
            [](char character) { return character >= '0' && character <= '9'; });
}

bool isReplayGain(const Comment& comment)
{
    const auto key = lowered(comment.first);
    return key.find("replaygain") != std::string::npos || key.compare(0, 5, "r128_") == 0;
}

std::vector<Comment> splitNul(const Comment& comment, Action action)
{
    std::vector<Comment> result;
    if (action == Action::Alternative) {
        std::string joined;
        for (const char character : comment.second) {
            if (character == '\0') joined += "; ";
            else joined += character;
        }
        result.emplace_back(comment.first, std::move(joined));
        return result;
    }
    std::size_t start = 0;
    for (;;) {
        const auto end = comment.second.find('\0', start);
        auto part = comment.second.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty()) result.emplace_back(comment.first, std::move(part));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

bool resolved(Action action)
{
    return action == Action::Apply || action == Action::Alternative;
}
}

namespace Mp3TagIssues
{
Found Detect(const FlacTagSource& source, bool sidecar)
{
    Found found;
    const auto mark = [&](Kind kind) { found.kinds[static_cast<std::size_t>(kind)] = true; };
    for (const auto& comment : source.comments) {
        if (comment.second.empty() && !sidecar) mark(Kind::EmptyValues);
        if (isNumericGenre(comment)) mark(Kind::NumericGenre);
        if (comment.second.find('\0') != std::string::npos) mark(Kind::EmbeddedNul);
        if (isReplayGain(comment)) mark(Kind::ReplayGain);
    }
    if (source.flacOnlyBlocks != 0 && !sidecar) mark(Kind::FlacOnlyBlocks);
    return found;
}

std::vector<Comment> Resolve(const std::vector<Comment>& comments, const Resolutions& resolutions)
{
    std::vector<Comment> result;
    result.reserve(comments.size());
    const auto nul = resolutions.Get(Kind::EmbeddedNul);
    for (const auto& comment : comments) {
        if (resolved(nul) && comment.second.find('\0') != std::string::npos) {
            for (auto& part : splitNul(comment, nul)) result.push_back(std::move(part));
        }
        else {
            result.push_back(comment);
        }
    }
    if (resolutions.Get(Kind::EmptyValues) == Action::Apply) {
        std::erase_if(result, [](const Comment& comment) { return comment.second.empty(); });
    }
    if (resolutions.Get(Kind::ReplayGain) == Action::Alternative) {
        std::erase_if(result, isReplayGain);
    }
    if (resolutions.Get(Kind::NumericGenre) == Action::Apply) {
        for (auto& comment : result) {
            if (!isNumericGenre(comment)) continue;
            const auto name = TagLib::ID3v1::genre(std::stoi(comment.second));
            // Numbers outside the ID3v1 list stay as written.
            if (!name.isEmpty()) comment.second = name.to8Bit(true);
        }
    }
    return result;
}

Found Changes(const std::vector<Comment>& comments, const Resolutions& resolutions)
{
    Found changes;
    for (const auto kind : AllKinds) {
        if (!resolved(resolutions.Get(kind))) continue;
        Resolutions single;
        single.Set(kind, resolutions.Get(kind));
        if (Resolve(comments, single) != comments) changes.kinds[static_cast<std::size_t>(kind)] = true;
    }
    return changes;
}

const char* Name(Kind kind)
{
    switch (kind) {
    case Kind::EmptyValues: return "Empty tag values";
    case Kind::NumericGenre: return "Numeric genre";
    case Kind::EmbeddedNul: return "Tag values containing NUL characters";
    case Kind::FlacOnlyBlocks: return "FLAC-only metadata blocks";
    default: return "ReplayGain/R128 tags";
    }
}

const char* Key(Kind kind)
{
    switch (kind) {
    case Kind::EmptyValues: return "emptyValues";
    case Kind::NumericGenre: return "numericGenre";
    case Kind::EmbeddedNul: return "embeddedNul";
    case Kind::FlacOnlyBlocks: return "flacOnlyBlocks";
    default: return "replayGain";
    }
}

const char* Explanation(Kind kind)
{
    switch (kind) {
    case Kind::EmptyValues:
        return "ID3v2 cannot store empty text values; without a metadata sidecar they would be lost.";
    case Kind::NumericGenre:
        return "A number in an ID3 genre is read by players as an ID3v1 genre (for example 17 = Rock).";
    case Kind::EmbeddedNul:
        return "ID3v2.4 uses NUL to separate values, so one FLAC value would become several.";
    case Kind::FlacOnlyBlocks:
        return "APPLICATION, CUESHEET or unknown FLAC blocks have no MP3 equivalent; without a sidecar they are lost.";
    default:
        return "ReplayGain/R128 values describe the FLAC audio and may be inaccurate for the lossy MP3.";
    }
}

std::vector<Choice> Choices(Kind kind)
{
    std::vector<Choice> choices;
    switch (kind) {
    case Kind::EmptyValues:
        choices.push_back({ Action::Apply, "Drop the empty values" });
        break;
    case Kind::NumericGenre:
        choices.push_back({ Action::Apply, "Convert ID3v1 genre numbers to names (other numbers kept)" });
        choices.push_back({ Action::Alternative, "Keep the numbers as written" });
        break;
    case Kind::EmbeddedNul:
        choices.push_back({ Action::Apply, "Split into separate values" });
        choices.push_back({ Action::Alternative, "Replace NUL with \"; \" and keep one value" });
        break;
    case Kind::FlacOnlyBlocks:
        choices.push_back({ Action::Apply, "Discard them from the MP3 (source unchanged)" });
        break;
    default:
        choices.push_back({ Action::Apply, "Keep the tags" });
        choices.push_back({ Action::Alternative, "Remove the tags from the MP3" });
        return choices;
    }
    choices.push_back({ Action::SkipFiles, "Skip the affected files" });
    choices.push_back({ Action::Stop, "Stop without converting" });
    return choices;
}

const char* ActionLabel(Kind kind, Action action)
{
    if (action == Action::Unresolved) return "Not resolved";
    for (const auto& choice : Choices(kind)) {
        if (choice.action == action) return choice.label;
    }
    return "Not resolved";
}
}
