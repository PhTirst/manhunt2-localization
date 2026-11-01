#include "mh2_font_preview.hpp"

#include "mh2_dds.hpp"
#include "mh2_font_dat.hpp"
#include "mh2_tex.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace mh2
{
    namespace
    {
        struct PreviewGlyph
        {
            bool missing = false;
            int advance = 0;
            int width = 0;
            int height = 0;
            int sourceX = 0;
            int sourceY = 0;
        };

        void WriteU16(std::ofstream &file, uint16_t value)
        {
            file.put(static_cast<char>(value & 0xFF));
            file.put(static_cast<char>((value >> 8) & 0xFF));
        }

        void WriteU32(std::ofstream &file, uint32_t value)
        {
            file.put(static_cast<char>(value & 0xFF));
            file.put(static_cast<char>((value >> 8) & 0xFF));
            file.put(static_cast<char>((value >> 16) & 0xFF));
            file.put(static_cast<char>((value >> 24) & 0xFF));
        }

        void WriteS32(std::ofstream &file, int32_t value)
        {
            WriteU32(file, static_cast<uint32_t>(value));
        }

        void WriteBmp(
            const std::filesystem::path &path,
            uint32_t width,
            uint32_t height,
            const std::vector<uint8_t> &bgra)
        {
            if (width == 0 || height == 0 || bgra.size() != static_cast<size_t>(width) * height * 4)
                throw std::runtime_error("invalid preview bitmap dimensions");
            if (!path.parent_path().empty())
                std::filesystem::create_directories(path.parent_path());

            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
                throw std::runtime_error("failed to write font preview: " + path.string());

            const uint32_t pixelBytes = width * height * 4;
            WriteU16(file, 0x4D42);
            WriteU32(file, 14 + 40 + pixelBytes);
            WriteU16(file, 0);
            WriteU16(file, 0);
            WriteU32(file, 14 + 40);

            WriteU32(file, 40);
            WriteS32(file, static_cast<int32_t>(width));
            WriteS32(file, -static_cast<int32_t>(height));
            WriteU16(file, 1);
            WriteU16(file, 32);
            WriteU32(file, 0);
            WriteU32(file, pixelBytes);
            WriteS32(file, 2835);
            WriteS32(file, 2835);
            WriteU32(file, 0);
            WriteU32(file, 0);
            file.write(reinterpret_cast<const char *>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
        }

        const FontDatSection &FindSection(const FontDatFile &font, uint32_t fontId)
        {
            auto it = std::find_if(font.sections.begin(), font.sections.end(), [fontId](const FontDatSection &section) {
                return section.fontId == fontId;
            });
            if (it == font.sections.end())
                throw std::runtime_error("font.dat does not contain font id " + std::to_string(fontId));
            return *it;
        }

        const TexTextureRecord &FindTexture(const TexFile &tex, uint32_t fontId, const std::string &requestedName)
        {
            std::string name = requestedName;
            if (name.empty())
            {
                if (fontId == 1)
                    name = "font2";
                else if (fontId == 2)
                    name = "font1";
                else
                    name = "font" + std::to_string(fontId);
            }
            auto it = std::find_if(tex.records.begin(), tex.records.end(), [&name](const TexTextureRecord &record) {
                return record.name == name;
            });
            if (it == tex.records.end())
                throw std::runtime_error("TEX does not contain texture " + name);
            return *it;
        }

        PreviewGlyph ResolveGlyph(
            const FontDatSection &section,
            char16_t code,
            uint32_t atlasWidth,
            uint32_t atlasHeight,
            bool flipV)
        {
            PreviewGlyph result;
            result.advance = std::max(1, static_cast<int>(std::lround(section.fontSize * 0.5f)));

            const uint32_t value = static_cast<uint32_t>(code);
            if (value >= section.fmap.size())
            {
                result.missing = true;
                return result;
            }

            const int32_t index = section.fmap[value];
            if (index < 0 || static_cast<size_t>(index) >= section.chars.size())
            {
                result.missing = true;
                return result;
            }

            const FontDatChar &ch = section.chars[static_cast<size_t>(index)];
            if (ch.code != value)
                throw std::runtime_error("fmap points to a different character for U+" + std::to_string(value));

            result.advance = std::max(1, static_cast<int>(std::lround(ch.widthScale * 512.0f)));
            result.sourceX = std::clamp(
                static_cast<int>(std::lround(ch.x1 * static_cast<float>(atlasWidth))),
                0,
                static_cast<int>(atlasWidth));
            const int y1 = std::clamp(
                static_cast<int>(std::lround(ch.y1 * static_cast<float>(atlasHeight))),
                0,
                static_cast<int>(atlasHeight));
            const int x2 = std::clamp(
                static_cast<int>(std::lround(ch.x2 * static_cast<float>(atlasWidth))),
                0,
                static_cast<int>(atlasWidth));
            const int y2 = std::clamp(
                static_cast<int>(std::lround(ch.y2 * static_cast<float>(atlasHeight))),
                0,
                static_cast<int>(atlasHeight));
            result.width = std::max(0, x2 - result.sourceX);
            result.height = std::max(0, y2 - y1);
            result.sourceY = flipV ? static_cast<int>(atlasHeight) - y2 : y1;
            return result;
        }
    }

    FontGlyphInspection InspectFontGlyph(
        const std::filesystem::path &fontDatPath,
        const std::filesystem::path &texPath,
        uint32_t fontId,
        char16_t code,
        const std::string &textureName,
        bool flipV)
    {
        FontDatFile font = ReadFontDat(fontDatPath);
        const FontDatSection &section = FindSection(font, fontId);
        TexFile tex = ReadTex(texPath);
        const TexTextureRecord &record = FindTexture(tex, fontId, textureName);
        std::vector<uint8_t> dds(
            tex.decompressed.begin() + static_cast<std::ptrdiff_t>(record.dataOffset),
            tex.decompressed.begin() + static_cast<std::ptrdiff_t>(record.dataOffset + record.dataSize));
        uint32_t atlasWidth = 0;
        uint32_t atlasHeight = 0;
        std::vector<uint8_t> atlas = DecodeDxt5DdsFirstMip(dds, atlasWidth, atlasHeight);

        PreviewGlyph glyph = ResolveGlyph(section, code, atlasWidth, atlasHeight, flipV);
        FontGlyphInspection result;
        result.missing = glyph.missing;
        result.advance = glyph.advance;
        result.sourceX = glyph.sourceX;
        result.sourceY = glyph.sourceY;
        result.width = glyph.width;
        result.height = glyph.height;
        if (glyph.missing || glyph.width <= 0 || glyph.height <= 0)
            return result;

        auto include = [](int x, int y, int &minX, int &minY, int &maxX, int &maxY) {
            if (minX < 0 || x < minX)
                minX = x;
            if (minY < 0 || y < minY)
                minY = y;
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        };

        for (int y = 0; y < glyph.height; y++)
        {
            for (int x = 0; x < glyph.width; x++)
            {
                const size_t src =
                    (static_cast<size_t>(glyph.sourceY + y) * atlasWidth + glyph.sourceX + x) * 4;
                const uint8_t alpha = atlas[src + 3];
                if (y == 0)
                    result.topRowMaxAlpha = std::max(result.topRowMaxAlpha, alpha);
                if (y == glyph.height - 1)
                    result.bottomRowMaxAlpha = std::max(result.bottomRowMaxAlpha, alpha);
                if (alpha > 0)
                    include(x, y, result.inkMinX, result.inkMinY, result.inkMaxX, result.inkMaxY);
                if (alpha >= 32)
                    include(x, y, result.solidMinX, result.solidMinY, result.solidMaxX, result.solidMaxY);
            }
        }
        return result;
    }

    void RenderFontPreview(
        const std::filesystem::path &fontDatPath,
        const std::filesystem::path &texPath,
        uint32_t fontId,
        const std::u16string &text,
        const std::filesystem::path &outputPath,
        uint32_t scale,
        const std::string &textureName,
        bool flipV)
    {
        if (scale == 0 || scale > 16)
            throw std::runtime_error("preview scale must be between 1 and 16");

        FontDatFile font = ReadFontDat(fontDatPath);
        const FontDatSection &section = FindSection(font, fontId);
        TexFile tex = ReadTex(texPath);
        const TexTextureRecord &record = FindTexture(tex, fontId, textureName);
        std::vector<uint8_t> dds(
            tex.decompressed.begin() + static_cast<std::ptrdiff_t>(record.dataOffset),
            tex.decompressed.begin() + static_cast<std::ptrdiff_t>(record.dataOffset + record.dataSize));
        uint32_t atlasWidth = 0;
        uint32_t atlasHeight = 0;
        std::vector<uint8_t> atlas = DecodeDxt5DdsFirstMip(dds, atlasWidth, atlasHeight);

        std::vector<std::vector<PreviewGlyph>> lines(1);
        std::vector<int> lineWidths(1, 0);
        int maxGlyphHeight = 1;
        for (char16_t code : text)
        {
            if (code == u'\r')
                continue;
            if (code == u'\n')
            {
                lines.emplace_back();
                lineWidths.push_back(0);
                continue;
            }
            PreviewGlyph glyph = ResolveGlyph(section, code, atlasWidth, atlasHeight, flipV);
            maxGlyphHeight = std::max(maxGlyphHeight, glyph.height);
            lineWidths.back() += glyph.advance;
            lines.back().push_back(glyph);
        }

        const int padding = 4;
        const int lineHeight = std::max(
            maxGlyphHeight,
            static_cast<int>(std::lround(section.heightScale * 512.0f)));
        const int logicalWidth = std::max(1, *std::max_element(lineWidths.begin(), lineWidths.end()) + padding * 2);
        const int logicalHeight = std::max(1, static_cast<int>(lines.size()) * lineHeight + padding * 2);
        if (logicalWidth > std::numeric_limits<int>::max() / static_cast<int>(scale) ||
            logicalHeight > std::numeric_limits<int>::max() / static_cast<int>(scale))
        {
            throw std::runtime_error("font preview dimensions overflow");
        }

        const uint32_t width = static_cast<uint32_t>(logicalWidth) * scale;
        const uint32_t height = static_cast<uint32_t>(logicalHeight) * scale;
        std::vector<uint8_t> preview(static_cast<size_t>(width) * height * 4, 0);
        for (size_t i = 0; i < preview.size(); i += 4)
        {
            preview[i + 0] = 72;
            preview[i + 1] = 72;
            preview[i + 2] = 72;
            preview[i + 3] = 255;
        }

        for (size_t lineIndex = 0; lineIndex < lines.size(); lineIndex++)
        {
            int cursorX = padding;
            const int cursorY = padding + static_cast<int>(lineIndex) * lineHeight;
            for (const PreviewGlyph &glyph : lines[lineIndex])
            {
                if (glyph.missing)
                {
                    for (int y = 0; y < lineHeight; y++)
                    {
                        for (int x = 0; x < glyph.advance; x++)
                        {
                            if (x != 0 && y != 0 && x != glyph.advance - 1 && y != lineHeight - 1)
                                continue;
                            for (uint32_t sy = 0; sy < scale; sy++)
                            {
                                for (uint32_t sx = 0; sx < scale; sx++)
                                {
                                    const uint32_t dx = static_cast<uint32_t>(cursorX + x) * scale + sx;
                                    const uint32_t dy = static_cast<uint32_t>(cursorY + y) * scale + sy;
                                    uint8_t *dst = preview.data() + (static_cast<size_t>(dy) * width + dx) * 4;
                                    dst[0] = 255;
                                    dst[1] = 0;
                                    dst[2] = 255;
                                }
                            }
                        }
                    }
                }
                else
                {
                    for (int y = 0; y < glyph.height; y++)
                    {
                        for (int x = 0; x < glyph.width; x++)
                        {
                            const size_t src = (static_cast<size_t>(glyph.sourceY + y) * atlasWidth + glyph.sourceX + x) * 4;
                            const uint8_t alpha = atlas[src + 3];
                            if (alpha == 0)
                                continue;
                            for (uint32_t sy = 0; sy < scale; sy++)
                            {
                                for (uint32_t sx = 0; sx < scale; sx++)
                                {
                                    const uint32_t dx = static_cast<uint32_t>(cursorX + x) * scale + sx;
                                    const uint32_t dy = static_cast<uint32_t>(cursorY + y) * scale + sy;
                                    if (dx >= width || dy >= height)
                                        continue;
                                    uint8_t *dst = preview.data() + (static_cast<size_t>(dy) * width + dx) * 4;
                                    for (int channel = 0; channel < 3; channel++)
                                    {
                                        dst[channel] = static_cast<uint8_t>(
                                            (static_cast<uint32_t>(atlas[src + channel]) * alpha +
                                                static_cast<uint32_t>(dst[channel]) * (255U - alpha) + 127U) /
                                            255U);
                                    }
                                }
                            }
                        }
                    }
                }
                cursorX += glyph.advance;
            }
        }

        WriteBmp(outputPath, width, height, preview);
    }
}
