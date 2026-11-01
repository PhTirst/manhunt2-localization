#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>

namespace mh2
{
    std::vector<uint8_t> ReadBytes(const std::filesystem::path &path);
    std::string ReadText(const std::filesystem::path &path);
    void WriteBytes(const std::filesystem::path &path, const std::vector<uint8_t> &bytes);
    void WriteText(const std::filesystem::path &path, const std::string &text);
}
