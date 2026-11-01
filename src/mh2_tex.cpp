#include "mh2_tex.hpp"

#include "mh2_dds.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace mh2
{
    namespace
    {
        const uint32_t Z2HM_MAGIC = 0x4D48325A;
        const uint32_t TCDT_MAGIC = 0x54444354;
        const uint32_t DDS_MAGIC = 0x20534444;

        uint32_t ReadU32(const std::vector<uint8_t> &data, size_t offset)
        {
            if (offset + 4 > data.size())
                throw std::runtime_error("unexpected end of TEX while reading u32");
            return static_cast<uint32_t>(data[offset]) |
                (static_cast<uint32_t>(data[offset + 1]) << 8) |
                (static_cast<uint32_t>(data[offset + 2]) << 16) |
                (static_cast<uint32_t>(data[offset + 3]) << 24);
        }

        void AppendU32(std::vector<uint8_t> &out, uint32_t value)
        {
            out.push_back(static_cast<uint8_t>(value & 0xFF));
            out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        }

        std::vector<uint8_t> ReadAllBytes(const std::filesystem::path &path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                throw std::runtime_error("failed to open TEX: " + path.string());
            file.seekg(0, std::ios::end);
            std::streamoff size = file.tellg();
            if (size < 0)
                throw std::runtime_error("failed to determine TEX size: " + path.string());
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
                throw std::runtime_error("failed to write TEX: " + path.string());
            if (!data.empty())
                file.write(reinterpret_cast<const char *>(&data[0]), static_cast<std::streamsize>(data.size()));
        }

        bool LooksLikeName(const uint8_t *data, size_t length)
        {
            if (length == 0 || data[0] == 0)
                return false;
            for (size_t i = 0; i < length && data[i] != 0; i++)
            {
                uint8_t c = data[i];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
                    return false;
            }
            return true;
        }

        std::string ReadName(const std::vector<uint8_t> &data, size_t offset, size_t length)
        {
            size_t end = offset;
            while (end < offset + length && end < data.size() && data[end] != 0)
                end++;
            return std::string(reinterpret_cast<const char *>(&data[offset]), end - offset);
        }

        TexTextureRecord ReadTextureRecord(const std::vector<uint8_t> &data, size_t base)
        {
            if (base + 128 > data.size())
                throw std::runtime_error("TEX texture record extends beyond the payload");

            const uint32_t dataOffset = ReadU32(data, base + 104);
            const uint32_t dataSize = ReadU32(data, base + 112);
            if (dataOffset >= data.size() || dataSize == 0 || static_cast<size_t>(dataOffset) + dataSize > data.size())
                throw std::runtime_error("TEX texture DDS range is invalid at record offset " + std::to_string(base));
            if (ReadU32(data, dataOffset) != DDS_MAGIC)
                throw std::runtime_error("TEX texture does not point to a DDS at record offset " + std::to_string(base));

            TexTextureRecord record;
            record.recordOffset = base;
            record.name = ReadName(data, base + 16, 32);
            if (record.name.empty())
                throw std::runtime_error("TEX texture has an empty name at record offset " + std::to_string(base));
            record.width = ReadU32(data, base + 80);
            record.height = ReadU32(data, base + 84);
            record.bpp = ReadU32(data, base + 88);
            record.pitch = ReadU32(data, base + 92);
            record.flags = ReadU32(data, base + 96);
            record.mipCount = ReadU32(data, base + 100);
            record.dataOffset = dataOffset;
            record.dataSize = dataSize;
            return record;
        }

        std::vector<TexTextureRecord> FindTextureRecordsByLinkedList(const std::vector<uint8_t> &data)
        {
            // TCDT embeds a circular intrusive list. The header stores the item
            // count at 0x20 and a pointer to the first node's +8 link field at
            // 0x24. Following the list avoids mistaking DDS references elsewhere
            // in the payload for additional texture records.
            if (data.size() < 44)
                return {};
            const uint32_t count = ReadU32(data, 32);
            uint32_t linkPointer = ReadU32(data, 36);
            if (count == 0 || count > data.size() / 128 || linkPointer < 8)
                return {};

            std::vector<TexTextureRecord> records;
            records.reserve(count);
            std::set<size_t> visited;
            for (uint32_t index = 0; index < count; index++)
            {
                if (linkPointer < 8)
                    throw std::runtime_error("TEX texture list contains an invalid link");
                const size_t base = static_cast<size_t>(linkPointer) - 8;
                if (!visited.insert(base).second)
                    throw std::runtime_error("TEX texture list loops before reaching its declared count");
                records.push_back(ReadTextureRecord(data, base));
                linkPointer = ReadU32(data, base + 8);
            }
            return records;
        }

        std::vector<TexTextureRecord> FindTextureRecordsByScan(const std::vector<uint8_t> &data)
        {
            std::vector<TexTextureRecord> records;
            if (data.size() < 128)
                return records;

            for (size_t base = 0; base + 128 <= data.size(); base += 4)
            {
                if (!LooksLikeName(&data[base + 16], 32))
                    continue;

                uint32_t dataOffset = ReadU32(data, base + 104);
                uint32_t dataSize = ReadU32(data, base + 112);
                if (dataOffset >= data.size() || dataSize == 0 || static_cast<size_t>(dataOffset) + dataSize > data.size())
                    continue;
                if (ReadU32(data, dataOffset) != DDS_MAGIC)
                    continue;

                records.push_back(ReadTextureRecord(data, base));
            }

            return records;
        }

        std::vector<TexTextureRecord> FindTextureRecords(const std::vector<uint8_t> &data)
        {
            std::vector<TexTextureRecord> records = FindTextureRecordsByLinkedList(data);
            if (!records.empty())
                return records;
            return FindTextureRecordsByScan(data);
        }

        std::vector<uint8_t> InflateZ2hm(const std::vector<uint8_t> &data)
        {
            if (data.size() < 8)
                throw std::runtime_error("Z2HM file is too small");
            uint32_t uncompressedSize = ReadU32(data, 4);
            if (uncompressedSize == 0 || uncompressedSize > 512 * 1024 * 1024)
                throw std::runtime_error("Z2HM TEX declared size exceeds the supported 512 MiB limit");
            std::vector<uint8_t> out(uncompressedSize);
            uLongf outSize = static_cast<uLongf>(out.size());
            int status = uncompress(
                out.data(),
                &outSize,
                data.data() + 8,
                static_cast<uLong>(data.size() - 8));
            if (status != Z_OK)
                throw std::runtime_error("failed to zlib-decompress Z2HM TEX");
            if (outSize != uncompressedSize) throw std::runtime_error("Z2HM declared size does not match decompressed data");
            out.resize(outSize);
            return out;
        }

        std::vector<uint8_t> DeflateZ2hm(const std::vector<uint8_t> &data)
        {
            if (data.size() > static_cast<size_t>(std::numeric_limits<uInt>::max()))
                throw std::runtime_error("TEX is too large for the zlib stream API");

            z_stream stream{};
            int status = deflateInit(&stream, Z_BEST_SPEED);
            if (status != Z_OK)
                throw std::runtime_error("failed to initialize Z2HM zlib stream");

            stream.next_in = const_cast<Bytef *>(data.data());
            stream.avail_in = static_cast<uInt>(data.size());

            std::vector<uint8_t> compressed;
            compressed.reserve(compressBound(static_cast<uLong>(data.size())));
            std::array<uint8_t, 8192> buffer{};

            do
            {
                stream.next_out = buffer.data();
                stream.avail_out = static_cast<uInt>(buffer.size());
                const int flush = stream.avail_in == 0 ? Z_FINISH : Z_NO_FLUSH;
                status = deflate(&stream, flush);
                if (status != Z_OK && status != Z_STREAM_END)
                {
                    deflateEnd(&stream);
                    throw std::runtime_error("failed to zlib-compress Z2HM TEX");
                }

                const size_t written = buffer.size() - stream.avail_out;
                compressed.insert(compressed.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(written));
            } while (status != Z_STREAM_END);

            deflateEnd(&stream);

            std::vector<uint8_t> out;
            AppendU32(out, Z2HM_MAGIC);
            AppendU32(out, static_cast<uint32_t>(data.size()));
            out.insert(out.end(), compressed.begin(), compressed.end());
            return out;
        }

    }

    TexFile ReadTex(const std::filesystem::path &path)
    {
        std::vector<uint8_t> raw = ReadAllBytes(path);
        TexFile tex;
        if (raw.size() >= 8 && ReadU32(raw, 0) == Z2HM_MAGIC)
        {
            tex.wrappedZ2hm = true;
            tex.decompressed = InflateZ2hm(raw);
        }
        else
        {
            tex.wrappedZ2hm = false;
            tex.decompressed = std::move(raw);
        }

        if (tex.decompressed.size() < 4 || ReadU32(tex.decompressed, 0) != TCDT_MAGIC)
            throw std::runtime_error("TEX payload is not TCDT");
        tex.records = FindTextureRecords(tex.decompressed);
        return tex;
    }

    void WriteTexReplacingDds(
        const std::filesystem::path &sourcePath,
        const std::filesystem::path &outputPath,
        const std::map<std::string, std::vector<uint8_t>> &replacementDds)
    {
        TexFile tex = ReadTex(sourcePath);

        for (const auto &item : replacementDds)
        {
            auto it = std::find_if(tex.records.begin(), tex.records.end(), [&item](const TexTextureRecord &record) {
                return record.name == item.first;
            });
            if (it == tex.records.end())
                throw std::runtime_error("texture not found in TEX: " + item.first);
        }

        std::vector<uint8_t> data = tex.decompressed;
        for (const TexTextureRecord &record : tex.records)
        {
            auto replacement = replacementDds.find(record.name);
            if (replacement == replacementDds.end())
                continue;

            const std::vector<uint8_t> &payload = replacement->second;
            (void)ReadDdsInfo(payload);
            if (payload.size() > record.dataSize)
            {
                throw std::runtime_error(
                    "replacement DDS for " + record.name + " is " + std::to_string(payload.size()) +
                    " bytes, but the fixed TEX slot is only " + std::to_string(record.dataSize) +
                    " bytes; set project.json texTemplate to a compatible TEX with a larger preallocated font slot");
            }

            const auto begin = data.begin() + static_cast<std::ptrdiff_t>(record.dataOffset);
            std::copy(payload.begin(), payload.end(), begin);
            std::fill(
                begin + static_cast<std::ptrdiff_t>(payload.size()),
                begin + static_cast<std::ptrdiff_t>(record.dataSize),
                0);
        }

        if (tex.wrappedZ2hm)
            WriteAllBytes(outputPath, DeflateZ2hm(data));
        else
            WriteAllBytes(outputPath, data);
    }

    void WriteTexReplacingDdsByRecordOffset(
        const std::filesystem::path &sourcePath,
        const std::filesystem::path &outputPath,
        const std::map<size_t, std::vector<uint8_t>> &replacementDds)
    {
        TexFile tex = ReadTex(sourcePath);

        for (const auto &item : replacementDds)
        {
            auto it = std::find_if(tex.records.begin(), tex.records.end(), [&item](const TexTextureRecord &record) {
                return record.recordOffset == item.first;
            });
            if (it == tex.records.end())
                throw std::runtime_error("texture record not found in TEX at offset " + std::to_string(item.first));
        }

        std::vector<uint8_t> data = tex.decompressed;
        for (const TexTextureRecord &record : tex.records)
        {
            auto replacement = replacementDds.find(record.recordOffset);
            if (replacement == replacementDds.end())
                continue;

            const std::vector<uint8_t> &payload = replacement->second;
            (void)ReadDdsInfo(payload);
            if (payload.size() > record.dataSize)
            {
                throw std::runtime_error(
                    "replacement DDS for " + record.name + " at record offset " +
                    std::to_string(record.recordOffset) + " is " + std::to_string(payload.size()) +
                    " bytes, but the fixed TEX slot is only " + std::to_string(record.dataSize) + " bytes");
            }

            const auto begin = data.begin() + static_cast<std::ptrdiff_t>(record.dataOffset);
            std::copy(payload.begin(), payload.end(), begin);
            std::fill(
                begin + static_cast<std::ptrdiff_t>(payload.size()),
                begin + static_cast<std::ptrdiff_t>(record.dataSize),
                0);
        }

        if (tex.wrappedZ2hm)
            WriteAllBytes(outputPath, DeflateZ2hm(data));
        else
            WriteAllBytes(outputPath, data);
    }

    void WriteFontTexFromTemplate(
        const std::filesystem::path &templatePath,
        const std::filesystem::path &originalPath,
        const std::filesystem::path &outputPath,
        const std::map<std::string, std::vector<uint8_t>> &fontReplacementDds)
    {
        TexFile templateTex = ReadTex(templatePath);
        TexFile originalTex = ReadTex(originalPath);
        if (templateTex.records.size() != originalTex.records.size())
            throw std::runtime_error("TEX template must contain exactly the same texture records as the original");

        std::map<std::string, std::vector<const TexTextureRecord *>> originalByName;
        for (const TexTextureRecord &record : originalTex.records)
            originalByName[record.name].push_back(&record);

        std::map<std::string, size_t> occurrenceByName;
        std::vector<uint8_t> data = templateTex.decompressed;
        for (const TexTextureRecord &templateRecord : templateTex.records)
        {
            const size_t occurrence = occurrenceByName[templateRecord.name]++;
            auto originals = originalByName.find(templateRecord.name);
            if (originals == originalByName.end() || occurrence >= originals->second.size())
            {
                throw std::runtime_error(
                    "TEX template record has no matching original record: " +
                    templateRecord.name + " occurrence " + std::to_string(occurrence + 1));
            }

            const TexTextureRecord &originalRecord = *originals->second[occurrence];
            std::vector<uint8_t> payload;
            auto generated = fontReplacementDds.find(templateRecord.name);
            if (generated != fontReplacementDds.end())
            {
                payload = generated->second;
            }
            else
            {
                // The capacity template is allowed to differ only in physical DDS
                // slot placement and mip removal. Restore the original base image
                // for every non-font texture so UI edits from another patch never
                // leak into a font-only build.
                std::vector<uint8_t> originalPayload(
                    originalTex.decompressed.begin() +
                        static_cast<std::ptrdiff_t>(originalRecord.dataOffset),
                    originalTex.decompressed.begin() +
                        static_cast<std::ptrdiff_t>(originalRecord.dataOffset + originalRecord.dataSize));
                std::vector<uint8_t> templatePayload(
                    templateTex.decompressed.begin() +
                        static_cast<std::ptrdiff_t>(templateRecord.dataOffset),
                    templateTex.decompressed.begin() +
                        static_cast<std::ptrdiff_t>(templateRecord.dataOffset + templateRecord.dataSize));

                const DdsInfo originalInfo = ReadDdsInfo(originalPayload);
                const DdsInfo templateInfo = ReadDdsInfo(templatePayload);
                if (originalInfo.width != templateInfo.width ||
                    originalInfo.height != templateInfo.height ||
                    originalInfo.fourCc != templateInfo.fourCc)
                {
                    throw std::runtime_error(
                        "non-font texture layout differs between original and template: " +
                        templateRecord.name + " occurrence " + std::to_string(occurrence + 1));
                }

                if (originalPayload.size() <= templateRecord.dataSize)
                {
                    payload = std::move(originalPayload);
                }
                else
                {
                    if (templatePayload.size() < 128 || originalPayload.size() < templatePayload.size())
                    {
                        throw std::runtime_error(
                            "cannot restore original base image for " + templateRecord.name +
                            " occurrence " + std::to_string(occurrence + 1));
                    }
                    payload = std::move(templatePayload);
                    std::copy(
                        originalPayload.begin() + 128,
                        originalPayload.begin() + static_cast<std::ptrdiff_t>(payload.size()),
                        payload.begin() + 128);
                }
            }

            (void)ReadDdsInfo(payload);
            if (payload.size() > templateRecord.dataSize)
            {
                throw std::runtime_error(
                    "DDS for " + templateRecord.name + " occurrence " +
                    std::to_string(occurrence + 1) + " is " +
                    std::to_string(payload.size()) +
                    " bytes, but the template slot is only " +
                    std::to_string(templateRecord.dataSize) + " bytes");
            }

            const auto begin =
                data.begin() + static_cast<std::ptrdiff_t>(templateRecord.dataOffset);
            std::copy(payload.begin(), payload.end(), begin);
            std::fill(
                begin + static_cast<std::ptrdiff_t>(payload.size()),
                begin + static_cast<std::ptrdiff_t>(templateRecord.dataSize),
                0);
        }

        for (const auto &font : fontReplacementDds)
        {
            if (occurrenceByName.find(font.first) == occurrenceByName.end())
                throw std::runtime_error("font texture not found in TEX template: " + font.first);
        }
        for (const auto &entry : originalByName)
            if (occurrenceByName[entry.first] != entry.second.size())
                throw std::runtime_error("TEX template is missing original texture occurrences: " + entry.first);

        if (templateTex.wrappedZ2hm)
            WriteAllBytes(outputPath, DeflateZ2hm(data));
        else
            WriteAllBytes(outputPath, data);
    }
}
