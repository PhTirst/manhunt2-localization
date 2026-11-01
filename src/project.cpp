#include "project.hpp"
#include "file_io.hpp"
#include "json.hpp"
#include "safe_files.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
namespace
{
    void Object(const mh2::JsonValue &value, const std::string &where, const std::set<std::string> &allowed)
    {
        if (!value.IsObject()) throw std::runtime_error(where + ": expected an object");
        for (const auto &item : value.objectValue)
            if (!allowed.count(item.first)) throw std::runtime_error(where + ": unknown option '" + item.first + "'");
    }
    std::string String(const mh2::JsonValue &object, const std::string &key, const std::string &fallback)
    {
        const auto *value = object.Find(key);
        if (!value) return fallback;
        if (!value->IsString() || value->stringValue.empty() || value->stringValue.find('\0') != std::string::npos)
            throw std::runtime_error("Option '" + key + "' must be a nonempty string");
        return value->stringValue;
    }
    double Number(const mh2::JsonValue &object, const std::string &key, double fallback, double low, double high, bool integer = true)
    {
        const auto *value = object.Find(key);
        if (!value) return fallback;
        if (!value->IsNumber() || !std::isfinite(value->numberValue) || value->numberValue < low || value->numberValue > high ||
            (integer && std::floor(value->numberValue) != value->numberValue))
            throw std::runtime_error("Option '" + key + "' must be " + (integer ? "an integer" : "a number") + " in [" + std::to_string(low) + ", " + std::to_string(high) + "]");
        return value->numberValue;
    }
    fs::path Path(const mh2::JsonValue &root, const std::string &key, const std::string &fallback, const fs::path &base)
    {
        const auto path = fs::u8path(String(root, key, fallback));
        return mh2::AbsolutePath(path.is_absolute() ? path : base / path);
    }
    fs::path OptionalPath(const mh2::JsonValue &root, const std::string &key, const fs::path &base)
    {
        const auto *value = root.Find(key);
        if (!value || value->IsNull()) return {};
        return Path(root, key, "", base);
    }
    void Font(const mh2::JsonValue &root, const char *name, mh2::FontSettings &settings)
    {
        const auto *value = root.Find(name);
        if (!value) return;
        Object(*value, name, {"size", "renderSize", "outlineWidth", "alphaGain"});
        settings.size = (uint32_t)Number(*value, "size", settings.size, 4, 256);
        settings.renderSize = (uint32_t)Number(*value, "renderSize", settings.size, 4, 256);
        settings.outlineWidth = (uint32_t)Number(*value, "outlineWidth", settings.outlineWidth, 0, 8);
        settings.alphaGain = (float)Number(*value, "alphaGain", settings.alphaGain, 0.1, 4, false);
    }
}
namespace mh2
{
    ProjectConfig ReadProject(const fs::path &file)
    {
        ProjectConfig config;
        config.file = AbsolutePath(file);
        config.root = config.file.parent_path();
        RejectLinks(config.file);
        config.sourceText = ReadText(config.file);
        const auto json = ParseJson(config.sourceText, config.file.u8string());
        Object(json, config.file.u8string(), {"language", "gameDirectory", "originalDirectory", "jsonDirectory", "fontDirectory",
            "outputDirectory", "cacheDirectory", "texTemplate", "fallbackFont", "asciiFont", "glyphSource", "font1", "font2", "workerThreads", "cacheGlyphs"});
        config.language = String(json, "language", config.language);
        ValidateLanguage(config.language);
        config.gameDir = Path(json, "gameDirectory", "..", config.root);
        config.originalDir = Path(json, "originalDirectory", "original", config.root);
        config.jsonDir = Path(json, "jsonDirectory", "JSON", config.root);
        config.fontDir = Path(json, "fontDirectory", "TTF", config.root);
        config.outputDir = Path(json, "outputDirectory", "dist", config.root);
        config.cacheDir = Path(json, "cacheDirectory", ".cache", config.root);
        config.texTemplate = OptionalPath(json, "texTemplate", config.root);
        config.fallbackFont = OptionalPath(json, "fallbackFont", config.root);
        config.asciiFont = OptionalPath(json, "asciiFont", config.root);
        const auto policy = String(json, "glyphSource", "ttf-first");
        if (policy != "ttf-first" && policy != "original-first") throw std::runtime_error("glyphSource must be ttf-first or original-first");
        config.glyphSource = policy == "ttf-first" ? GlyphSourcePolicy::TtfFirst : GlyphSourcePolicy::OriginalFirst;
        Font(json, "font1", config.font1); Font(json, "font2", config.font2);
        config.workerThreads = (size_t)Number(json, "workerThreads", 0, 0, 256);
        if (const auto *value = json.Find("cacheGlyphs"))
        {
            if (!value->IsBool()) throw std::runtime_error("cacheGlyphs must be true or false");
            config.cacheGlyphs = value->boolValue;
        }
        for (const auto &path : {config.gameDir, config.originalDir, config.jsonDir, config.fontDir, config.outputDir, config.cacheDir}) RejectLinks(path);
        std::vector<fs::path> inputs{config.originalDir, config.jsonDir, config.fontDir};
        for (const auto &optional : {config.fallbackFont, config.asciiFont})
            if (!optional.empty())
            {
                RejectLinks(optional);
                if (Within(optional, config.outputDir) || Within(config.outputDir, optional) || Within(optional, config.cacheDir) || Within(config.cacheDir, optional))
                    throw std::runtime_error("Optional font input overlaps output/cache directories");
            }
        if (!config.texTemplate.empty())
        {
            RejectLinks(config.texTemplate);
            if (Within(config.texTemplate, config.outputDir) || Within(config.texTemplate, config.cacheDir))
                throw std::runtime_error("TEX template must not be inside output/cache directories");
        }
        for (const auto &input : inputs)
        {
            if (Within(input, config.outputDir) || Within(config.outputDir, input) ||
                Within(input, config.cacheDir) || Within(config.cacheDir, input))
                throw std::runtime_error("Output/cache directories must not overlap input directories");
        }
        if (Within(config.root, config.outputDir) || Within(config.root, config.cacheDir) ||
            Within(config.gameDir, config.outputDir) || Within(config.gameDir, config.cacheDir) ||
            Within(config.outputDir, config.cacheDir) || Within(config.cacheDir, config.outputDir))
            throw std::runtime_error("Output/cache must be separate dedicated directories, not the project or game root");
        for (size_t i = 0; i < inputs.size(); ++i)
            for (size_t j = i + 1; j < inputs.size(); ++j)
                if (Within(inputs[i], inputs[j]) || Within(inputs[j], inputs[i]))
                    throw std::runtime_error("Original, JSON and TTF inputs must be separate directories");
        return config;
    }
    fs::path SingleTtf(const fs::path &directory)
    {
        RejectLinks(directory);
        std::vector<fs::path> fonts;
        if (fs::is_directory(directory))
            for (const auto &entry : fs::directory_iterator(directory))
            {
                RejectLinks(entry.path());
                auto extension = entry.path().extension().u8string();
                for (auto &c : extension) c = (char)std::tolower((unsigned char)c);
                if (entry.is_regular_file() && extension == ".ttf") fonts.push_back(entry.path());
            }
        if (fonts.size() != 1)
        {
            std::string message = directory.u8string() + ": expected exactly one TTF, found " + std::to_string(fonts.size());
            for (const auto &font : fonts) message += "\n  " + font.filename().u8string();
            throw std::runtime_error(message);
        }
        return fonts.front();
    }
    LocalizationBuildOptions BuildOptions(const ProjectConfig &config, const fs::path &stage)
    {
        LocalizationBuildOptions options;
        options.originalDirectory = config.originalDir;
        options.jsonDir = config.jsonDir;
        options.font1TtfPath = SingleTtf(config.fontDir / "font1");
        options.font2TtfPath = SingleTtf(config.fontDir / "font2");
        options.fallbackTtfPath = config.fallbackFont.empty() ? fs::path() : SingleTtf(config.fallbackFont);
        options.asciiTtfPath = config.asciiFont.empty() ? fs::path() : SingleTtf(config.asciiFont);
        options.texTemplatePath = config.texTemplate;
        options.outputDir = stage;
        options.cacheDirectory = config.cacheDir;
        options.cacheGlyphs = config.cacheGlyphs;
        options.workerThreads = config.workerThreads;
        options.font1Size = config.font1.size; options.font2Size = config.font2.size;
        options.font1RenderSize = config.font1.renderSize; options.font2RenderSize = config.font2.renderSize;
        options.font1OutlineWidth = config.font1.outlineWidth; options.font2OutlineWidth = config.font2.outlineWidth;
        options.font1AlphaGain = config.font1.alphaGain; options.font2AlphaGain = config.font2.alphaGain;
        options.glyphSourcePolicy = config.glyphSource;
        options.language = config.language;
        return options;
    }
}
