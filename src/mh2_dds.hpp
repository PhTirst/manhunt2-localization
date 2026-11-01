#pragma once

#include <cstdint>
#include <vector>

namespace mh2
{
    struct DdsInfo
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipCount = 0;
        uint32_t fourCc = 0;
    };

    DdsInfo ReadDdsInfo(const std::vector<uint8_t> &dds);
    std::vector<uint8_t> DecodeDxt5DdsFirstMip(const std::vector<uint8_t> &dds, uint32_t &width, uint32_t &height);
    std::vector<uint8_t> EncodeDxt5Dds(const std::vector<uint8_t> &bgra, uint32_t width, uint32_t height, uint32_t mipCount, size_t workerLimit = 0);
}
