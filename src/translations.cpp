#include "translations.hpp"
#include "file_io.hpp"
#include "safe_files.hpp"
#include <algorithm>
#include <stdexcept>

namespace fs = std::filesystem;
namespace mh2
{
    TranslationInput CompileTranslations(const fs::path &directory, const std::string &language)
    {
        ValidateLanguage(language);
        TranslationInput result;
        for (const auto &relative : ResourceFiles(directory, ".json"))
        {
            const auto path = directory / relative;
            GxtFile gxt;
            const auto source = ReadText(path);
            result.sourceHashes[relative] = Sha256({source.begin(), source.end()});
            const auto json = ParseJson(source, path.u8string());
            try { gxt = JsonToGxt(json, language, false); }
            catch (const std::exception &error) { throw std::runtime_error(path.u8string() + ": " + error.what()); }
            for (const auto &entry : gxt.entries)
            {
                for (size_t i = 0; i < entry.value.size(); ++i)
                {
                    const uint32_t code = entry.value[i];
                    if (code == '~')
                    {
                        const auto end = entry.value.find(u'~', i + 1);
                        if (end != std::u16string::npos) { i = end; continue; }
                    }
                    if (code >= 0xD800 && code <= 0xDFFF)
                        throw std::runtime_error(path.u8string() + ": key '" + entry.key + "': non-BMP characters are not supported by the game's font renderer");
                    if (code >= 0x20) result.codepoints.insert(code);
                }
            }
            auto output = relative;
            output.replace_extension(".gxt");
            result.documents.push_back({output, std::move(gxt)});
        }
        if (result.documents.empty()) throw std::runtime_error(directory.u8string() + ": no JSON files found");
        for (uint32_t code = 0x20; code <= 0x7E; ++code) result.codepoints.insert(code);
        return result;
    }
    void VerifyTranslationSources(const TranslationInput &input, const fs::path &directory)
    {
        const auto paths = ResourceFiles(directory, ".json");
        if (paths.size() != input.sourceHashes.size()) throw std::runtime_error("JSON inputs changed during the build; run again");
        for (const auto &path : paths)
        {
            const auto found = input.sourceHashes.find(path);
            if (found == input.sourceHashes.end() || FileHash(directory / path) != found->second)
                throw std::runtime_error((directory / path).u8string() + ": JSON input changed during the build; run again");
        }
    }
}
