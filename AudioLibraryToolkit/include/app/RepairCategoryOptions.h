#pragma once

#include "RepairIntegrity.h"
#include "rapidjson/fwd.h"

#include <array>
#include <optional>

namespace RepairCategoryOptions
{
using RepairIntegrity::Categories;
using RepairIntegrity::Mode;

struct Field
{
    const char* key;
    Mode Categories::*member;
    bool canFix;

    const char* Choices() const { return canFix ? "off|detect|fix" : "off|detect"; }
    bool Parse(std::string_view text, Mode& mode) const
    {
        return RepairIntegrity::ParseMode(text, mode) && (canFix || mode != Mode::Fix);
    }
};

inline constexpr std::array<Field, 4> Fields{ {
    { "metadata", &Categories::metadata, true },
    { "integrity", &Categories::integrity, true },
    { "container", &Categories::container, false },
    { "decode", &Categories::decode, false }
} };

struct Overrides
{
    std::array<std::optional<Mode>, Fields.size()> modes{};

    Categories Resolve(Categories base) const
    {
        for (std::size_t i = 0; i < Fields.size(); ++i) {
            if (modes[i]) base.*Fields[i].member = *modes[i];
        }
        return base;
    }

    bool Read(const rapidjson::Value& parent, const char* key, std::string& error);
};

inline std::string Summary(const Categories& categories)
{
    return "Repair categories: " + RepairIntegrity::Describe(categories) +
        "\n  Metadata fixes preserve the copied audio byte range. Integrity fixes may change file bytes,"
        "\n  but require unchanged compressed frames and decoded PCM. No audio is re-encoded.";
}
}
