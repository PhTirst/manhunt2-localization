#include "mh2_font_build.hpp"

#include "json.hpp"
#include "mh2_dds.hpp"
#include "mh2_font_dat.hpp"
#include "mh2_gxt.hpp"
#include "mh2_tex.hpp"
#include "safe_files.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <atomic>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace mh2
{
    namespace
    {
        struct GlyphBitmap
        {
            uint32_t code = 0;
            int width = 0;
            int height = 0;
            int advance = 0;
            bool generatedFromTtf = false;
            std::vector<uint8_t> alpha;
            std::vector<uint8_t> intensity;
        };

        struct RenderCache
        {
            std::map<std::string, std::map<uint32_t, GlyphBitmap>> glyphs;
        };
        thread_local RenderCache *activeRenderCache = nullptr;

        struct PackedGlyph
        {
            GlyphBitmap glyph;
            int x = 0;
            int y = 0;
        };

        struct PackedAtlas
        {
            uint32_t width = 0;
            uint32_t height = 0;
            std::vector<uint8_t> bgra;
            std::vector<PackedGlyph> glyphs;
        };

        struct RoleTarget
        {
            size_t sectionIndex = 0;
            std::string textureName;
            int outputFontSize = 0;
            int renderFontSize = 0;
        };

        class StageTimer
        {
        public:
            explicit StageTimer(std::string label)
                : label_(std::move(label)),
                start_(std::chrono::steady_clock::now())
            {
            }

            ~StageTimer()
            {
                auto end = std::chrono::steady_clock::now();
                double seconds = std::chrono::duration<double>(end - start_).count();
                std::cout << "  " << label_ << " took " << seconds << "s\n";
            }

            StageTimer(const StageTimer &) = delete;
            StageTimer &operator=(const StageTimer &) = delete;

        private:
            std::string label_;
            std::chrono::steady_clock::time_point start_;
        };

        std::wstring Utf8PathToWide(const fs::path &path) { return path.wstring(); }
        std::string HashFileHex(const fs::path &path) { return FileHash(path); }

        class GdiplusSession
        {
        public:
            GdiplusSession()
            {
                Gdiplus::GdiplusStartupInput input;
                Gdiplus::Status status = Gdiplus::GdiplusStartup(&token_, &input, nullptr);
                if (status != Gdiplus::Ok)
                    throw std::runtime_error("failed to initialize GDI+");
            }

            ~GdiplusSession()
            {
                if (token_ != 0)
                    Gdiplus::GdiplusShutdown(token_);
            }

            GdiplusSession(const GdiplusSession &) = delete;
            GdiplusSession &operator=(const GdiplusSession &) = delete;

        private:
            ULONG_PTR token_ = 0;
        };

        std::unique_ptr<Gdiplus::FontFamily[]> LoadFontFamilies(
            Gdiplus::PrivateFontCollection &collection,
            const fs::path &ttfPath);

        class GlyphRenderer
        {
        public:
            GlyphRenderer(const fs::path &ttfPath, int fontSize)
                : fontSize_(fontSize)
            {
                std::unique_ptr<Gdiplus::FontFamily[]> families = LoadFontFamilies(collection_, ttfPath);
                WCHAR faceName[LF_FACESIZE]{};
                if (families[0].GetFamilyName(faceName) != Gdiplus::Ok || faceName[0] == 0)
                    throw std::runtime_error("failed to determine TTF family name: " + ttfPath.u8string());

                fontResourcePath_ = Utf8PathToWide(ttfPath);
                AddFontResourceExW(fontResourcePath_.c_str(), FR_PRIVATE, nullptr);

                hdc_ = CreateCompatibleDC(nullptr);
                if (!hdc_)
                    throw std::runtime_error("failed to create glyph render DC");

                font_ = CreateFontW(
                    fontSize_,
                    0,
                    0,
                    0,
                    FW_NORMAL,
                    FALSE,
                    FALSE,
                    FALSE,
                    DEFAULT_CHARSET,
                    OUT_TT_PRECIS,
                    CLIP_DEFAULT_PRECIS,
                    ANTIALIASED_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE,
                    faceName);
                if (!font_)
                    throw std::runtime_error("failed to create glyph render font");

                oldFont_ = SelectObject(hdc_, font_);
                if (!oldFont_)
                    throw std::runtime_error("failed to select glyph render font");

                TEXTMETRICW metrics{};
                if (!GetTextMetricsW(hdc_, &metrics))
                    throw std::runtime_error("failed to query glyph render text metrics");
                ascent_ = metrics.tmAscent;
                canvasH_ = std::max(1, static_cast<int>(metrics.tmHeight));
            }

            ~GlyphRenderer()
            {
                if (hdc_ && oldFont_)
                    SelectObject(hdc_, oldFont_);
                if (font_)
                    DeleteObject(font_);
                if (hdc_)
                    DeleteDC(hdc_);
                if (!fontResourcePath_.empty())
                    RemoveFontResourceExW(fontResourcePath_.c_str(), FR_PRIVATE, nullptr);
            }

            GlyphRenderer(const GlyphRenderer &) = delete;
            GlyphRenderer &operator=(const GlyphRenderer &) = delete;

            bool HasGlyph(uint32_t code) const
            {
                if (code == 0x20)
                    return true;
                if (code > 0xFFFF || (code >= 0xD800 && code <= 0xDFFF))
                    return false;

                WCHAR character = static_cast<WCHAR>(code);
                WORD glyphIndex = 0xFFFF;
                DWORD result = GetGlyphIndicesW(
                    hdc_,
                    &character,
                    1,
                    &glyphIndex,
                    GGI_MARK_NONEXISTING_GLYPHS);
                return result != GDI_ERROR && glyphIndex != 0xFFFF;
            }

            GlyphBitmap Render(uint32_t code)
            {
                GlyphBitmap glyph;
                glyph.code = code;
                glyph.height = canvasH_;

                if (code == 0x20)
                {
                    SIZE size{};
                    const WCHAR space = L' ';
                    if (!GetTextExtentPoint32W(hdc_, &space, 1, &size))
                        throw std::runtime_error("failed to measure space glyph");
                    glyph.width = std::max(1, static_cast<int>(size.cx));
                    glyph.advance = glyph.width;
                    glyph.alpha.assign(static_cast<size_t>(glyph.width) * glyph.height, 0);
                    return glyph;
                }

                if (code > 0xFFFF)
                {
                    glyph.width = std::max(4, fontSize_ / 3);
                    glyph.advance = glyph.width;
                    glyph.alpha.assign(static_cast<size_t>(glyph.width) * glyph.height, 0);
                    return glyph;
                }

                if (!HasGlyph(code))
                    throw std::runtime_error("TTF has no glyph for code " + std::to_string(code));

                MAT2 matrix{};
                matrix.eM11.value = 1;
                matrix.eM22.value = 1;

                GLYPHMETRICS metrics{};
                DWORD needed = GetGlyphOutlineW(
                    hdc_,
                    static_cast<WCHAR>(code),
                    GGO_GRAY8_BITMAP,
                    &metrics,
                    0,
                    nullptr,
                    &matrix);
                if (needed == GDI_ERROR || needed == 0 || metrics.gmBlackBoxX == 0 || metrics.gmBlackBoxY == 0)
                {
                    glyph.width = std::max(4, fontSize_ / 3);
                    glyph.advance = glyph.width;
                    glyph.alpha.assign(static_cast<size_t>(glyph.width) * glyph.height, 0);
                    return glyph;
                }

                const int originX = static_cast<int>(metrics.gmptGlyphOrigin.x);
                const int inkRight = originX + static_cast<int>(metrics.gmBlackBoxX);
                const int cellAdvance = std::max(1, static_cast<int>(metrics.gmCellIncX));
                const int cellLeft = std::min(0, originX);
                const int cellRight = std::max(cellAdvance, inkRight);
                glyph.width = std::max(1, cellRight - cellLeft);
                glyph.advance = cellAdvance;
                glyph.alpha.assign(static_cast<size_t>(glyph.width) * glyph.height, 0);

                std::vector<uint8_t> buffer(static_cast<size_t>(needed));
                DWORD got = GetGlyphOutlineW(
                    hdc_,
                    static_cast<WCHAR>(code),
                    GGO_GRAY8_BITMAP,
                    &metrics,
                    needed,
                    buffer.data(),
                    &matrix);
                if (got == GDI_ERROR)
                    throw std::runtime_error("failed to render glyph outline");

                uint32_t srcW = metrics.gmBlackBoxX;
                uint32_t srcH = metrics.gmBlackBoxY;
                uint32_t srcPitch = (srcW + 3) & ~3U;
                const int dstX = originX - cellLeft;
                int dstY = std::max(0, ascent_ - static_cast<int>(metrics.gmptGlyphOrigin.y));
                const int unclampedDstY = ascent_ - static_cast<int>(metrics.gmptGlyphOrigin.y);
                if (unclampedDstY < 0 || unclampedDstY + static_cast<int>(srcH) > glyph.height)
                {
                    char buffer[32]{};
                    std::snprintf(buffer, sizeof(buffer), "U+%04X", code);
                    throw std::runtime_error(
                        std::string("glyph exceeds measured font cell instead of being clipped: ") + buffer);
                }
                for (uint32_t y = 0; y < srcH; y++)
                {
                    int gy = dstY + static_cast<int>(y);
                    const uint8_t *src = buffer.data() + static_cast<size_t>(y) * srcPitch;
                    for (uint32_t x = 0; x < srcW; x++)
                    {
                        const int gx = dstX + static_cast<int>(x);
                        if (gx < 0 || gx >= glyph.width)
                            throw std::runtime_error("glyph horizontal bounds calculation failed");
                        uint8_t gray = src[x];
                        glyph.alpha[static_cast<size_t>(gy) * glyph.width + gx] = static_cast<uint8_t>(std::min<int>(255, gray * 4));
                    }
                }
                return glyph;
            }

        private:
            int fontSize_ = 0;
            int canvasH_ = 0;
            int ascent_ = 0;
            Gdiplus::PrivateFontCollection collection_;
            std::wstring fontResourcePath_;
            HDC hdc_ = nullptr;
            HFONT font_ = nullptr;
            HGDIOBJ oldFont_ = nullptr;
        };

        size_t ChooseRenderWorkerCount(size_t glyphCount, size_t workerLimit)
        {
            if (glyphCount < 128)
                return 1;

            unsigned int hardware = std::thread::hardware_concurrency();
            if (hardware == 0)
                hardware = 4;

            size_t byWork = (glyphCount + 63) / 64;
            size_t maxWorkers = std::min<size_t>(workerLimit, static_cast<size_t>(hardware));
            return std::max<size_t>(1, std::min(byWork, maxWorkers));
        }

        std::unique_ptr<Gdiplus::FontFamily[]> LoadFontFamilies(
            Gdiplus::PrivateFontCollection &collection,
            const fs::path &ttfPath)
        {
            std::wstring widePath = Utf8PathToWide(ttfPath);
            if (collection.AddFontFile(widePath.c_str()) != Gdiplus::Ok)
                throw std::runtime_error("failed to load TTF: " + ttfPath.u8string());

            INT familyCount = collection.GetFamilyCount();
            if (familyCount <= 0)
                throw std::runtime_error("TTF contains no font family: " + ttfPath.u8string());

            std::unique_ptr<Gdiplus::FontFamily[]> families(new Gdiplus::FontFamily[static_cast<size_t>(familyCount)]);
            INT found = 0;
            collection.GetFamilies(familyCount, families.get(), &found);
            if (found <= 0)
                throw std::runtime_error("failed to enumerate TTF families: " + ttfPath.u8string());
            return families;
        }

        std::string CodeFileName(uint32_t code)
        {
            char buffer[32]{};
            std::snprintf(buffer, sizeof(buffer), "U+%04X.glyph", code);
            return buffer;
        }

        fs::path GlyphCacheDir(const fs::path &cacheDir, const std::string &ttfHash, int fontSize)
        {
            return cacheDir / "glyphs-v2-natural-metrics" / ("ttf-" + ttfHash) / ("size-" + std::to_string(fontSize));
        }

        void WriteU32Binary(std::ofstream &file, uint32_t value)
        {
            file.write(reinterpret_cast<const char *>(&value), sizeof(value));
        }

        bool ReadU32Binary(std::ifstream &file, uint32_t &value)
        {
            file.read(reinterpret_cast<char *>(&value), sizeof(value));
            return static_cast<bool>(file);
        }

        bool TryLoadCachedGlyph(const fs::path &dir, uint32_t code, GlyphBitmap &glyph)
        {
            if (dir.empty()) return false;
            std::ifstream file(dir / CodeFileName(code), std::ios::binary);
            if (!file)
                return false;

            char magic[8]{};
            file.read(magic, sizeof(magic));
            if (!file || std::string(magic, sizeof(magic)) != std::string("MH2GLY1\0", 8))
                return false;

            uint32_t storedCode = 0;
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t advance = 0;
            if (!ReadU32Binary(file, storedCode) ||
                !ReadU32Binary(file, width) ||
                !ReadU32Binary(file, height) ||
                !ReadU32Binary(file, advance))
            {
                return false;
            }
            if (storedCode != code || width == 0 || height == 0 || width > 4096 || height > 4096)
                return false;

            std::vector<uint8_t> alpha(static_cast<size_t>(width) * height);
            if (!alpha.empty())
                file.read(reinterpret_cast<char *>(alpha.data()), static_cast<std::streamsize>(alpha.size()));
            if (!file)
                return false;

            glyph.code = storedCode;
            glyph.width = static_cast<int>(width);
            glyph.height = static_cast<int>(height);
            glyph.advance = static_cast<int>(advance);
            glyph.alpha = std::move(alpha);
            return true;
        }

        void SaveCachedGlyph(const fs::path &dir, const GlyphBitmap &glyph)
        {
            if (dir.empty()) return;
            fs::create_directories(dir);
            std::ofstream file(dir / CodeFileName(glyph.code), std::ios::binary | std::ios::trunc);
            if (!file)
                return;

            const char magic[8] = {'M', 'H', '2', 'G', 'L', 'Y', '1', '\0'};
            file.write(magic, sizeof(magic));
            WriteU32Binary(file, glyph.code);
            WriteU32Binary(file, static_cast<uint32_t>(std::max(0, glyph.width)));
            WriteU32Binary(file, static_cast<uint32_t>(std::max(0, glyph.height)));
            WriteU32Binary(file, static_cast<uint32_t>(std::max(0, glyph.advance)));
            if (!glyph.alpha.empty())
                file.write(reinterpret_cast<const char *>(glyph.alpha.data()), static_cast<std::streamsize>(glyph.alpha.size()));
        }

        std::vector<GlyphBitmap> RenderGlyphs(
            const fs::path &ttfPath,
            const std::set<uint32_t> &codes,
            int fontSize,
            size_t workerLimit,
            const fs::path &cacheDir,
            const std::string &ttfHash)
        {
            std::vector<uint32_t> codeList(codes.begin(), codes.end());
            std::vector<GlyphBitmap> glyphs(codeList.size());
            if (codeList.empty())
                return glyphs;

            fs::path fontCacheDir = cacheDir.empty() ? fs::path() : GlyphCacheDir(cacheDir, ttfHash, fontSize);
            const std::string memoryKey = ttfHash + ":" + std::to_string(fontSize);
            auto *memory = activeRenderCache ? &activeRenderCache->glyphs[memoryKey] : nullptr;
            std::vector<size_t> missingIndices;
            missingIndices.reserve(codeList.size());
            size_t loadedFromCache = 0;
            for (size_t i = 0; i < codeList.size(); i++)
            {
                if (memory && memory->count(codeList[i]))
                {
                    glyphs[i] = memory->at(codeList[i]);
                    loadedFromCache++;
                }
                else if (TryLoadCachedGlyph(fontCacheDir, codeList[i], glyphs[i]))
                {
                    glyphs[i].generatedFromTtf = true;
                    loadedFromCache++;
                }
                else
                    missingIndices.push_back(i);
            }

            if (missingIndices.empty())
            {
                if (memory) for (const auto &glyph : glyphs) (*memory)[glyph.code] = glyph;
                std::cout << "  loaded " << codeList.size() << " cached glyphs at " << fontSize << "px\n";
                std::sort(glyphs.begin(), glyphs.end(), [](const GlyphBitmap &a, const GlyphBitmap &b) {
                    return a.code < b.code;
                });
                return glyphs;
            }

            size_t workers = ChooseRenderWorkerCount(missingIndices.size(), workerLimit);
            std::cout << "  rendering " << missingIndices.size() << " glyphs at " << fontSize << "px with "
                << workers << " worker(s)";
            if (loadedFromCache != 0)
                std::cout << " (" << loadedFromCache << " loaded from glyph cache)";
            std::cout << "\n";

            if (workers == 1)
            {
                GlyphRenderer renderer(ttfPath, fontSize);
                for (size_t missingIndex : missingIndices)
                {
                    size_t i = missingIndex;
                    glyphs[i] = renderer.Render(codeList[i]);
                    glyphs[i].generatedFromTtf = true;
                    SaveCachedGlyph(fontCacheDir, glyphs[i]);
                }
            }
            else
            {
                const size_t chunkSize = 16;
                std::atomic<size_t> nextIndex{0};
                std::atomic<bool> stop{false};
                std::exception_ptr firstException;
                std::mutex exceptionMutex;
                std::vector<std::thread> threads;
                threads.reserve(workers);

                for (size_t worker = 0; worker < workers; worker++)
                {
                    threads.emplace_back([&, worker]() {
                        (void)worker;
                        try
                        {
                            GlyphRenderer renderer(ttfPath, fontSize);
                            while (!stop.load(std::memory_order_relaxed))
                            {
                                size_t start = nextIndex.fetch_add(chunkSize, std::memory_order_relaxed);
                                if (start >= missingIndices.size())
                                    break;
                                size_t end = std::min(missingIndices.size(), start + chunkSize);
                                for (size_t i = start; i < end; i++)
                                {
                                    size_t glyphIndex = missingIndices[i];
                                    glyphs[glyphIndex] = renderer.Render(codeList[glyphIndex]);
                                    glyphs[glyphIndex].generatedFromTtf = true;
                                    SaveCachedGlyph(fontCacheDir, glyphs[glyphIndex]);
                                }
                            }
                        }
                        catch (...)
                        {
                            stop.store(true, std::memory_order_relaxed);
                            std::lock_guard<std::mutex> lock(exceptionMutex);
                            if (!firstException)
                                firstException = std::current_exception();
                        }
                    });
                }

                for (std::thread &thread : threads)
                    thread.join();

                if (firstException)
                    std::rethrow_exception(firstException);
            }

            if (memory) for (const auto &glyph : glyphs) (*memory)[glyph.code] = glyph;
            std::sort(glyphs.begin(), glyphs.end(), [](const GlyphBitmap &a, const GlyphBitmap &b) {
                return a.code < b.code;
            });
            return glyphs;
        }

        PackedAtlas PackAtlas(const std::vector<GlyphBitmap> &glyphs, uint32_t width, uint32_t height)
        {
            PackedAtlas atlas;
            atlas.width = width;
            atlas.height = height;
            atlas.bgra.assign(static_cast<size_t>(width) * height * 4, 0);

            int x = 1;
            int y = 1;
            int rowH = 0;
            for (const GlyphBitmap &glyph : glyphs)
            {
                int gw = glyph.width;
                int gh = glyph.height;
                if (x + gw + 1 > static_cast<int>(width))
                {
                    x = 1;
                    y += rowH + 1;
                    rowH = 0;
                }
                if (y + gh + 1 > static_cast<int>(height))
                    throw std::runtime_error("glyph atlas overflow; choose a larger target texture or fewer characters");

                PackedGlyph packed;
                packed.glyph = glyph;
                packed.x = x;
                packed.y = y;
                atlas.glyphs.push_back(packed);

                for (int py = 0; py < gh; py++)
                {
                    for (int px = 0; px < gw; px++)
                    {
                        uint8_t a = glyph.alpha[static_cast<size_t>(py) * gw + px];
                        uint8_t intensity = glyph.intensity.empty()
                            ? 255
                            : glyph.intensity[static_cast<size_t>(py) * gw + px];
                        size_t dst = (static_cast<size_t>(y + py) * width + (x + px)) * 4;
                        atlas.bgra[dst + 0] = intensity;
                        atlas.bgra[dst + 1] = intensity;
                        atlas.bgra[dst + 2] = intensity;
                        atlas.bgra[dst + 3] = a;
                    }
                }

                x += gw + 1;
                rowH = std::max(rowH, gh);
            }
            return atlas;
        }

        size_t ApplyTtfAlphaGain(std::vector<GlyphBitmap> &glyphs, float gain)
        {
            if (gain == 1.0f)
                return 0;

            size_t changed = 0;
            for (GlyphBitmap &glyph : glyphs)
            {
                if (!glyph.generatedFromTtf)
                    continue;
                for (uint8_t &alpha : glyph.alpha)
                {
                    const int amplified = static_cast<int>(std::lround(static_cast<float>(alpha) * gain));
                    alpha = static_cast<uint8_t>(std::clamp(amplified, 0, 255));
                }
                changed++;
            }
            return changed;
        }

        void AddBlackOutline(GlyphBitmap &glyph, uint32_t outlineWidth)
        {
            if (outlineWidth == 0 || glyph.width <= 0 || glyph.height <= 0 || glyph.alpha.empty())
                return;
            if (std::none_of(glyph.alpha.begin(), glyph.alpha.end(), [](uint8_t alpha) { return alpha != 0; }))
                return;

            const int radius = static_cast<int>(outlineWidth);
            const int sourceWidth = glyph.width;
            const int sourceHeight = glyph.height;
            const int outlinedWidth = sourceWidth + radius * 2;
            const int outlinedHeight = sourceHeight + radius * 2;
            std::vector<uint8_t> outlinedAlpha(
                static_cast<size_t>(outlinedWidth) * outlinedHeight,
                0);
            std::vector<uint8_t> outlinedIntensity(
                static_cast<size_t>(outlinedWidth) * outlinedHeight,
                0);

            for (int y = 0; y < outlinedHeight; y++)
            {
                for (int x = 0; x < outlinedWidth; x++)
                {
                    const int sourceX = x - radius;
                    const int sourceY = y - radius;
                    uint8_t fillAlpha = 0;
                    if (sourceX >= 0 && sourceX < sourceWidth &&
                        sourceY >= 0 && sourceY < sourceHeight)
                    {
                        fillAlpha = glyph.alpha[
                            static_cast<size_t>(sourceY) * sourceWidth + sourceX];
                    }

                    uint8_t outlineAlpha = 0;
                    for (int offsetY = -radius; offsetY <= radius; offsetY++)
                    {
                        const int sampleY = sourceY + offsetY;
                        if (sampleY < 0 || sampleY >= sourceHeight)
                            continue;
                        for (int offsetX = -radius; offsetX <= radius; offsetX++)
                        {
                            const int sampleX = sourceX + offsetX;
                            if (sampleX < 0 || sampleX >= sourceWidth)
                                continue;
                            outlineAlpha = std::max(
                                outlineAlpha,
                                glyph.alpha[static_cast<size_t>(sampleY) * sourceWidth + sampleX]);
                        }
                    }

                    const uint8_t finalAlpha = std::max(fillAlpha, outlineAlpha);
                    const size_t destination = static_cast<size_t>(y) * outlinedWidth + x;
                    outlinedAlpha[destination] = finalAlpha;
                    if (finalAlpha != 0)
                    {
                        outlinedIntensity[destination] = static_cast<uint8_t>(
                            (static_cast<uint32_t>(fillAlpha) * 255U + finalAlpha / 2U) / finalAlpha);
                    }
                }
            }

            glyph.width = outlinedWidth;
            glyph.height = outlinedHeight;
            glyph.alpha = std::move(outlinedAlpha);
            glyph.intensity = std::move(outlinedIntensity);
        }

        PackedAtlas PackAtlasAutoSize(const std::vector<GlyphBitmap> &glyphs, uint32_t minWidth, uint32_t minHeight)
        {
            const uint32_t maxSize = 4096;
            uint64_t paddedArea = 0;
            uint32_t largestGlyphSide = 0;
            for (const GlyphBitmap &glyph : glyphs)
            {
                const uint32_t paddedWidth = static_cast<uint32_t>(std::max(1, glyph.width)) + 1;
                const uint32_t paddedHeight = static_cast<uint32_t>(std::max(1, glyph.height)) + 1;
                paddedArea += static_cast<uint64_t>(paddedWidth) * paddedHeight;
                largestGlyphSide = std::max(largestGlyphSide, std::max(paddedWidth, paddedHeight) + 1);
            }

            auto nextPowerOfTwo = [](uint32_t value) {
                uint32_t out = 1;
                while (out < value)
                    out <<= 1;
                return out;
            };

            uint32_t areaSide = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(paddedArea))));
            uint32_t side = nextPowerOfTwo(std::max({
                256U,
                minWidth,
                minHeight,
                largestGlyphSide,
                areaSide}));

            auto fits = [&glyphs](uint32_t candidate) {
                int x = 1;
                int y = 1;
                int rowH = 0;
                for (const GlyphBitmap &glyph : glyphs)
                {
                    const int gw = glyph.width;
                    const int gh = glyph.height;
                    if (x + gw + 1 > static_cast<int>(candidate))
                    {
                        x = 1;
                        y += rowH + 1;
                        rowH = 0;
                    }
                    if (y + gh + 1 > static_cast<int>(candidate))
                        return false;
                    x += gw + 1;
                    rowH = std::max(rowH, gh);
                }
                return true;
            };

            while (side <= maxSize && !fits(side))
                side *= 2;

            if (side <= maxSize)
            {
                return PackAtlas(glyphs, side, side);
            }

            throw std::runtime_error("glyph atlas overflow even at 4096x4096");
        }

        FontDatSection BuildDatSectionFromAtlas(
            const FontDatSection &source,
            const PackedAtlas &atlas,
            uint32_t outputFontSize)
        {
            FontDatSection section;
            section.fontId = source.fontId;
            section.fontSize = outputFontSize;
            section.heightScale = static_cast<float>(outputFontSize) / 512.0f;
            section.chars.reserve(atlas.glyphs.size());
            for (const PackedGlyph &packed : atlas.glyphs)
            {
                FontDatChar ch;
                ch.code = packed.glyph.code;
                ch.widthScale = static_cast<float>(std::max(1, packed.glyph.advance)) / 512.0f;
                ch.x1 = static_cast<float>(packed.x) / static_cast<float>(atlas.width);
                ch.y1 = static_cast<float>(packed.y) / static_cast<float>(atlas.height);
                ch.x2 = static_cast<float>(packed.x + packed.glyph.width) / static_cast<float>(atlas.width);
                ch.y2 = static_cast<float>(packed.y + packed.glyph.height) / static_cast<float>(atlas.height);
                section.chars.push_back(ch);
            }
            RebuildDirectFmap(section);
            return section;
        }

        int ClampInt(int value, int low, int high)
        {
            return std::max(low, std::min(value, high));
        }

        std::map<uint32_t, GlyphBitmap> ExtractOriginalGlyphs(
            const FontDatSection &section,
            const std::vector<uint8_t> &decodedBgra,
            uint32_t texWidth,
            uint32_t texHeight)
        {
            if (texWidth == 0 || texHeight == 0)
                throw std::runtime_error("cannot extract glyphs from zero-sized texture");
            if (decodedBgra.size() != static_cast<size_t>(texWidth) * texHeight * 4)
                throw std::runtime_error("decoded texture size does not match DDS dimensions");

            std::map<uint32_t, GlyphBitmap> glyphs;
            for (const FontDatChar &ch : section.chars)
            {
                int x1 = ClampInt(static_cast<int>(std::lround(ch.x1 * static_cast<float>(texWidth))), 0, static_cast<int>(texWidth));
                int y1 = ClampInt(static_cast<int>(std::lround(ch.y1 * static_cast<float>(texHeight))), 0, static_cast<int>(texHeight));
                int x2 = ClampInt(static_cast<int>(std::lround(ch.x2 * static_cast<float>(texWidth))), 0, static_cast<int>(texWidth));
                int y2 = ClampInt(static_cast<int>(std::lround(ch.y2 * static_cast<float>(texHeight))), 0, static_cast<int>(texHeight));
                int advance = std::max(1, static_cast<int>(std::lround(ch.widthScale * 512.0f)));

                GlyphBitmap glyph;
                glyph.code = ch.code;
                glyph.width = std::max(1, x2 - x1);
                glyph.height = std::max(1, y2 - y1);
                glyph.advance = advance;
                glyph.alpha.assign(static_cast<size_t>(glyph.width) * glyph.height, 0);

                if (x2 > x1 && y2 > y1)
                {
                    for (int py = 0; py < glyph.height; py++)
                    {
                        int sy = y1 + py;
                        if (sy < 0 || sy >= static_cast<int>(texHeight))
                            continue;
                        for (int px = 0; px < glyph.width; px++)
                        {
                            int sx = x1 + px;
                            if (sx < 0 || sx >= static_cast<int>(texWidth))
                                continue;
                            size_t src = (static_cast<size_t>(sy) * texWidth + static_cast<uint32_t>(sx)) * 4;
                            glyph.alpha[static_cast<size_t>(py) * glyph.width + px] = decodedBgra[src + 3];
                        }
                    }
                }

                glyphs[glyph.code] = std::move(glyph);
            }
            return glyphs;
        }

        int OriginalCellHeight(
            const std::map<uint32_t, GlyphBitmap> &originalGlyphs,
            int renderFontSize)
        {
            std::map<int, size_t> heightCounts;
            for (const auto &entry : originalGlyphs)
                heightCounts[entry.second.height]++;

            int targetCellHeight = renderFontSize;
            size_t mostCommonHeightCount = 0;
            for (const auto &entry : heightCounts)
            {
                if (entry.second > mostCommonHeightCount)
                {
                    mostCommonHeightCount = entry.second;
                    targetCellHeight = std::max(renderFontSize, entry.first);
                }
            }
            return targetCellHeight;
        }

        int OriginalTopPadding(const std::map<uint32_t, GlyphBitmap> &originalGlyphs)
        {
            std::vector<int> samples;
            auto addSample = [&](uint32_t code) {
                auto entry = originalGlyphs.find(code);
                if (entry == originalGlyphs.end())
                    return;
                const GlyphBitmap &glyph = entry->second;
                for (int y = 0; y < glyph.height; y++)
                {
                    const auto row = glyph.alpha.begin() + static_cast<std::ptrdiff_t>(y) * glyph.width;
                    if (std::any_of(row, row + glyph.width, [](uint8_t alpha) { return alpha != 0; }))
                    {
                        samples.push_back(y);
                        return;
                    }
                }
            };

            for (uint32_t code = 'A'; code <= 'Z'; code++)
                addSample(code);
            for (uint32_t code = '0'; code <= '9'; code++)
                addSample(code);

            if (samples.empty())
                return 1;
            std::sort(samples.begin(), samples.end());
            return std::max(1, samples[samples.size() / 2]);
        }

        void AlignTtfGlyphToOriginalCell(
            GlyphBitmap &glyph,
            int targetCellHeight,
            int targetTopPadding)
        {
            // Manhunt 2 clips font quads against geometry derived from the original
            // font cell. Every replacement glyph, including Latin glyphs, must keep
            // that transparent area above the baseline even when all glyphs use TTF.
            int minInkY = glyph.height;
            int maxInkY = -1;
            for (int y = 0; y < glyph.height; y++)
            {
                for (int x = 0; x < glyph.width; x++)
                {
                    if (glyph.alpha[static_cast<size_t>(y) * glyph.width + x] != 0)
                    {
                        minInkY = std::min(minInkY, y);
                        maxInkY = y;
                    }
                }
            }

            const int baseHeight = std::max(targetCellHeight, glyph.height);
            const int heightAlignmentShift = std::max(1, baseHeight - glyph.height);
            const int topSafetyShift = maxInkY < 0
                ? 0
                : std::max(0, targetTopPadding - minInkY);
            const int shiftY = std::max(heightAlignmentShift, topSafetyShift);
            // Keep one transparent row below the ink. Expanding the sampled cell is
            // preferable to clipping descenders or shrinking the TTF before packing.
            const int alignedHeight = maxInkY < 0
                ? baseHeight
                : std::max(baseHeight, maxInkY + shiftY + 2);
            std::vector<uint8_t> aligned(
                static_cast<size_t>(glyph.width) * alignedHeight,
                0);

            for (int y = 0; y < glyph.height && y + shiftY < alignedHeight; y++)
            {
                std::copy_n(
                    glyph.alpha.begin() + static_cast<std::ptrdiff_t>(y) * glyph.width,
                    glyph.width,
                    aligned.begin() + static_cast<std::ptrdiff_t>(y + shiftY) * glyph.width);
            }
            glyph.height = alignedHeight;
            glyph.alpha = std::move(aligned);
        }

        std::vector<GlyphBitmap> BuildOriginalFirstGlyphs(
            const FontDatSection &sourceSection,
            const std::vector<uint8_t> &decodedOriginalBgra,
            uint32_t originalWidth,
            uint32_t originalHeight,
            const std::set<uint32_t> &requiredCodes,
            const fs::path &ttfPath,
            const fs::path &fallbackTtfPath,
            const RoleTarget &role,
            size_t workerLimit,
            const fs::path &cacheDir,
            const std::string &ttfHash,
            const std::string &fallbackTtfHash)
        {
            std::map<uint32_t, GlyphBitmap> originalGlyphs =
                ExtractOriginalGlyphs(sourceSection, decodedOriginalBgra, originalWidth, originalHeight);

            const int targetCellHeight = OriginalCellHeight(originalGlyphs, role.renderFontSize);
            const int targetTopPadding = OriginalTopPadding(originalGlyphs);

            std::set<uint32_t> missingCodes;
            for (uint32_t code : requiredCodes)
            {
                if (originalGlyphs.find(code) == originalGlyphs.end())
                    missingCodes.insert(code);
            }

            std::map<uint32_t, GlyphBitmap> merged;
            for (uint32_t code : requiredCodes)
            {
                auto original = originalGlyphs.find(code);
                if (original != originalGlyphs.end())
                    merged.emplace(code, original->second);
            }

            if (!missingCodes.empty())
            {
                std::set<uint32_t> primaryCodes;
                std::set<uint32_t> fallbackCodes;
                std::set<uint32_t> unsupportedCodes;
                GlyphRenderer primaryProbe(ttfPath, role.renderFontSize);
                std::unique_ptr<GlyphRenderer> fallbackProbe;
                if (!fallbackTtfPath.empty())
                    fallbackProbe = std::make_unique<GlyphRenderer>(fallbackTtfPath, role.renderFontSize);

                for (uint32_t code : missingCodes)
                {
                    if (primaryProbe.HasGlyph(code))
                        primaryCodes.insert(code);
                    else if (fallbackProbe && fallbackProbe->HasGlyph(code))
                        fallbackCodes.insert(code);
                    else
                        unsupportedCodes.insert(code);
                }

                if (!unsupportedCodes.empty())
                {
                    std::string message = "no source font contains glyphs for";
                    size_t shown = 0;
                    for (uint32_t code : unsupportedCodes)
                    {
                        char buffer[20]{};
                        std::snprintf(buffer, sizeof(buffer), " U+%04X", code);
                        message += buffer;
                        if (++shown == 20 && unsupportedCodes.size() > shown)
                        {
                            message += " ...";
                            break;
                        }
                    }
                    throw std::runtime_error(message);
                }

                if (!fallbackCodes.empty())
                    std::cout << "  using fallback TTF for " << fallbackCodes.size() << " glyph(s)\n";

                auto renderAndMerge = [&](const fs::path &fontPath, const std::set<uint32_t> &fontCodes, const std::string &fontHash) {
                    if (fontCodes.empty())
                        return;
                    std::vector<GlyphBitmap> rendered = RenderGlyphs(
                        fontPath,
                        fontCodes,
                        role.renderFontSize,
                        workerLimit,
                        cacheDir,
                        fontHash);
                    for (GlyphBitmap &glyph : rendered)
                    {
                        AlignTtfGlyphToOriginalCell(glyph, targetCellHeight, targetTopPadding);
                        merged[glyph.code] = std::move(glyph);
                    }
                };

                renderAndMerge(ttfPath, primaryCodes, ttfHash);
                renderAndMerge(fallbackTtfPath, fallbackCodes, fallbackTtfHash);
            }

            std::vector<GlyphBitmap> glyphs;
            glyphs.reserve(merged.size());
            for (auto &entry : merged)
                glyphs.push_back(std::move(entry.second));
            return glyphs;
        }

        std::vector<GlyphBitmap> BuildTtfFirstGlyphs(
            const FontDatSection &sourceSection,
            const std::vector<uint8_t> &decodedOriginalBgra,
            uint32_t originalWidth,
            uint32_t originalHeight,
            const std::set<uint32_t> &requiredCodes,
            const fs::path &ttfPath,
            const fs::path &fallbackTtfPath,
            const RoleTarget &role,
            size_t workerLimit,
            const fs::path &cacheDir,
            const std::string &ttfHash,
            const std::string &fallbackTtfHash)
        {
            std::map<uint32_t, GlyphBitmap> originalGlyphs =
                ExtractOriginalGlyphs(sourceSection, decodedOriginalBgra, originalWidth, originalHeight);
            const int targetCellHeight = OriginalCellHeight(originalGlyphs, role.renderFontSize);
            const int targetTopPadding = OriginalTopPadding(originalGlyphs);

            std::set<uint32_t> primaryCodes;
            std::set<uint32_t> fallbackCodes;
            std::set<uint32_t> originalFallbackCodes;
            std::set<uint32_t> unsupportedCodes;
            GlyphRenderer primaryProbe(ttfPath, role.renderFontSize);
            std::unique_ptr<GlyphRenderer> fallbackProbe;
            if (!fallbackTtfPath.empty())
                fallbackProbe = std::make_unique<GlyphRenderer>(fallbackTtfPath, role.renderFontSize);

            for (uint32_t code : requiredCodes)
            {
                if (primaryProbe.HasGlyph(code))
                    primaryCodes.insert(code);
                else if (fallbackProbe && fallbackProbe->HasGlyph(code))
                    fallbackCodes.insert(code);
                else if (originalGlyphs.find(code) != originalGlyphs.end())
                    originalFallbackCodes.insert(code);
                else
                    unsupportedCodes.insert(code);
            }

            if (!unsupportedCodes.empty())
            {
                std::string message = "no TTF or original font contains glyphs for";
                size_t shown = 0;
                for (uint32_t code : unsupportedCodes)
                {
                    char buffer[20]{};
                    std::snprintf(buffer, sizeof(buffer), " U+%04X", code);
                    message += buffer;
                    if (++shown == 20 && unsupportedCodes.size() > shown)
                    {
                        message += " ...";
                        break;
                    }
                }
                throw std::runtime_error(message);
            }

            if (!fallbackCodes.empty())
                std::cout << "  using fallback TTF for " << fallbackCodes.size() << " glyph(s)\n";
            if (!originalFallbackCodes.empty())
                std::cout << "  using original font for " << originalFallbackCodes.size()
                    << " glyph(s) missing from all TTFs\n";

            std::map<uint32_t, GlyphBitmap> merged;
            for (uint32_t code : originalFallbackCodes)
                merged.emplace(code, originalGlyphs.at(code));

            auto renderAndMerge = [&](const fs::path &fontPath, const std::set<uint32_t> &fontCodes, const std::string &fontHash) {
                if (fontCodes.empty())
                    return;
                std::vector<GlyphBitmap> rendered = RenderGlyphs(
                    fontPath,
                    fontCodes,
                    role.renderFontSize,
                    workerLimit,
                    cacheDir,
                    fontHash);
                for (GlyphBitmap &glyph : rendered)
                {
                    AlignTtfGlyphToOriginalCell(glyph, targetCellHeight, targetTopPadding);
                    merged[glyph.code] = std::move(glyph);
                }
            };

            renderAndMerge(ttfPath, primaryCodes, ttfHash);
            renderAndMerge(fallbackTtfPath, fallbackCodes, fallbackTtfHash);

            std::vector<GlyphBitmap> glyphs;
            glyphs.reserve(merged.size());
            for (auto &entry : merged)
                glyphs.push_back(std::move(entry.second));
            return glyphs;
        }

        void ApplyAsciiTtfOverride(
            std::vector<GlyphBitmap> &glyphs,
            const FontDatSection &sourceSection,
            const std::vector<uint8_t> &decodedOriginalBgra,
            uint32_t originalWidth,
            uint32_t originalHeight,
            const std::set<uint32_t> &requiredCodes,
            const fs::path &asciiTtfPath,
            const RoleTarget &role,
            size_t workerLimit,
            const fs::path &cacheDir,
            const std::string &asciiTtfHash)
        {
            if (asciiTtfPath.empty())
                return;

            std::set<uint32_t> asciiCodes;
            std::set<uint32_t> missingCodes;
            GlyphRenderer probe(asciiTtfPath, role.renderFontSize);
            for (uint32_t code : requiredCodes)
            {
                if (code < 0x20 || code > 0x7E)
                    continue;
                if (probe.HasGlyph(code))
                    asciiCodes.insert(code);
                else
                    missingCodes.insert(code);
            }

            if (!missingCodes.empty())
            {
                std::cout << "  ASCII override font is missing " << missingCodes.size()
                    << " requested ASCII glyph(s); keeping their existing sources\n";
            }
            if (asciiCodes.empty())
                return;

            std::map<uint32_t, GlyphBitmap> originalGlyphs =
                ExtractOriginalGlyphs(sourceSection, decodedOriginalBgra, originalWidth, originalHeight);
            const int targetCellHeight = OriginalCellHeight(originalGlyphs, role.renderFontSize);
            const int targetTopPadding = OriginalTopPadding(originalGlyphs);

            std::map<uint32_t, GlyphBitmap> merged;
            for (GlyphBitmap &glyph : glyphs)
                merged.emplace(glyph.code, std::move(glyph));

            auto inkBottom = [](const GlyphBitmap &glyph) {
                for (int y = glyph.height - 1; y >= 0; y--)
                {
                    const auto row = glyph.alpha.begin() + static_cast<std::ptrdiff_t>(y) * glyph.width;
                    if (std::any_of(row, row + glyph.width, [](uint8_t alpha) { return alpha != 0; }))
                        return y;
                }
                return -1;
            };
            auto median = [](std::vector<int> values) {
                if (values.empty())
                    return -1;
                std::sort(values.begin(), values.end());
                return values[values.size() / 2];
            };
            auto isCjkIdeograph = [](uint32_t code) {
                return (code >= 0x3400 && code <= 0x4DBF) ||
                    (code >= 0x4E00 && code <= 0x9FFF) ||
                    (code >= 0xF900 && code <= 0xFAFF);
            };

            std::vector<int> cjkBaselines;
            for (const auto &entry : merged)
            {
                if (!isCjkIdeograph(entry.first))
                    continue;
                const int bottom = inkBottom(entry.second);
                if (bottom >= 0)
                    cjkBaselines.push_back(bottom);
            }
            const int targetBaseline = median(std::move(cjkBaselines));

            std::vector<GlyphBitmap> rendered = RenderGlyphs(
                asciiTtfPath,
                asciiCodes,
                role.renderFontSize,
                workerLimit,
                cacheDir,
                asciiTtfHash);
            for (GlyphBitmap &glyph : rendered)
                AlignTtfGlyphToOriginalCell(glyph, targetCellHeight, targetTopPadding);

            std::vector<int> asciiBaselines;
            for (const GlyphBitmap &glyph : rendered)
            {
                if (!((glyph.code >= 'A' && glyph.code <= 'Z') ||
                    (glyph.code >= '0' && glyph.code <= '9')))
                {
                    continue;
                }
                const int bottom = inkBottom(glyph);
                if (bottom >= 0)
                    asciiBaselines.push_back(bottom);
            }
            const int asciiBaseline = median(std::move(asciiBaselines));
            const int baselineShift = targetBaseline >= 0 && asciiBaseline >= 0
                ? targetBaseline - asciiBaseline
                : 0;

            if (baselineShift != 0)
            {
                for (GlyphBitmap &glyph : rendered)
                {
                    int minInkY = glyph.height;
                    int maxInkY = -1;
                    for (int y = 0; y < glyph.height; y++)
                    {
                        const auto row = glyph.alpha.begin() + static_cast<std::ptrdiff_t>(y) * glyph.width;
                        if (std::any_of(row, row + glyph.width, [](uint8_t alpha) { return alpha != 0; }))
                        {
                            minInkY = std::min(minInkY, y);
                            maxInkY = y;
                        }
                    }
                    if (maxInkY < 0)
                        continue;

                    // Preserve at least one transparent row at both edges. The
                    // baseline shift applies to the whole ASCII font, so commas,
                    // periods and descenders retain their TTF-relative placement.
                    const int glyphShift = std::max(baselineShift, 1 - minInkY);
                    const int alignedHeight = std::max(glyph.height, maxInkY + glyphShift + 2);
                    std::vector<uint8_t> aligned(
                        static_cast<size_t>(glyph.width) * alignedHeight,
                        0);
                    for (int y = 0; y < glyph.height; y++)
                    {
                        const int dstY = y + glyphShift;
                        if (dstY < 0 || dstY >= alignedHeight)
                            continue;
                        std::copy_n(
                            glyph.alpha.begin() + static_cast<std::ptrdiff_t>(y) * glyph.width,
                            glyph.width,
                            aligned.begin() + static_cast<std::ptrdiff_t>(dstY) * glyph.width);
                    }
                    glyph.height = alignedHeight;
                    glyph.alpha = std::move(aligned);
                }
                std::cout << "  ASCII baseline alignment "
                    << (baselineShift > 0 ? "+" : "") << baselineShift
                    << "px (ASCII " << asciiBaseline << " -> CJK " << targetBaseline << ")\n";
            }

            for (GlyphBitmap &glyph : rendered)
            {
                merged[glyph.code] = std::move(glyph);
            }

            glyphs.clear();
            glyphs.reserve(merged.size());
            for (auto &entry : merged)
                glyphs.push_back(std::move(entry.second));

            std::cout << "  ASCII override " << asciiTtfPath.filename().u8string()
                << " supplied " << asciiCodes.size() << " glyph(s)\n";
        }

        const TexTextureRecord &FindRecord(const TexFile &tex, const std::string &name)
        {
            auto it = std::find_if(tex.records.begin(), tex.records.end(), [&name](const TexTextureRecord &record) {
                return record.name == name;
            });
            if (it == tex.records.end())
                throw std::runtime_error("texture not found: " + name);
            return *it;
        }

        std::vector<RoleTarget> DefaultRoleTargets(
            size_t sectionCount,
            uint32_t font1Size,
            uint32_t font2Size,
            uint32_t font1RenderSize,
            uint32_t font2RenderSize)
        {
            std::vector<RoleTarget> roles;
            if (sectionCount >= 3)
            {
                // Font section ids and TEX names are crossed in Manhunt 2:
                // section 1 samples font2, while section 2 samples font1.
                // The regional font1r/font2r textures remain unchanged.
                roles.push_back({1, "font2", static_cast<int>(font2Size), static_cast<int>(font2RenderSize)});
                roles.push_back({2, "font1", static_cast<int>(font1Size), static_cast<int>(font1RenderSize)});
            }
            else
            {
                if (sectionCount >= 1)
                    roles.push_back({0, "font2", static_cast<int>(font2Size), static_cast<int>(font2RenderSize)});
                if (sectionCount >= 2)
                    roles.push_back({1, "font1", static_cast<int>(font1Size), static_cast<int>(font1RenderSize)});
            }
            return roles;
        }
    }

    void BuildLocalizationPackage(const LocalizationBuildOptions &options, const TranslationInput &translations)
    {
        if (options.originalDirectory.empty() || options.outputDir.empty() || translations.documents.empty())
            throw std::runtime_error("build requires originalDirectory, outputDir and prepared translations");
        RenderCache renderCache;
        struct CacheScope
        {
            RenderCache *previous;
            explicit CacheScope(RenderCache *cache) : previous(activeRenderCache) { activeRenderCache = cache; }
            ~CacheScope() { activeRenderCache = previous; }
        } cacheScope(&renderCache);
        if (options.font1Size < 4 || options.font1Size > 256 ||
            options.font2Size < 4 || options.font2Size > 256)
        {
            throw std::runtime_error("font1Size and font2Size must be between 4 and 256");
        }
        const uint32_t font1RenderSize = options.font1RenderSize == 0 ? options.font1Size : options.font1RenderSize;
        const uint32_t font2RenderSize = options.font2RenderSize == 0 ? options.font2Size : options.font2RenderSize;
        if (font1RenderSize < 4 || font1RenderSize > 256 ||
            font2RenderSize < 4 || font2RenderSize > 256)
        {
            throw std::runtime_error("font1RenderSize and font2RenderSize must be between 4 and 256");
        }
        if (!std::isfinite(options.font1AlphaGain) ||
            !std::isfinite(options.font2AlphaGain) ||
            options.font1AlphaGain < 0.1f || options.font1AlphaGain > 4.0f ||
            options.font2AlphaGain < 0.1f || options.font2AlphaGain > 4.0f)
        {
            throw std::runtime_error("font1AlphaGain and font2AlphaGain must be between 0.1 and 4");
        }

        const fs::path font1TtfPath = options.font1TtfPath.empty() ? options.ttfPath : options.font1TtfPath;
        const fs::path font2TtfPath = options.font2TtfPath.empty() ? options.ttfPath : options.font2TtfPath;
        if (font1TtfPath.empty() || font2TtfPath.empty())
            throw std::runtime_error("build requires --ttf or both --font1-ttf and --font2-ttf");

        fs::path sourceRoot = options.originalDirectory;
        fs::path sourceFontDat = sourceRoot / "global" / "initscripts" / "font.dat";
        fs::path sourceTex = sourceRoot / "global" / "pictures" / "global_pc.tex";
        fs::path texTemplate = options.texTemplatePath.empty() ? sourceTex : options.texTemplatePath;
        if (!fs::is_regular_file(sourceFontDat))
            throw std::runtime_error("source font.dat not found: " + sourceFontDat.u8string());
        if (!fs::is_regular_file(sourceTex))
            throw std::runtime_error("source global_pc.tex not found: " + sourceTex.u8string());
        if (!fs::is_regular_file(texTemplate))
            throw std::runtime_error("TEX template global_pc.tex not found: " + texTemplate.u8string());
        if (!fs::is_regular_file(font1TtfPath))
            throw std::runtime_error("font1 TTF not found: " + font1TtfPath.u8string());
        if (!fs::is_regular_file(font2TtfPath))
            throw std::runtime_error("font2 TTF not found: " + font2TtfPath.u8string());
        if (!options.fallbackTtfPath.empty() && !fs::is_regular_file(options.fallbackTtfPath))
            throw std::runtime_error("fallback TTF not found: " + options.fallbackTtfPath.u8string());
        if (!options.asciiTtfPath.empty() && !fs::is_regular_file(options.asciiTtfPath))
            throw std::runtime_error("ASCII TTF not found: " + options.asciiTtfPath.u8string());

        fs::path outFontDat = options.outputDir / "global" / "initscripts" / "font.dat";
        fs::path outTex = options.outputDir / "global" / "pictures" / "global_pc.tex";
        const fs::path cacheDir = options.cacheGlyphs ? options.cacheDirectory : fs::path();
        const std::set<uint32_t> &codes = translations.codepoints;
        std::cout << "Codepoints: " << codes.size() << "\n";

        GdiplusSession gdiplus;
        FontDatFile fontDat = ReadFontDat(sourceFontDat);
        TexFile tex = ReadTex(sourceTex);
        std::vector<RoleTarget> roles = DefaultRoleTargets(
            fontDat.sections.size(),
            options.font1Size,
            options.font2Size,
            font1RenderSize,
            font2RenderSize);

        unsigned int hardware = std::thread::hardware_concurrency();
        if (hardware == 0)
            hardware = 4;
        size_t renderWorkerLimit = options.workerThreads ? options.workerThreads : std::max<size_t>(1, hardware > 1 ? hardware - 1 : 1);
        const std::map<std::string, fs::path> roleTtfPaths = {
            {"font1", font1TtfPath},
            {"font2", font2TtfPath}};
        const std::map<std::string, std::string> roleTtfHashes = {
            {"font1", HashFileHex(font1TtfPath)},
            {"font2", HashFileHex(font2TtfPath)}};
        std::string fallbackTtfHash = options.fallbackTtfPath.empty()
            ? std::string()
            : HashFileHex(options.fallbackTtfPath);
        std::string asciiTtfHash = options.asciiTtfPath.empty()
            ? std::string()
            : HashFileHex(options.asciiTtfPath);

        std::map<std::string, std::vector<uint8_t>> replacementDds;

        for (RoleTarget role : roles)
        {
            const FontDatSection &sourceSection = fontDat.sections[role.sectionIndex];
            const fs::path &roleTtfPath = roleTtfPaths.at(role.textureName);
            const std::string &roleTtfHash = roleTtfHashes.at(role.textureName);
            const TexTextureRecord &record = FindRecord(tex, role.textureName);
            std::vector<uint8_t> originalDds(
                tex.decompressed.begin() + static_cast<std::ptrdiff_t>(record.dataOffset),
                tex.decompressed.begin() + static_cast<std::ptrdiff_t>(record.dataOffset + record.dataSize));
            DdsInfo originalInfo = ReadDdsInfo(originalDds);

            std::cout << "Rendering font section " << sourceSection.fontId << " -> " << role.textureName
                << " with " << roleTtfPath.filename().u8string()
                << " at " << role.renderFontSize << "px"
                << " into " << role.outputFontSize << "px game line box"
                << " using " << (options.glyphSourcePolicy == GlyphSourcePolicy::TtfFirst ? "TTF-first" : "original-first")
                << " (base " << record.width << "x" << record.height << ", " << originalInfo.mipCount << " mips)\n";

            uint32_t decodedWidth = 0;
            uint32_t decodedHeight = 0;
            std::vector<uint8_t> decodedOriginal = DecodeDxt5DdsFirstMip(originalDds, decodedWidth, decodedHeight);
            std::vector<GlyphBitmap> glyphs;
            {
                StageTimer timer("select glyph sources and render " + role.textureName);
                if (options.glyphSourcePolicy == GlyphSourcePolicy::TtfFirst)
                {
                    glyphs = BuildTtfFirstGlyphs(
                        sourceSection,
                        decodedOriginal,
                        decodedWidth,
                        decodedHeight,
                        codes,
                        roleTtfPath,
                        options.fallbackTtfPath,
                        role,
                        renderWorkerLimit,
                        cacheDir,
                        roleTtfHash,
                        fallbackTtfHash);
                }
                else
                {
                    glyphs = BuildOriginalFirstGlyphs(
                        sourceSection,
                        decodedOriginal,
                        decodedWidth,
                        decodedHeight,
                        codes,
                        roleTtfPath,
                        options.fallbackTtfPath,
                        role,
                        renderWorkerLimit,
                        cacheDir,
                        roleTtfHash,
                        fallbackTtfHash);
                }
            }
            if (!options.asciiTtfPath.empty())
            {
                StageTimer timer("apply ASCII font override to " + role.textureName);
                ApplyAsciiTtfOverride(
                    glyphs,
                    sourceSection,
                    decodedOriginal,
                    decodedWidth,
                    decodedHeight,
                    codes,
                    options.asciiTtfPath,
                    role,
                    renderWorkerLimit,
                    cacheDir,
                    asciiTtfHash);
            }
            std::cout << "  glyphs " << glyphs.size() << " ("
                << (options.glyphSourcePolicy == GlyphSourcePolicy::TtfFirst
                    ? "TTF glyphs preferred; original used only as fallback"
                    : "original glyphs kept when present")
                << ")\n";
            const float alphaGain = role.textureName == "font1"
                ? options.font1AlphaGain
                : options.font2AlphaGain;
            if (alphaGain != 1.0f)
            {
                StageTimer timer("amplify TTF alpha for " + role.textureName);
                const size_t changed = ApplyTtfAlphaGain(glyphs, alphaGain);
                std::cout << "  TTF alpha gain " << alphaGain
                    << " applied to " << changed << " glyphs; original fallback unchanged\n";
            }
            else
            {
                std::cout << "  TTF alpha gain disabled\n";
            }
            const uint32_t outlineWidth = role.textureName == "font1"
                ? options.font1OutlineWidth
                : options.font2OutlineWidth;
            if (outlineWidth != 0)
            {
                StageTimer timer("add black outline to " + role.textureName);
                for (GlyphBitmap &glyph : glyphs)
                    AddBlackOutline(glyph, outlineWidth);
                std::cout << "  black outline " << outlineWidth << "px; advances unchanged\n";
            }
            else
            {
                std::cout << "  black outline disabled\n";
            }
            PackedAtlas atlas;
            {
                StageTimer timer("pack atlas for " + role.textureName);
                atlas = PackAtlasAutoSize(glyphs, record.width, record.height);
            }
            std::cout << "  atlas " << atlas.width << "x" << atlas.height << "\n";
            fontDat.sections[role.sectionIndex] = BuildDatSectionFromAtlas(
                sourceSection,
                atlas,
                static_cast<uint32_t>(role.outputFontSize));
            const uint32_t mipCount = 0; // One physical mip; retain the proven Manhunt 2 DDS header convention.
            {
                StageTimer timer("encode DDS for " + role.textureName);
                replacementDds[role.textureName] = EncodeDxt5Dds(atlas.bgra, atlas.width, atlas.height, mipCount, options.workerThreads);
            }
        }

        std::cout << "Writing font.dat...\n";
        WriteFontDat(outFontDat, fontDat);
        std::cout << "Writing global_pc.tex...\n";
        {
            StageTimer timer("write TEX");
            WriteFontTexFromTemplate(texTemplate, sourceTex, outTex, replacementDds);
        }
        for (const auto &document : translations.documents)
            WriteGxt(options.outputDir / document.relativePath, document.gxt);
        std::cout << "Generated " << translations.documents.size() << " GXT files and font resources\n";
    }
}
