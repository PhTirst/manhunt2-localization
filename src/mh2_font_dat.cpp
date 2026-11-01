#include "mh2_font_dat.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace mh2
{
    namespace
    {
        uint32_t ReadU32(const std::vector<uint8_t> &data, size_t offset)
        {
            if (offset + 4 > data.size())
                throw std::runtime_error("unexpected end of font.dat while reading u32");
            return static_cast<uint32_t>(data[offset]) |
                (static_cast<uint32_t>(data[offset + 1]) << 8) |
                (static_cast<uint32_t>(data[offset + 2]) << 16) |
                (static_cast<uint32_t>(data[offset + 3]) << 24);
        }

        int32_t ReadS32(const std::vector<uint8_t> &data, size_t offset)
        {
            return static_cast<int32_t>(ReadU32(data, offset));
        }

        float ReadF32(const std::vector<uint8_t> &data, size_t offset)
        {
            uint32_t raw = ReadU32(data, offset);
            float value = 0.0f;
            static_assert(sizeof(value) == sizeof(raw), "float must be 32-bit");
            std::memcpy(&value, &raw, sizeof(value));
            return value;
        }

        void WriteU32(std::vector<uint8_t> &out, uint32_t value)
        {
            out.push_back(static_cast<uint8_t>(value & 0xFF));
            out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        }

        void WriteS32(std::vector<uint8_t> &out, int32_t value)
        {
            WriteU32(out, static_cast<uint32_t>(value));
        }

        void WriteF32(std::vector<uint8_t> &out, float value)
        {
            uint32_t raw = 0;
            static_assert(sizeof(value) == sizeof(raw), "float must be 32-bit");
            std::memcpy(&raw, &value, sizeof(raw));
            WriteU32(out, raw);
        }

        std::vector<uint8_t> ReadAllBytes(const std::filesystem::path &path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                throw std::runtime_error("failed to open font.dat: " + path.string());
            file.seekg(0, std::ios::end);
            std::streamoff size = file.tellg();
            if (size < 0)
                throw std::runtime_error("failed to determine font.dat size: " + path.string());
            file.seekg(0, std::ios::beg);
            std::vector<uint8_t> data(static_cast<size_t>(size));
            if (!data.empty())
                file.read(reinterpret_cast<char *>(&data[0]), size);
            return data;
        }

        void WriteAllBytes(const std::filesystem::path &path, const std::vector<uint8_t> &data)
        {
            if (!path.parent_path().empty())
                std::filesystem::create_directories(path.parent_path());
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
                throw std::runtime_error("failed to write font.dat: " + path.string());
            if (!data.empty())
                file.write(reinterpret_cast<const char *>(&data[0]), static_cast<std::streamsize>(data.size()));
        }

        bool HasFontTag(const std::vector<uint8_t> &data, size_t offset)
        {
            static const char tag[] = "<FONT>";
            if (offset + 6 > data.size())
                return false;
            return std::equal(tag, tag + 6, data.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }

    FontDatFile ReadFontDat(const std::filesystem::path &path)
    {
        std::vector<uint8_t> data = ReadAllBytes(path);
        FontDatFile result;
        size_t offset = 0;
        while (offset < data.size())
        {
            if (!HasFontTag(data, offset))
                throw std::runtime_error("font.dat section is missing <FONT> tag at offset " + std::to_string(offset));

            FontDatSection section;
            size_t p = offset + 6;
            uint32_t fmapField = ReadU32(data, p); p += 4;
            p += 4; // zero
            section.fontId = ReadU32(data, p); p += 4;
            uint32_t charCount = ReadU32(data, p); p += 4;
            section.fontSize = ReadU32(data, p); p += 4;
            section.heightScale = ReadF32(data, p); p += 4;

            size_t fmapCount = static_cast<size_t>(fmapField) + 1;
            if (fmapCount > 65536 || charCount > 65536 || p > data.size() ||
                fmapCount * 4 + static_cast<size_t>(charCount) * 24 > data.size() - p)
                throw std::runtime_error("font.dat map/character counts exceed the payload or BMP range");
            if (!std::isfinite(section.heightScale) || section.fontSize == 0 || section.fontSize > 4096)
                throw std::runtime_error("font.dat has invalid font metrics");
            section.fmap.reserve(fmapCount);
            for (size_t i = 0; i < fmapCount; i++, p += 4)
                section.fmap.push_back(ReadS32(data, p));

            section.chars.reserve(charCount);
            for (uint32_t i = 0; i < charCount; i++)
            {
                FontDatChar ch;
                ch.code = ReadU32(data, p); p += 4;
                ch.widthScale = ReadF32(data, p); p += 4;
                ch.x1 = ReadF32(data, p); p += 4;
                ch.y1 = ReadF32(data, p); p += 4;
                ch.x2 = ReadF32(data, p); p += 4;
                ch.y2 = ReadF32(data, p); p += 4;
                if (ch.code > 0xFFFF || !std::isfinite(ch.widthScale) || !std::isfinite(ch.x1) || !std::isfinite(ch.y1) ||
                    !std::isfinite(ch.x2) || !std::isfinite(ch.y2) || ch.widthScale < 0 || ch.x2 < ch.x1 || ch.y2 < ch.y1)
                    throw std::runtime_error("font.dat has invalid glyph code/metrics");
                section.chars.push_back(ch);
            }

            result.sections.push_back(section);
            offset = p;
        }
        return result;
    }

    void RebuildDirectFmap(FontDatSection &section)
    {
        uint32_t maxCode = 0;
        for (const FontDatChar &ch : section.chars)
        {
            if (ch.code > 0xFFFF) throw std::runtime_error("font.dat supports BMP codepoints only");
            maxCode = std::max(maxCode, ch.code);
        }

        section.fmap.assign(static_cast<size_t>(maxCode) + 1, -1);
        for (size_t i = 0; i < section.chars.size(); i++)
        {
            uint32_t code = section.chars[i].code;
            if (code < section.fmap.size())
                section.fmap[code] = static_cast<int32_t>(i);
        }
    }

    void WriteFontDat(const std::filesystem::path &path, const FontDatFile &fontDat)
    {
        std::vector<uint8_t> out;
        for (FontDatSection section : fontDat.sections)
        {
            RebuildDirectFmap(section);
            if (section.fmap.empty())
                section.fmap.push_back(-1);

            const char tag[] = "<FONT>";
            out.insert(out.end(), tag, tag + 6);

            // Manhunt 2 stores fmap count minus one in the header.
            WriteU32(out, static_cast<uint32_t>(section.fmap.size() - 1));
            WriteU32(out, 0);
            WriteU32(out, section.fontId);
            WriteU32(out, static_cast<uint32_t>(section.chars.size()));
            WriteU32(out, section.fontSize);
            WriteF32(out, section.heightScale);
            for (int32_t value : section.fmap)
                WriteS32(out, value);
            for (const FontDatChar &ch : section.chars)
            {
                WriteU32(out, ch.code);
                WriteF32(out, ch.widthScale);
                WriteF32(out, ch.x1);
                WriteF32(out, ch.y1);
                WriteF32(out, ch.x2);
                WriteF32(out, ch.y2);
            }
        }
        WriteAllBytes(path, out);
    }
}
