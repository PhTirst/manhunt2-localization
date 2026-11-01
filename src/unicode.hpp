#pragma once

#include <string>

namespace mh2
{
    std::string Utf16ToUtf8(const std::u16string &text);
    std::u16string Utf8ToUtf16(const std::string &text);
}
