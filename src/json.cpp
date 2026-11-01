#include "json.hpp"
#include "unicode.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace mh2
{
    JsonError::JsonError(
        const std::string &message,
        std::string sourceName,
        size_t byteOffset,
        size_t line,
        size_t column)
        : std::runtime_error(message),
        sourceName_(std::move(sourceName)),
        byteOffset_(byteOffset),
        line_(line),
        column_(column)
    {
    }

    JsonValue JsonValue::Null()
    {
        return JsonValue();
    }

    JsonValue JsonValue::Bool(bool value)
    {
        JsonValue v;
        v.type = Type::Bool;
        v.boolValue = value;
        return v;
    }

    JsonValue JsonValue::Number(double value)
    {
        JsonValue v;
        v.type = Type::Number;
        v.numberValue = value;
        return v;
    }

    JsonValue JsonValue::String(std::string value)
    {
        JsonValue v;
        v.type = Type::String;
        v.stringValue = std::move(value);
        return v;
    }

    JsonValue JsonValue::Array(std::vector<JsonValue> value)
    {
        JsonValue v;
        v.type = Type::Array;
        v.arrayValue = std::move(value);
        return v;
    }

    JsonValue JsonValue::Object(std::map<std::string, JsonValue> value)
    {
        JsonValue v;
        v.type = Type::Object;
        v.objectValue = std::move(value);
        return v;
    }

    bool JsonValue::IsNull() const { return type == Type::Null; }
    bool JsonValue::IsBool() const { return type == Type::Bool; }
    bool JsonValue::IsNumber() const { return type == Type::Number; }
    bool JsonValue::IsString() const { return type == Type::String; }
    bool JsonValue::IsArray() const { return type == Type::Array; }
    bool JsonValue::IsObject() const { return type == Type::Object; }

    const JsonValue *JsonValue::Find(const std::string &key) const
    {
        if (!IsObject())
            return nullptr;

        std::map<std::string, JsonValue>::const_iterator it = objectValue.find(key);
        if (it == objectValue.end())
            return nullptr;
        return &it->second;
    }

    namespace
    {
        void AppendUtf8(std::string &out, unsigned int codepoint)
        {
            if (codepoint <= 0x7F)
            {
                out.push_back(static_cast<char>(codepoint));
            }
            else if (codepoint <= 0x7FF)
            {
                out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
            else if (codepoint <= 0xFFFF)
            {
                out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
                out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
            else if (codepoint <= 0x10FFFF)
            {
                out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
                out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
        }

        int HexValue(char ch)
        {
            if (ch >= '0' && ch <= '9')
                return ch - '0';
            if (ch >= 'a' && ch <= 'f')
                return 10 + ch - 'a';
            if (ch >= 'A' && ch <= 'F')
                return 10 + ch - 'A';
            return -1;
        }

        class Parser
        {
        public:
            Parser(const std::string &input, std::string inputName)
                : text(input), sourceName(std::move(inputName)), pos(0)
            {
                const bool bom = text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0;
                lineStarts.push_back(bom ? 3 : 0);
                for (size_t i = 0; i < text.size(); i++)
                    if (text[i] == '\n')
                        lineStarts.push_back(i + 1);
            }

            JsonValue Parse()
            {
                SkipBom();
                SkipWhitespace();
                JsonValue value = ParseValue();
                SkipWhitespace();
                if (pos != text.size())
                    Fail("trailing data after JSON value");
                return value;
            }

        private:
            const std::string &text;
            std::string sourceName;
            size_t pos;
            size_t depth = 0;
            std::vector<size_t> lineStarts;

            struct Location
            {
                size_t line = 1;
                size_t column = 1;
            };

            Location LocationAt(size_t offset) const
            {
                Location location;
                const size_t end = std::min(offset, text.size());
                auto line = std::upper_bound(lineStarts.begin(), lineStarts.end(), end);
                const size_t index = line == lineStarts.begin() ? 0 : static_cast<size_t>(line - lineStarts.begin() - 1);
                location.line = index + 1;
                for (size_t i = lineStarts[index]; i < end; i++)
                {
                    const unsigned char ch = static_cast<unsigned char>(text[i]);
                    if (ch == '\n')
                    {
                        location.line++;
                        location.column = 1;
                    }
                    else if ((ch & 0xC0) != 0x80)
                    {
                        location.column++;
                    }
                }
                return location;
            }

            std::string NearbyText(size_t offset) const
            {
                const size_t lineStartFound = text.rfind('\n', std::min(offset, text.size()));
                const size_t lineStart = lineStartFound == std::string::npos ? 0 : lineStartFound + 1;
                const size_t lineEndFound = text.find('\n', std::min(offset, text.size()));
                const size_t lineEnd = lineEndFound == std::string::npos ? text.size() : lineEndFound;

                size_t start = lineStart;
                size_t end = lineEnd;
                if (offset > start + 70)
                    start = offset - 70;
                if (end > offset + 70)
                    end = offset + 70;
                while (start < end && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80)
                    start++;
                while (end > start && end < text.size() &&
                    (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
                {
                    end--;
                }

                std::string nearby = text.substr(start, end - start);
                for (char &ch : nearby)
                {
                    if (ch == '\r' || ch == '\t')
                        ch = ' ';
                }
                if (start > lineStart)
                    nearby.insert(0, "...");
                if (end < lineEnd)
                    nearby += "...";
                return nearby;
            }

            void SkipBom()
            {
                if (text.size() >= 3 &&
                    static_cast<unsigned char>(text[0]) == 0xEF &&
                    static_cast<unsigned char>(text[1]) == 0xBB &&
                    static_cast<unsigned char>(text[2]) == 0xBF)
                {
                    pos = 3;
                }
            }

            void SkipWhitespace()
            {
                while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
                    pos++;
            }

            [[noreturn]] void FailAt(size_t offset, const std::string &message) const
            {
                const Location location = LocationAt(offset);
                std::ostringstream oss;
                if (!sourceName.empty())
                    oss << sourceName;
                else
                    oss << "<json>";
                oss << ':' << location.line << ':' << location.column
                    << ": JSON syntax error: " << message;
                const std::string nearby = NearbyText(offset);
                if (!nearby.empty())
                    oss << "\n  near: " << nearby;
                throw JsonError(
                    oss.str(),
                    sourceName,
                    offset,
                    location.line,
                    location.column);
            }

            [[noreturn]] void Fail(const std::string &message) const
            {
                FailAt(pos, message);
            }

            bool Match(char ch)
            {
                if (pos < text.size() && text[pos] == ch)
                {
                    pos++;
                    return true;
                }
                return false;
            }

            void Expect(char ch)
            {
                if (!Match(ch))
                {
                    std::string msg = "expected '";
                    msg.push_back(ch);
                    msg.push_back('\'');
                    Fail(msg);
                }
            }

            JsonValue ParseValue()
            {
                if (++depth > 128)
                    Fail("JSON nesting exceeds 128 levels");
                if (pos >= text.size())
                    Fail("expected value");

                const size_t start = pos;
                char ch = text[pos];
                JsonValue value;
                if (ch == '"')
                    value = JsonValue::String(ParseString());
                else if (ch == '[')
                    value = ParseArray();
                else if (ch == '{')
                    value = ParseObject();
                else if (ch == 't')
                    value = ParseLiteral("true", JsonValue::Bool(true));
                else if (ch == 'f')
                    value = ParseLiteral("false", JsonValue::Bool(false));
                else if (ch == 'n')
                    value = ParseLiteral("null", JsonValue::Null());
                else if (ch == '-' || (ch >= '0' && ch <= '9'))
                    value = ParseNumber();
                else
                {
                    std::string found(1, ch);
                    Fail("expected a JSON value, found '" + found + "'");
                }

                const Location location = LocationAt(start);
                value.sourceOffset = start;
                value.sourceLine = location.line;
                value.sourceColumn = location.column;
                --depth;
                return value;
            }

            JsonValue ParseLiteral(const char *literal, JsonValue value)
            {
                size_t start = pos;
                while (*literal)
                {
                    if (pos >= text.size() || text[pos] != *literal)
                    {
                        pos = start;
                        Fail("invalid literal");
                    }
                    pos++;
                    literal++;
                }
                return value;
            }

            JsonValue ParseNumber()
            {
                const size_t start = pos;
                if (Match('-') && pos >= text.size())
                    Fail("number is missing digits after '-'");
                if (Match('0'))
                {
                    if (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])))
                        Fail("leading zeros are not allowed in JSON numbers");
                }
                else
                {
                    if (pos >= text.size() || text[pos] < '1' || text[pos] > '9')
                        Fail("invalid number");
                    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])))
                        pos++;
                }
                if (Match('.'))
                {
                    if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos])))
                        Fail("fractional part requires at least one digit");
                    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])))
                        pos++;
                }
                if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E'))
                {
                    pos++;
                    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-'))
                        pos++;
                    if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos])))
                        Fail("exponent requires at least one digit");
                    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])))
                        pos++;
                }

                const std::string token = text.substr(start, pos - start);
                double value = 0;
                const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
                if (parsed.ec != std::errc() || parsed.ptr != token.data() + token.size() || !std::isfinite(value))
                    FailAt(start, "invalid or out-of-range number");
                return JsonValue::Number(value);
            }

            unsigned int ParseHex4()
            {
                if (pos + 4 > text.size())
                    Fail("incomplete unicode escape");

                unsigned int value = 0;
                for (int i = 0; i < 4; i++)
                {
                    int h = HexValue(text[pos++]);
                    if (h < 0)
                        Fail("invalid unicode escape");
                    value = (value << 4) | static_cast<unsigned int>(h);
                }
                return value;
            }

            std::string ParseString()
            {
                const size_t start = pos;
                Expect('"');
                std::string out;

                while (pos < text.size())
                {
                    unsigned char ch = static_cast<unsigned char>(text[pos++]);
                    if (ch == '"')
                    {
                        try { (void)Utf8ToUtf16(out); }
                        catch (const std::exception &ex) { FailAt(start, ex.what()); }
                        return out;
                    }

                    if (ch != '\\')
                    {
                        if (ch < 0x20)
                            FailAt(pos - 1, "unescaped control character inside string");
                        out.push_back(static_cast<char>(ch));
                        continue;
                    }

                    if (pos >= text.size())
                        Fail("incomplete escape");

                    char esc = text[pos++];
                    switch (esc)
                    {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u':
                    {
                        unsigned int code = ParseHex4();
                        if (code >= 0xD800 && code <= 0xDBFF)
                        {
                            size_t beforeLow = pos;
                            if (pos + 2 <= text.size() && text[pos] == '\\' && text[pos + 1] == 'u')
                            {
                                pos += 2;
                                unsigned int low = ParseHex4();
                                if (low >= 0xDC00 && low <= 0xDFFF)
                                {
                                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                                }
                                else
                                {
                                    Fail("invalid low surrogate");
                                }
                            }
                            else
                            {
                                pos = beforeLow;
                                Fail("missing low surrogate");
                            }
                        }
                        else if (code >= 0xDC00 && code <= 0xDFFF)
                            Fail("unpaired low surrogate");
                        AppendUtf8(out, code);
                        break;
                    }
                    default:
                        FailAt(pos - 1, std::string("invalid escape sequence \\") + esc + "'");
                    }
                }

                Fail("unterminated string");
                return out;
            }

            JsonValue ParseArray()
            {
                Expect('[');
                SkipWhitespace();

                std::vector<JsonValue> values;
                if (Match(']'))
                    return JsonValue::Array(std::move(values));

                for (;;)
                {
                    SkipWhitespace();
                    values.push_back(ParseValue());
                    SkipWhitespace();
                    if (Match(']'))
                        break;
                    if (!Match(','))
                        Fail("expected ',' or ']' after array item");
                    SkipWhitespace();
                    if (pos < text.size() && text[pos] == ']')
                        Fail("trailing comma is not allowed in a JSON array");
                }

                return JsonValue::Array(std::move(values));
            }

            JsonValue ParseObject()
            {
                Expect('{');
                SkipWhitespace();

                std::map<std::string, JsonValue> values;
                if (Match('}'))
                    return JsonValue::Object(std::move(values));

                for (;;)
                {
                    SkipWhitespace();
                    if (pos >= text.size() || text[pos] != '"')
                        Fail("expected object key");
                    const size_t keyOffset = pos;
                    std::string key = ParseString();
                    if (values.find(key) != values.end())
                        FailAt(keyOffset, "duplicate object key '" + key + "'");
                    SkipWhitespace();
                    Expect(':');
                    SkipWhitespace();
                    values.emplace(std::move(key), ParseValue());
                    SkipWhitespace();
                    if (Match('}'))
                        break;
                    if (!Match(','))
                        Fail("expected ',' or '}' after object member");
                    SkipWhitespace();
                    if (pos < text.size() && text[pos] == '}')
                        Fail("trailing comma is not allowed in a JSON object");
                }

                return JsonValue::Object(std::move(values));
            }
        };

        void AppendIndent(std::string &out, int indent)
        {
            for (int i = 0; i < indent; i++)
                out.push_back(' ');
        }

        void WriteJsonValue(const JsonValue &value, std::string &out, int indent)
        {
            switch (value.type)
            {
            case JsonValue::Type::Null:
                out += "null";
                break;
            case JsonValue::Type::Bool:
                out += value.boolValue ? "true" : "false";
                break;
            case JsonValue::Type::Number:
            {
                char buf[64];
                const auto formatted = std::to_chars(buf, buf + sizeof(buf), value.numberValue);
                if (formatted.ec != std::errc()) throw std::runtime_error("Cannot encode JSON number");
                out.append(buf, formatted.ptr);
                break;
            }
            case JsonValue::Type::String:
                out.push_back('"');
                out += JsonEscape(value.stringValue);
                out.push_back('"');
                break;
            case JsonValue::Type::Array:
                if (value.arrayValue.empty())
                {
                    out += "[]";
                    break;
                }
                out += "[\n";
                for (size_t i = 0; i < value.arrayValue.size(); i++)
                {
                    AppendIndent(out, indent + 2);
                    WriteJsonValue(value.arrayValue[i], out, indent + 2);
                    if (i + 1 != value.arrayValue.size())
                        out.push_back(',');
                    out.push_back('\n');
                }
                AppendIndent(out, indent);
                out.push_back(']');
                break;
            case JsonValue::Type::Object:
                if (value.objectValue.empty())
                {
                    out += "{}";
                    break;
                }
                out += "{\n";
                for (std::map<std::string, JsonValue>::const_iterator it = value.objectValue.begin(); it != value.objectValue.end();)
                {
                    AppendIndent(out, indent + 2);
                    out.push_back('"');
                    out += JsonEscape(it->first);
                    out += "\": ";
                    WriteJsonValue(it->second, out, indent + 2);
                    ++it;
                    if (it != value.objectValue.end())
                        out.push_back(',');
                    out.push_back('\n');
                }
                AppendIndent(out, indent);
                out.push_back('}');
                break;
            }
        }
    }

    JsonValue ParseJson(const std::string &text, const std::string &sourceName)
    {
        Parser parser(text, sourceName);
        return parser.Parse();
    }

    std::string JsonEscape(const std::string &text)
    {
        std::string out;
        for (size_t i = 0; i < text.size(); i++)
        {
            unsigned char ch = static_cast<unsigned char>(text[i]);
            switch (ch)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20)
                {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", ch);
                    out += buf;
                }
                else
                {
                    out.push_back(static_cast<char>(ch));
                }
                break;
            }
        }
        return out;
    }

    std::string WriteJson(const JsonValue &value, int indent)
    {
        std::string out;
        WriteJsonValue(value, out, indent);
        out.push_back('\n');
        return out;
    }
}
