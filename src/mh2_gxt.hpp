#pragma once

#include "json.hpp"
#include "unicode.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mh2
{
    struct GxtEntry
    {
        uint32_t offset = 0;
        std::string key;
        uint32_t extra = 0;
        std::u16string value;
    };

    struct GxtFile
    {
        std::vector<GxtEntry> entries;
        bool hasTdat = false;
    };

    GxtFile ReadGxt(const std::filesystem::path &path);
    void WriteGxt(const std::filesystem::path &path, const GxtFile &gxt);

    JsonValue GxtToJson(const GxtFile &gxt, const std::string &language);
    GxtFile JsonToGxt(const JsonValue &json, const std::string &language, bool allowLegacyTextFields = true);

    std::string FormatHex32(uint32_t value);
    uint32_t ParseExtraValue(const JsonValue &value);
}
