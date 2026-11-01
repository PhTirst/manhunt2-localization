#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mh2
{
    struct TexTextureRecord
    {
        size_t recordOffset = 0;
        std::string name;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t bpp = 0;
        uint32_t pitch = 0;
        uint32_t flags = 0;
        uint32_t mipCount = 0;
        uint32_t dataOffset = 0;
        uint32_t dataSize = 0;
    };

    struct TexFile
    {
        bool wrappedZ2hm = false;
        std::vector<uint8_t> decompressed;
        std::vector<TexTextureRecord> records;
    };

    TexFile ReadTex(const std::filesystem::path &path);
    void WriteTexReplacingDds(
        const std::filesystem::path &sourcePath,
        const std::filesystem::path &outputPath,
        const std::map<std::string, std::vector<uint8_t>> &replacementDds);
    void WriteTexReplacingDdsByRecordOffset(
        const std::filesystem::path &sourcePath,
        const std::filesystem::path &outputPath,
        const std::map<size_t, std::vector<uint8_t>> &replacementDds);
    void WriteFontTexFromTemplate(
        const std::filesystem::path &templatePath,
        const std::filesystem::path &originalPath,
        const std::filesystem::path &outputPath,
        const std::map<std::string, std::vector<uint8_t>> &fontReplacementDds);
}
