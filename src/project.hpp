#pragma once
#include "mh2_font_build.hpp"
#include <filesystem>
#include <string>

namespace mh2
{
    struct FontSettings
    {
        uint32_t size = 31;
        uint32_t renderSize = 31;
        uint32_t outlineWidth = 0;
        float alphaGain = 1;
    };
    struct ProjectConfig
    {
        std::filesystem::path file, root, gameDir, originalDir, jsonDir, fontDir, outputDir, cacheDir;
        std::filesystem::path texTemplate, fallbackFont, asciiFont;
        std::string language = "zh_CN";
        std::string sourceText;
        FontSettings font1{31, 31, 0, 2.6f}, font2{30, 30, 0, 2.2f};
        GlyphSourcePolicy glyphSource = GlyphSourcePolicy::TtfFirst;
        size_t workerThreads = 0;
        bool cacheGlyphs = false;
    };
    ProjectConfig ReadProject(const std::filesystem::path &file);
    std::filesystem::path SingleTtf(const std::filesystem::path &directory);
    LocalizationBuildOptions BuildOptions(const ProjectConfig &project, const std::filesystem::path &stage);
}
