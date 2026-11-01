#include "mh2_dds.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace mh2
{
    namespace
    {
        const uint32_t DDS_MAGIC = 0x20534444;
        const uint32_t FOURCC_DXT5 = 0x35545844;

        uint32_t ReadU32(const std::vector<uint8_t> &data, size_t offset)
        {
            if (offset + 4 > data.size())
                throw std::runtime_error("unexpected end of DDS header");
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

        void WriteU32To(uint8_t *out, uint32_t value)
        {
            out[0] = static_cast<uint8_t>(value & 0xFF);
            out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        }

        size_t ChooseWorkerCount(size_t workItems, size_t itemsPerWorker, size_t workerLimit = 0)
        {
            if (workItems < itemsPerWorker)
                return 1;
            unsigned int hardware = std::thread::hardware_concurrency();
            if (hardware == 0)
                hardware = 4;
            size_t byWork = (workItems + itemsPerWorker - 1) / itemsPerWorker;
            size_t maxWorkers = workerLimit ? workerLimit : std::max<size_t>(1, hardware > 1 ? hardware - 1 : 1);
            return std::max<size_t>(1, std::min(byWork, maxWorkers));
        }

        uint16_t PackRgb565(uint8_t r, uint8_t g, uint8_t b)
        {
            return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }

        std::array<uint8_t, 3> UnpackRgb565(uint16_t color)
        {
            const uint8_t r5 = static_cast<uint8_t>((color >> 11) & 0x1F);
            const uint8_t g6 = static_cast<uint8_t>((color >> 5) & 0x3F);
            const uint8_t b5 = static_cast<uint8_t>(color & 0x1F);
            return {
                static_cast<uint8_t>((r5 << 3) | (r5 >> 2)),
                static_cast<uint8_t>((g6 << 2) | (g6 >> 4)),
                static_cast<uint8_t>((b5 << 3) | (b5 >> 2))};
        }

        std::array<std::array<uint8_t, 3>, 4> BuildDxtColorPalette(uint16_t c0, uint16_t c1)
        {
            std::array<std::array<uint8_t, 3>, 4> palette{};
            palette[0] = UnpackRgb565(c0);
            palette[1] = UnpackRgb565(c1);
            for (int channel = 0; channel < 3; channel++)
            {
                palette[2][channel] = static_cast<uint8_t>(
                    (2 * static_cast<int>(palette[0][channel]) + palette[1][channel]) / 3);
                palette[3][channel] = static_cast<uint8_t>(
                    (static_cast<int>(palette[0][channel]) + 2 * static_cast<int>(palette[1][channel])) / 3);
            }
            return palette;
        }

        std::array<uint8_t, 8> BuildDxt5AlphaPalette(uint8_t a0, uint8_t a1)
        {
            std::array<uint8_t, 8> palette{};
            palette[0] = a0;
            palette[1] = a1;
            if (a0 > a1)
            {
                palette[2] = static_cast<uint8_t>((6 * a0 + 1 * a1) / 7);
                palette[3] = static_cast<uint8_t>((5 * a0 + 2 * a1) / 7);
                palette[4] = static_cast<uint8_t>((4 * a0 + 3 * a1) / 7);
                palette[5] = static_cast<uint8_t>((3 * a0 + 4 * a1) / 7);
                palette[6] = static_cast<uint8_t>((2 * a0 + 5 * a1) / 7);
                palette[7] = static_cast<uint8_t>((1 * a0 + 6 * a1) / 7);
            }
            else
            {
                palette[2] = static_cast<uint8_t>((4 * a0 + 1 * a1) / 5);
                palette[3] = static_cast<uint8_t>((3 * a0 + 2 * a1) / 5);
                palette[4] = static_cast<uint8_t>((2 * a0 + 3 * a1) / 5);
                palette[5] = static_cast<uint8_t>((1 * a0 + 4 * a1) / 5);
                palette[6] = 0;
                palette[7] = 255;
            }
            return palette;
        }

        void CompressDxt5Block(const std::vector<uint8_t> &bgra, uint32_t width, uint32_t height, uint32_t bx, uint32_t by, uint8_t *out)
        {
            std::array<uint8_t, 16> alpha{};
            std::array<std::array<uint8_t, 3>, 16> rgb{};
            uint8_t minA = 255;
            uint8_t maxA = 0;
            bool visible = false;
            int darkestLuma = 256 * 3;
            int brightestLuma = -1;
            std::array<uint8_t, 3> darkest{0, 0, 0};
            std::array<uint8_t, 3> brightest{0, 0, 0};

            for (uint32_t y = 0; y < 4; y++)
            {
                for (uint32_t x = 0; x < 4; x++)
                {
                    uint32_t sx = std::min(width - 1, bx * 4 + x);
                    uint32_t sy = std::min(height - 1, by * 4 + y);
                    const size_t source = (static_cast<size_t>(sy) * width + sx) * 4;
                    const int pixel = static_cast<int>(y * 4 + x);
                    uint8_t a = bgra[source + 3];
                    alpha[pixel] = a;
                    rgb[pixel] = {bgra[source + 2], bgra[source + 1], bgra[source + 0]};
                    minA = std::min(minA, a);
                    maxA = std::max(maxA, a);
                    visible = visible || a != 0;
                    if (a != 0)
                    {
                        const int luma = static_cast<int>(rgb[pixel][0]) +
                            rgb[pixel][1] + rgb[pixel][2];
                        if (luma < darkestLuma)
                        {
                            darkestLuma = luma;
                            darkest = rgb[pixel];
                        }
                        if (luma > brightestLuma)
                        {
                            brightestLuma = luma;
                            brightest = rgb[pixel];
                        }
                    }
                }
            }

            uint8_t a0 = visible ? maxA : 0;
            uint8_t a1 = visible ? minA : 0;
            if (a0 <= a1 && visible)
            {
                if (a1 < 255)
                    a0 = static_cast<uint8_t>(a1 + 1);
                else
                    a1 = static_cast<uint8_t>(a0 - 1);
            }

            std::array<uint8_t, 8> palette = BuildDxt5AlphaPalette(a0, a1);

            uint64_t alphaBits = 0;
            for (int i = 0; i < 16; i++)
            {
                int best = 0;
                int bestDiff = 999;
                for (int p = 0; p < 8; p++)
                {
                    int diff = std::abs(static_cast<int>(alpha[i]) - static_cast<int>(palette[p]));
                    if (diff < bestDiff)
                    {
                        bestDiff = diff;
                        best = p;
                    }
                }
                alphaBits |= (static_cast<uint64_t>(best) << (3 * i));
            }

            size_t offset = 0;
            out[offset++] = a0;
            out[offset++] = a1;
            for (int i = 0; i < 6; i++)
                out[offset++] = static_cast<uint8_t>((alphaBits >> (8 * i)) & 0xFF);

            uint16_t c0 = visible ? PackRgb565(brightest[0], brightest[1], brightest[2]) : 0;
            uint16_t c1 = visible ? PackRgb565(darkest[0], darkest[1], darkest[2]) : 0;
            if (c0 < c1)
                std::swap(c0, c1);
            const auto colorPalette = BuildDxtColorPalette(c0, c1);
            uint32_t colorBits = 0;
            for (int i = 0; i < 16; i++)
            {
                int best = 0;
                int bestError = 3 * 255 * 255 + 1;
                for (int p = 0; p < 4; p++)
                {
                    int error = 0;
                    for (int channel = 0; channel < 3; channel++)
                    {
                        int difference = static_cast<int>(rgb[i][channel]) - colorPalette[p][channel];
                        error += difference * difference;
                    }
                    if (error < bestError)
                    {
                        bestError = error;
                        best = p;
                    }
                }
                colorBits |= static_cast<uint32_t>(best) << (2 * i);
            }
            out[offset++] = static_cast<uint8_t>(c0 & 0xFF);
            out[offset++] = static_cast<uint8_t>(c0 >> 8);
            out[offset++] = static_cast<uint8_t>(c1 & 0xFF);
            out[offset++] = static_cast<uint8_t>(c1 >> 8);
            WriteU32To(out + offset, colorBits);
        }

        std::vector<uint8_t> DownsampleBgra2x(const std::vector<uint8_t> &src, uint32_t width, uint32_t height, uint32_t &outWidth, uint32_t &outHeight)
        {
            outWidth = std::max<uint32_t>(1, width / 2);
            outHeight = std::max<uint32_t>(1, height / 2);
            std::vector<uint8_t> dst(static_cast<size_t>(outWidth) * outHeight * 4, 0);

            auto processRows = [&](uint32_t yBegin, uint32_t yEnd) {
                for (uint32_t y = yBegin; y < yEnd; y++)
                {
                    for (uint32_t x = 0; x < outWidth; x++)
                    {
                        uint64_t weightedColor[3] = {0, 0, 0};
                        uint32_t alphaSum = 0;
                        uint32_t samples = 0;
                        for (uint32_t oy = 0; oy < 2; oy++)
                        {
                            for (uint32_t ox = 0; ox < 2; ox++)
                            {
                                uint32_t sx = std::min(width - 1, x * 2 + ox);
                                uint32_t sy = std::min(height - 1, y * 2 + oy);
                                const uint8_t *p = &src[(static_cast<size_t>(sy) * width + sx) * 4];
                                const uint32_t alpha = p[3];
                                for (int c = 0; c < 3; c++)
                                    weightedColor[c] += static_cast<uint64_t>(p[c]) * alpha;
                                alphaSum += alpha;
                                samples++;
                            }
                        }
                        uint8_t *q = &dst[(static_cast<size_t>(y) * outWidth + x) * 4];
                        q[3] = static_cast<uint8_t>(alphaSum / samples);
                        if (alphaSum != 0)
                        {
                            for (int c = 0; c < 3; c++)
                                q[c] = static_cast<uint8_t>(weightedColor[c] / alphaSum);
                        }
                    }
                }
            };

            size_t workers = ChooseWorkerCount(outHeight, 32);
            if (workers == 1)
            {
                processRows(0, outHeight);
            }
            else
            {
                std::atomic<uint32_t> nextY{0};
                std::vector<std::thread> threads;
                threads.reserve(workers);
                for (size_t worker = 0; worker < workers; worker++)
                {
                    threads.emplace_back([&]() {
                        const uint32_t chunkRows = 16;
                        while (true)
                        {
                            uint32_t y = nextY.fetch_add(chunkRows, std::memory_order_relaxed);
                            if (y >= outHeight)
                                break;
                            processRows(y, std::min<uint32_t>(outHeight, y + chunkRows));
                        }
                    });
                }
                for (std::thread &thread : threads)
                    thread.join();
            }
            return dst;
        }

        void AppendDxt5Mip(const std::vector<uint8_t> &bgra, uint32_t width, uint32_t height, std::vector<uint8_t> &out, size_t workerLimit)
        {
            uint32_t blocksW = std::max<uint32_t>(1, (width + 3) / 4);
            uint32_t blocksH = std::max<uint32_t>(1, (height + 3) / 4);
            size_t blockCount = static_cast<size_t>(blocksW) * blocksH;
            std::vector<uint8_t> mip(blockCount * 16, 0);

            auto processBlocks = [&](size_t begin, size_t end) {
                for (size_t block = begin; block < end; block++)
                {
                    uint32_t by = static_cast<uint32_t>(block / blocksW);
                    uint32_t bx = static_cast<uint32_t>(block % blocksW);
                    CompressDxt5Block(bgra, width, height, bx, by, mip.data() + block * 16);
                }
            };

            size_t workers = ChooseWorkerCount(blockCount, 4096, workerLimit);
            if (workers == 1)
            {
                processBlocks(0, blockCount);
            }
            else
            {
                const size_t chunkBlocks = 1024;
                std::atomic<size_t> nextBlock{0};
                std::vector<std::thread> threads;
                threads.reserve(workers);
                for (size_t worker = 0; worker < workers; worker++)
                {
                    threads.emplace_back([&]() {
                        while (true)
                        {
                            size_t block = nextBlock.fetch_add(chunkBlocks, std::memory_order_relaxed);
                            if (block >= blockCount)
                                break;
                            processBlocks(block, std::min(blockCount, block + chunkBlocks));
                        }
                    });
                }
                for (std::thread &thread : threads)
                    thread.join();
            }

            out.insert(out.end(), mip.begin(), mip.end());
        }
    }

    DdsInfo ReadDdsInfo(const std::vector<uint8_t> &dds)
    {
        if (dds.size() < 128 || ReadU32(dds, 0) != DDS_MAGIC)
            throw std::runtime_error("invalid DDS payload");
        DdsInfo info;
        info.height = ReadU32(dds, 12);
        info.width = ReadU32(dds, 16);
        info.mipCount = ReadU32(dds, 28);
        info.fourCc = ReadU32(dds, 84);
        if (ReadU32(dds, 4) != 124 || ReadU32(dds, 76) != 32 || info.width == 0 || info.height == 0 || info.width > 8192 || info.height > 8192 || info.mipCount > 14)
            throw std::runtime_error("DDS header dimensions/structure are invalid or exceed the supported limit");
        return info;
    }

    std::vector<uint8_t> DecodeDxt5DdsFirstMip(const std::vector<uint8_t> &dds, uint32_t &width, uint32_t &height)
    {
        DdsInfo info = ReadDdsInfo(dds);
        if (info.fourCc != FOURCC_DXT5)
            throw std::runtime_error("only DXT5 DDS payloads can be decoded");
        if (info.width == 0 || info.height == 0)
            throw std::runtime_error("DDS dimensions cannot be zero");

        width = info.width;
        height = info.height;
        uint32_t blocksW = std::max<uint32_t>(1, (width + 3) / 4);
        uint32_t blocksH = std::max<uint32_t>(1, (height + 3) / 4);
        size_t dataOffset = 128;
        size_t expectedBytes = static_cast<size_t>(blocksW) * blocksH * 16;
        if (dds.size() < dataOffset + expectedBytes)
            throw std::runtime_error("DDS payload is shorter than first DXT5 mip");

        std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4, 0);
        for (uint32_t by = 0; by < blocksH; by++)
        {
            for (uint32_t bx = 0; bx < blocksW; bx++)
            {
                const uint8_t *block = dds.data() + dataOffset + (static_cast<size_t>(by) * blocksW + bx) * 16;
                std::array<uint8_t, 8> alphaPalette = BuildDxt5AlphaPalette(block[0], block[1]);
                uint64_t alphaBits = 0;
                for (int i = 0; i < 6; i++)
                    alphaBits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
                const uint16_t c0 = static_cast<uint16_t>(block[8] | (block[9] << 8));
                const uint16_t c1 = static_cast<uint16_t>(block[10] | (block[11] << 8));
                const auto colorPalette = BuildDxtColorPalette(c0, c1);
                const uint32_t colorBits = static_cast<uint32_t>(block[12]) |
                    (static_cast<uint32_t>(block[13]) << 8) |
                    (static_cast<uint32_t>(block[14]) << 16) |
                    (static_cast<uint32_t>(block[15]) << 24);

                for (uint32_t py = 0; py < 4; py++)
                {
                    for (uint32_t px = 0; px < 4; px++)
                    {
                        uint32_t x = bx * 4 + px;
                        uint32_t y = by * 4 + py;
                        if (x >= width || y >= height)
                            continue;
                        int pixelIndex = static_cast<int>(py * 4 + px);
                        uint8_t alpha = alphaPalette[(alphaBits >> (3 * pixelIndex)) & 0x7];
                        const auto &color = colorPalette[(colorBits >> (2 * pixelIndex)) & 0x3];
                        size_t dst = (static_cast<size_t>(y) * width + x) * 4;
                        bgra[dst + 0] = color[2];
                        bgra[dst + 1] = color[1];
                        bgra[dst + 2] = color[0];
                        bgra[dst + 3] = alpha;
                    }
                }
            }
        }
        return bgra;
    }

    std::vector<uint8_t> EncodeDxt5Dds(const std::vector<uint8_t> &bgra, uint32_t width, uint32_t height, uint32_t mipCount, size_t workerLimit)
    {
        if (width == 0 || height == 0 || width > 8192 || height > 8192 || mipCount > 14)
            throw std::runtime_error("DDS dimensions cannot be zero");
        if (bgra.size() != static_cast<size_t>(width) * height * 4)
            throw std::runtime_error("BGRA buffer size does not match DDS dimensions");
        uint32_t physicalMipCount = mipCount == 0 ? 1 : mipCount;

        std::vector<uint8_t> out;
        WriteU32(out, DDS_MAGIC);
        WriteU32(out, 124);
        WriteU32(out, mipCount == 0 ? 0x00081007 : 0x000A1007);
        WriteU32(out, height);
        WriteU32(out, width);
        WriteU32(out, width * height);
        WriteU32(out, 0);
        WriteU32(out, mipCount);

        for (int i = 0; i < 11; i++)
        {
            if (mipCount != 0 && i == 9)
                WriteU32(out, 0x5454564E); // NVTT
            else
                WriteU32(out, 0);
        }

        WriteU32(out, 32);
        WriteU32(out, 0x00000004);
        WriteU32(out, FOURCC_DXT5);
        WriteU32(out, 0);
        WriteU32(out, 0);
        WriteU32(out, 0);
        WriteU32(out, 0);
        WriteU32(out, 0);
        WriteU32(out, mipCount == 0 ? 0x00001000 : 0x00401008);
        WriteU32(out, 0);
        WriteU32(out, 0);
        WriteU32(out, 0);
        WriteU32(out, 0);

        std::vector<uint8_t> level = bgra;
        uint32_t levelWidth = width;
        uint32_t levelHeight = height;
        for (uint32_t mip = 0; mip < physicalMipCount; mip++)
        {
            AppendDxt5Mip(level, levelWidth, levelHeight, out, workerLimit);
            if (mip + 1 < physicalMipCount)
            {
                uint32_t nextWidth = 0;
                uint32_t nextHeight = 0;
                level = DownsampleBgra2x(level, levelWidth, levelHeight, nextWidth, nextHeight);
                levelWidth = nextWidth;
                levelHeight = nextHeight;
            }
        }
        return out;
    }
}
