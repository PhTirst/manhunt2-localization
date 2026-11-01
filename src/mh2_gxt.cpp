#include "mh2_gxt.hpp"
#include "file_io.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace mh2
{
    namespace
    {
        const uint32_t TKEY_MAGIC = 0x59454B54;
        const uint32_t TDAT_MAGIC = 0x54414454;
        const size_t MH2_TKEY_ENTRY_SIZE = 20;
        const size_t MH2_KEY_SIZE = 12;

        uint32_t ReadU32(const std::vector<uint8_t> &data, size_t offset)
        {
            if (offset + 4 > data.size())
                throw std::runtime_error("unexpected end of file while reading u32");

            return static_cast<uint32_t>(data[offset]) |
                (static_cast<uint32_t>(data[offset + 1]) << 8) |
                (static_cast<uint32_t>(data[offset + 2]) << 16) |
                (static_cast<uint32_t>(data[offset + 3]) << 24);
        }

        void WriteU32(std::vector<uint8_t> &out, uint32_t value)
        {
            out.push_back(static_cast<uint8_t>(value & 0xFF));
            out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        }

        std::vector<uint8_t> ReadAllBytes(const std::filesystem::path &path)
        {
            return ReadBytes(path);
        }

        void WriteAllBytes(const std::filesystem::path &path, const std::vector<uint8_t> &data)
        {
            WriteBytes(path, data);
        }

        std::string ReadFixedAscii(const std::vector<uint8_t> &data, size_t offset, size_t length)
        {
            if (offset + length > data.size())
                throw std::runtime_error("unexpected end of file while reading key");

            size_t end = offset;
            while (end < offset + length && data[end] != 0)
                end++;

            return std::string(reinterpret_cast<const char *>(&data[offset]), end - offset);
        }

        void ValidateKey(const std::string &key)
        {
            if (key.empty())
                throw std::runtime_error("GXT key cannot be empty");
            if (key.size() > MH2_KEY_SIZE)
                throw std::runtime_error("Manhunt 2 GXT key is longer than 12 bytes: " + key);
            for (size_t i = 0; i < key.size(); i++)
            {
                unsigned char ch = static_cast<unsigned char>(key[i]);
                if (ch < 0x20 || ch > 0x7E)
                    throw std::runtime_error("GXT key must be printable ASCII: " + key);
            }
        }

        std::u16string ReadUtf16LeNullTerminated(const std::vector<uint8_t> &data, size_t offset, size_t end)
        {
            if (offset > end)
                throw std::runtime_error("string offset is outside TDAT");
            if (((end - offset) % 2) != 0)
                throw std::runtime_error("TDAT string range is not aligned to UTF-16LE");

            std::u16string value;
            for (size_t pos = offset; pos + 1 < end; pos += 2)
            {
                uint16_t ch = static_cast<uint16_t>(data[pos]) |
                    static_cast<uint16_t>(static_cast<uint16_t>(data[pos + 1]) << 8);
                if (ch == 0)
                    return value;
                value.push_back(static_cast<char16_t>(ch));
            }

            throw std::runtime_error("unterminated UTF-16LE string in TDAT");
        }

        void WriteUtf16LeNullTerminated(std::vector<uint8_t> &out, const std::u16string &value)
        {
            for (size_t i = 0; i < value.size(); i++)
            {
                uint16_t ch = static_cast<uint16_t>(value[i]);
                out.push_back(static_cast<uint8_t>(ch & 0xFF));
                out.push_back(static_cast<uint8_t>((ch >> 8) & 0xFF));
            }
            out.push_back(0);
            out.push_back(0);
        }


        const JsonValue *FindAny(const JsonValue &object, const char *const *keys, size_t count)
        {
            for (size_t i = 0; i < count; i++)
            {
                const JsonValue *value = object.Find(keys[i]);
                if (value)
                    return value;
            }
            return nullptr;
        }

        std::string EntryContext(size_t index, const JsonValue &row, const std::string &key = {})
        {
            std::ostringstream oss;
            oss << "entry[" << index << ']';
            if (row.sourceLine != 0)
                oss << " at line " << row.sourceLine << ", column " << row.sourceColumn;
            if (!key.empty())
                oss << " (Key=\"" << key << "\")";
            return oss.str();
        }

        std::string AvailableStringFields(const JsonValue &row)
        {
            std::string fields;
            for (const auto &entry : row.objectValue)
            {
                if (!entry.second.IsString())
                    continue;
                if (!fields.empty())
                    fields += ", ";
                fields += entry.first;
            }
            return fields.empty() ? std::string("none") : fields;
        }
    }


    std::string FormatHex32(uint32_t value)
    {
        std::ostringstream oss;
        oss << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
        return oss.str();
    }

    uint32_t ParseExtraValue(const JsonValue &value)
    {
        if (value.IsNumber())
        {
            if (!std::isfinite(value.numberValue) || value.numberValue < 0 ||
                value.numberValue > 4294967295.0 || std::trunc(value.numberValue) != value.numberValue)
                throw std::runtime_error("Extra numeric value is outside uint32 range");
            return static_cast<uint32_t>(value.numberValue);
        }

        if (!value.IsString())
            throw std::runtime_error("Extra must be a number or hex string");

        std::string text = value.stringValue;
        int base = 10;
        size_t start = 0;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        {
            base = 16;
            start = 2;
        }
        if (text.empty())
            throw std::runtime_error("Extra cannot be an empty string");

        uint64_t result = 0;
        for (size_t i = start; i < text.size(); i++)
        {
            char ch = text[i];
            int digit = -1;
            if (ch >= '0' && ch <= '9')
                digit = ch - '0';
            else if (base == 16 && ch >= 'a' && ch <= 'f')
                digit = 10 + ch - 'a';
            else if (base == 16 && ch >= 'A' && ch <= 'F')
                digit = 10 + ch - 'A';
            else
                throw std::runtime_error("invalid Extra value: " + text);

            if (digit >= base)
                throw std::runtime_error("invalid Extra value: " + text);
            result = result * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
            if (result > 0xFFFFFFFFull)
                throw std::runtime_error("Extra value is outside uint32 range: " + text);
        }

        return static_cast<uint32_t>(result);
    }

    GxtFile ReadGxt(const std::filesystem::path &path)
    {
        std::vector<uint8_t> data = ReadAllBytes(path);
        if (data.size() < 8)
            throw std::runtime_error("file is too small to be a Manhunt 2 GXT: " + path.u8string());

        if (ReadU32(data, 0) != TKEY_MAGIC)
            throw std::runtime_error("missing TKEY magic: " + path.u8string());

        uint32_t tkeySize = ReadU32(data, 4);
        if ((tkeySize % MH2_TKEY_ENTRY_SIZE) != 0)
            throw std::runtime_error("TKEY size is not divisible by 20: " + path.u8string());

        size_t tkeyStart = 8;
        size_t tkeyEnd = tkeyStart + static_cast<size_t>(tkeySize);
        if (tkeyEnd > data.size())
            throw std::runtime_error("TKEY chunk extends beyond file: " + path.u8string());

        GxtFile gxt;
        size_t entryCount = static_cast<size_t>(tkeySize) / MH2_TKEY_ENTRY_SIZE;
        gxt.entries.reserve(entryCount);
        std::map<std::string, size_t> seenKeys;

        for (size_t i = 0; i < entryCount; i++)
        {
            size_t pos = tkeyStart + i * MH2_TKEY_ENTRY_SIZE;
            GxtEntry entry;
            entry.offset = ReadU32(data, pos);
            entry.key = ReadFixedAscii(data, pos + 4, MH2_KEY_SIZE);
            ValidateKey(entry.key);
            if (!seenKeys.emplace(entry.key, i).second)
                throw std::runtime_error("duplicate GXT key: " + entry.key);
            entry.extra = ReadU32(data, pos + 16);
            gxt.entries.push_back(entry);
        }

        if (entryCount == 0 && data.size() == 8)
            return gxt;

        if (tkeyEnd + 8 > data.size())
            throw std::runtime_error("missing TDAT chunk: " + path.u8string());
        if (ReadU32(data, tkeyEnd) != TDAT_MAGIC)
            throw std::runtime_error("missing TDAT magic: " + path.u8string());

        uint32_t tdatSize = ReadU32(data, tkeyEnd + 4);
        if ((tdatSize % 2) != 0)
            throw std::runtime_error("TDAT size must be even (UTF-16LE)");
        size_t tdatStart = tkeyEnd + 8;
        size_t tdatEnd = tdatStart + static_cast<size_t>(tdatSize);
        if (tdatEnd > data.size())
            throw std::runtime_error("TDAT chunk extends beyond file: " + path.u8string());

        gxt.hasTdat = true;
        for (size_t i = 0; i < gxt.entries.size(); i++)
        {
            if (gxt.entries[i].offset >= tdatSize || (gxt.entries[i].offset % 2) != 0)
                throw std::runtime_error("string offset is outside TDAT for key " + gxt.entries[i].key);
            gxt.entries[i].value = ReadUtf16LeNullTerminated(data, tdatStart + gxt.entries[i].offset, tdatEnd);
            (void)Utf16ToUtf8(gxt.entries[i].value);
        }

        return gxt;
    }

    void WriteGxt(const std::filesystem::path &path, const GxtFile &gxt)
    {
        if (gxt.entries.size() > std::numeric_limits<uint32_t>::max() / MH2_TKEY_ENTRY_SIZE)
            throw std::runtime_error("too many GXT entries");
        std::vector<uint8_t> out;
        WriteU32(out, TKEY_MAGIC);
        WriteU32(out, static_cast<uint32_t>(gxt.entries.size() * MH2_TKEY_ENTRY_SIZE));

        if (gxt.entries.empty())
        {
            WriteAllBytes(path, out);
            return;
        }

        std::vector<uint8_t> tdat;
        std::vector<uint32_t> offsets;
        offsets.reserve(gxt.entries.size());
        std::map<std::string, size_t> seenKeys;

        for (size_t i = 0; i < gxt.entries.size(); i++)
        {
            ValidateKey(gxt.entries[i].key);
            if (!seenKeys.emplace(gxt.entries[i].key, i).second)
                throw std::runtime_error("duplicate GXT key: " + gxt.entries[i].key);
            if (gxt.entries[i].value.find(u'\0') != std::u16string::npos)
                throw std::runtime_error("embedded NUL would truncate GXT text: " + gxt.entries[i].key);
            (void)Utf16ToUtf8(gxt.entries[i].value);
            const auto limit = static_cast<size_t>(std::numeric_limits<uint32_t>::max());
            if (gxt.entries[i].value.size() > (limit - 2) / 2 ||
                tdat.size() > limit - (gxt.entries[i].value.size() + 1) * 2)
                throw std::runtime_error("GXT text exceeds the 32-bit TDAT size limit");
            offsets.push_back(static_cast<uint32_t>(tdat.size()));
            WriteUtf16LeNullTerminated(tdat, gxt.entries[i].value);
        }

        for (size_t i = 0; i < gxt.entries.size(); i++)
        {
            WriteU32(out, offsets[i]);
            const std::string &key = gxt.entries[i].key;
            for (size_t j = 0; j < MH2_KEY_SIZE; j++)
            {
                if (j < key.size())
                    out.push_back(static_cast<uint8_t>(key[j]));
                else
                    out.push_back(0);
            }
            WriteU32(out, gxt.entries[i].extra);
        }

        WriteU32(out, TDAT_MAGIC);
        WriteU32(out, static_cast<uint32_t>(tdat.size()));
        out.insert(out.end(), tdat.begin(), tdat.end());
        WriteAllBytes(path, out);
    }

    JsonValue GxtToJson(const GxtFile &gxt, const std::string &language)
    {
        std::vector<JsonValue> rows;
        rows.reserve(gxt.entries.size());

        for (size_t i = 0; i < gxt.entries.size(); i++)
        {
            std::map<std::string, JsonValue> row;
            row["Key"] = JsonValue::String(gxt.entries[i].key);
            row["Extra"] = JsonValue::String(FormatHex32(gxt.entries[i].extra));
            row[language] = JsonValue::String(Utf16ToUtf8(gxt.entries[i].value));
            rows.push_back(JsonValue::Object(std::move(row)));
        }

        return JsonValue::Array(std::move(rows));
    }

    GxtFile JsonToGxt(const JsonValue &json, const std::string &language, bool allowLegacyTextFields)
    {
        const JsonValue *entries = &json;
        if (json.IsObject())
        {
            entries = json.Find("entries");
            if (!entries)
                entries = json.Find("Entries");
        }

        if (!entries || !entries->IsArray())
            throw std::runtime_error("JSON root must be an array or an object with entries array");

        GxtFile gxt;
        gxt.hasTdat = !entries->arrayValue.empty();
        gxt.entries.reserve(entries->arrayValue.size());
        std::map<std::string, size_t> firstKeyIndex;

        for (size_t i = 0; i < entries->arrayValue.size(); i++)
        {
            const JsonValue &row = entries->arrayValue[i];
            if (!row.IsObject())
                throw std::runtime_error(EntryContext(i, row) + " must be a JSON object");

            const char *keyNames[] = { "Key", "key", "KEY" };
            const JsonValue *keyValue = FindAny(row, keyNames, 3);
            if (!keyValue)
                throw std::runtime_error(EntryContext(i, row) + " is missing required string field 'Key'");
            if (!keyValue->IsString())
                throw std::runtime_error(EntryContext(i, row) + " field 'Key' must be a string");
            const std::string key = keyValue->stringValue;
            try
            {
                ValidateKey(key);
            }
            catch (const std::exception &ex)
            {
                throw std::runtime_error(EntryContext(i, row, key) + ": " + ex.what());
            }
            const auto duplicate = firstKeyIndex.find(key);
            if (duplicate != firstKeyIndex.end())
            {
                throw std::runtime_error(
                    EntryContext(i, row, key) + " duplicates entry[" +
                    std::to_string(duplicate->second) + "]");
            }
            firstKeyIndex.emplace(key, i);

            const JsonValue *extraValue = row.Find("Extra");
            if (!extraValue)
                extraValue = row.Find("extra");

            const JsonValue *textValue = row.Find(language);
            if (!textValue && allowLegacyTextFields)
                textValue = row.Find("Value");
            if (!textValue && allowLegacyTextFields)
                textValue = row.Find("Text");
            if (!textValue || !textValue->IsString())
            {
                throw std::runtime_error(
                    EntryContext(i, row, key) + " has no string field for language '" + language +
                    "' (available string fields: " + AvailableStringFields(row) + ")");
            }

            GxtEntry entry;
            entry.key = key;
            try
            {
                entry.extra = extraValue ? ParseExtraValue(*extraValue) : 0;
            }
            catch (const std::exception &ex)
            {
                throw std::runtime_error(EntryContext(i, row, key) + " has invalid Extra: " + ex.what());
            }
            try
            {
                entry.value = Utf8ToUtf16(textValue->stringValue);
                if (entry.value.find(u'\0') != std::u16string::npos)
                    throw std::runtime_error("embedded NUL would truncate GXT text");
            }
            catch (const std::exception &ex)
            {
                throw std::runtime_error(
                    EntryContext(i, row, key) + " invalid field '" + language + "': " + ex.what());
            }
            gxt.entries.push_back(entry);
        }

        return gxt;
    }
}
