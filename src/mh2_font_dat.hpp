#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace mh2
{
    struct FontDatChar
    {
        uint32_t code = 0;
        float widthScale = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;
        float x2 = 0.0f;
        float y2 = 0.0f;
    };

    struct FontDatSection
    {
        uint32_t fontId = 0;
        uint32_t fontSize = 0;
        float heightScale = 0.0f;
        std::vector<int32_t> fmap;
        std::vector<FontDatChar> chars;
    };

    struct FontDatFile
    {
        std::vector<FontDatSection> sections;
    };

    FontDatFile ReadFontDat(const std::filesystem::path &path);
    void WriteFontDat(const std::filesystem::path &path, const FontDatFile &fontDat);
    void RebuildDirectFmap(FontDatSection &section);
}
