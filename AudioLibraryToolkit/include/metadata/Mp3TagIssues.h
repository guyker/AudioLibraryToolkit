#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// FLAC metadata conditions that change MP3 tags or would stop the fail-closed preservation audit,
// each with resolutions chosen per run (one choice per kind, applied to every affected file).
namespace Mp3TagIssues
{
enum class Kind { EmptyValues, NumericGenre, EmbeddedNul, FlacOnlyBlocks, ReplayGain };
inline constexpr std::size_t KindCount = 5;
inline constexpr std::array<Kind, KindCount> AllKinds{ Kind::EmptyValues, Kind::NumericGenre,
    Kind::EmbeddedNul, Kind::FlacOnlyBlocks, Kind::ReplayGain };

// Apply is the recommended resolution; Alternative is offered only where Choices lists it.
// Unresolved keeps the writer's default behaviour and leaves any difference to the fail-closed audit.
enum class Action { Unresolved, Apply, Alternative, SkipFiles, Stop };

struct Resolutions
{
    std::array<Action, KindCount> actions{};
    Action Get(Kind kind) const { return actions[static_cast<std::size_t>(kind)]; }
    void Set(Kind kind, Action action) { actions[static_cast<std::size_t>(kind)] = action; }
};

struct Found
{
    std::array<bool, KindCount> kinds{};
    bool Has(Kind kind) const { return kinds[static_cast<std::size_t>(kind)]; }
    bool Any() const
    {
        for (const bool found : kinds) if (found) return true;
        return false;
    }
};

using Comment = std::pair<std::string, std::string>;

// Native FLAC Vorbis comments in file order and the number of APPLICATION, CUESHEET and unknown blocks.
struct FlacTagSource
{
    std::vector<Comment> comments;
    std::size_t flacOnlyBlocks{ 0 };
};

struct Choice
{
    Action action;
    const char* label;
};

// With a verified sidecar, empty values and FLAC-only blocks are already preserved and are not reported.
Found Detect(const FlacTagSource& source, bool sidecar);
// Applies the resolved (Apply/Alternative) comment transformations; other kinds are left unchanged.
std::vector<Comment> Resolve(const std::vector<Comment>& comments, const Resolutions& resolutions);
// The resolved kinds that change at least one of these comments.
Found Changes(const std::vector<Comment>& comments, const Resolutions& resolutions);
const char* Name(Kind kind);
const char* Key(Kind kind);
const char* Explanation(Kind kind);
// Recommended choice first.
std::vector<Choice> Choices(Kind kind);
const char* ActionLabel(Kind kind, Action action);
}
