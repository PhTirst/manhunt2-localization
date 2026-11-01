#include "unicode.hpp"

#include <stdexcept>

namespace mh2
{
    std::u16string Utf8ToUtf16(const std::string &text)
    {
        std::u16string out;
        for (size_t pos = 0; pos < text.size();)
        {
            const auto first = static_cast<unsigned char>(text[pos++]);
            unsigned int code = first;
            unsigned int minimum = 0;
            size_t continuation = 0;
            if (first >= 0xC2 && first <= 0xDF)
            {
                code = first & 0x1F;
                minimum = 0x80;
                continuation = 1;
            }
            else if (first >= 0xE0 && first <= 0xEF)
            {
                code = first & 0x0F;
                minimum = 0x800;
                continuation = 2;
            }
            else if (first >= 0xF0 && first <= 0xF4)
            {
                code = first & 0x07;
                minimum = 0x10000;
                continuation = 3;
            }
            else if (first > 0x7F)
                throw std::runtime_error("invalid UTF-8 leading byte");
            for (size_t i = 0; i < continuation; i++)
            {
                if (pos == text.size())
                    throw std::runtime_error("truncated UTF-8 sequence");
                const auto next = static_cast<unsigned char>(text[pos++]);
                if ((next & 0xC0) != 0x80)
                    throw std::runtime_error("invalid UTF-8 continuation byte");
                code = (code << 6) | (next & 0x3F);
            }
            if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
                throw std::runtime_error("invalid UTF-8 Unicode scalar");
            if (code <= 0xFFFF)
                out.push_back(static_cast<char16_t>(code));
            else
            {
                code -= 0x10000;
                out.push_back(static_cast<char16_t>(0xD800 + (code >> 10)));
                out.push_back(static_cast<char16_t>(0xDC00 + (code & 0x3FF)));
            }
        }
        return out;
    }

    std::string Utf16ToUtf8(const std::u16string &text)
    {
        std::string out;
        for (size_t pos = 0; pos < text.size(); pos++)
        {
            unsigned int code = text[pos];
            if (code >= 0xD800 && code <= 0xDBFF)
            {
                if (++pos == text.size() || text[pos] < 0xDC00 || text[pos] > 0xDFFF)
                    throw std::runtime_error("unpaired high surrogate in UTF-16");
                code = 0x10000 + ((code - 0xD800) << 10) + (text[pos] - 0xDC00);
            }
            else if (code >= 0xDC00 && code <= 0xDFFF)
                throw std::runtime_error("unpaired low surrogate in UTF-16");
            if (code <= 0x7F)
                out.push_back(static_cast<char>(code));
            else if (code <= 0x7FF)
            {
                out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            else if (code <= 0xFFFF)
            {
                out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (code >> 18)));
                out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
        }
        return out;
    }
}
