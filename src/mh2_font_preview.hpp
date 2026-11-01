#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace mh2
{
    struct FontGlyphInspection
    {
        bool missing = false;
        int advance = 0;
        int sourceX = 0;
        int sourceY = 0;
        int width = 0;
        int height = 0;
        int inkMinX = -1;
        int inkMinY = -1;
        int inkMaxX = -1;
        int inkMaxY = -1;
        int solidMinX = -1;
        int solidMinY = -1;
        int solidMaxX = -1;
        int solidMaxY = -1;
        uint8_t topRowMaxAlpha = 0;
        uint8_t bottomRowMaxAlpha = 0;
    };

    FontGlyphInspection InspectFontGlyph(
        const std::filesystem::path &fontDatPath,
        const std::filesystem::path &texPath,
        uint32_t fontId,
        char16_t code,
        const std::string &textureName = {},
        bool flipV = false);

    void RenderFontPreview(
        const std::filesystem::path &fontDatPath,
        const std::filesystem::path &texPath,
        uint32_t fontId,
        const std::u16string &text,
        const std::filesystem::path &outputPath,
        uint32_t scale,
        const std::string &textureName = {},
        bool flipV = false);
}
