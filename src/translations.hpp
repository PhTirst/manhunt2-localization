#pragma once
#include "mh2_gxt.hpp"
#include <set>

namespace mh2
{
    struct TranslationDocument { std::filesystem::path relativePath; GxtFile gxt; };
    struct TranslationInput
    {
        std::vector<TranslationDocument> documents;
        std::set<uint32_t> codepoints;
        std::map<std::filesystem::path, std::string> sourceHashes;
    };
    TranslationInput CompileTranslations(const std::filesystem::path &directory, const std::string &language);
    void VerifyTranslationSources(const TranslationInput &input, const std::filesystem::path &directory);
}
