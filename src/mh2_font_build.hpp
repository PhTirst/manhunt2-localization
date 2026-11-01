#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include "translations.hpp"

namespace mh2
{
    enum class GlyphSourcePolicy
    {
        OriginalFirst,
        TtfFirst
    };

    struct LocalizationBuildOptions
    {
        std::filesystem::path originalDirectory;
        std::filesystem::path texTemplatePath;
        std::filesystem::path cacheDirectory;
        bool cacheGlyphs = false;
        size_t workerThreads = 0;
        std::filesystem::path jsonDir;
        std::filesystem::path ttfPath;
        std::filesystem::path font1TtfPath;
        std::filesystem::path font2TtfPath;
        std::filesystem::path fallbackTtfPath;
        std::filesystem::path asciiTtfPath;
        std::filesystem::path outputDir;
        uint32_t font1Size = 31;
        uint32_t font2Size = 30;
        uint32_t font1RenderSize = 0;
        uint32_t font2RenderSize = 0;
        uint32_t font1OutlineWidth = 0;
        uint32_t font2OutlineWidth = 0;
        float font1AlphaGain = 1.0f;
        float font2AlphaGain = 1.0f;
        GlyphSourcePolicy glyphSourcePolicy = GlyphSourcePolicy::TtfFirst;
        std::string language = "zh_CN";
    };

    void BuildLocalizationPackage(const LocalizationBuildOptions &options, const TranslationInput &translations);
}
