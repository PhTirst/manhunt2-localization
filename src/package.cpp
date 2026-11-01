#include "package.hpp"
#include "file_io.hpp"
#include "json.hpp"
#include "mh2_font_dat.hpp"
#include "mh2_tex.hpp"
#include "safe_files.hpp"
#include "translations.hpp"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
namespace
{
    constexpr const char *OriginalManifest = "original.manifest.json";
    constexpr const char *BuildManifest = "build.manifest.json";
    constexpr const char *DeploymentManifest = ".mh2loc-deployment.json";
    using Assets = std::map<std::string, std::string>;

    std::string Lower(std::string value)
    {
        for (auto &c : value) c = (char)std::tolower((unsigned char)c);
        return value;
    }
    std::string Key(const fs::path &path) { return Lower(path.generic_u8string()); }
    bool Managed(const fs::path &path)
    {
        return Key(path) == "global/initscripts/font.dat" || Key(path) == "global/pictures/global_pc.tex" || Lower(path.extension().u8string()) == ".gxt";
    }
    std::vector<uint8_t> Bytes(const mh2::JsonValue &value)
    {
        const auto text = mh2::WriteJson(value, 2) + "\n";
        return {text.begin(), text.end()};
    }
    std::string String(const mh2::JsonValue &object, const std::string &field)
    {
        const auto *value = object.Find(field);
        if (!value || !value->IsString()) throw std::runtime_error("Manifest missing string '" + field + "'");
        return value->stringValue;
    }
    mh2::JsonValue Manifest(const fs::path &path, const char *format)
    {
        mh2::RejectLinks(path);
        const auto json = mh2::ParseJson(mh2::ReadText(path), path.u8string());
        if (!json.IsObject() || String(json, "format") != format) throw std::runtime_error(path.u8string() + ": incompatible manifest format");
        return json;
    }
    Assets Records(const mh2::JsonValue &json)
    {
        const auto *files = json.Find("files");
        if (!files || !files->IsArray()) throw std::runtime_error("Manifest missing files array");
        Assets result;
        for (const auto &row : files->arrayValue)
        {
            const auto path = fs::u8path(String(row, "path"));
            mh2::ValidateRelative(path);
            if (!Managed(path)) throw std::runtime_error("Manifest contains an unmanaged resource path: " + path.u8string());
            const auto hash = String(row, "sha256");
            if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](unsigned char c) { return std::isxdigit(c); }))
                throw std::runtime_error("Manifest contains an invalid SHA-256");
            if (!result.emplace(Key(path), Lower(hash)).second) throw std::runtime_error("Duplicate path in manifest: " + path.u8string());
        }
        return result;
    }
    mh2::JsonValue JsonRecords(const Assets &assets)
    {
        std::vector<mh2::JsonValue> rows;
        for (const auto &file : assets)
            rows.push_back(mh2::JsonValue::Object({{"path", mh2::JsonValue::String(file.first)}, {"sha256", mh2::JsonValue::String(file.second)}}));
        return mh2::JsonValue::Array(std::move(rows));
    }
    void VerifyFiles(const fs::path &root, const Assets &files)
    {
        for (const auto &item : files)
        {
            const auto path = root / fs::u8path(item.first);
            mh2::RejectLinks(path);
            if (!fs::is_regular_file(path) || mh2::FileHash(path) != item.second)
                throw std::runtime_error("Missing/modified managed resource: " + path.u8string());
        }
    }
    Assets OriginalFiles(const mh2::ProjectConfig &project)
    {
        const auto files = Records(Manifest(project.originalDir / OriginalManifest, "mh2loc-original-v1"));
        if (!files.count("global/initscripts/font.dat") || !files.count("global/pictures/global_pc.tex"))
            throw std::runtime_error("Original snapshot must contain font.dat and global_pc.tex");
        VerifyFiles(project.originalDir, files);
        return files;
    }
    Assets HashManagedFiles(const fs::path &root)
    {
        Assets files;
        for (const auto &path : mh2::ResourceFiles(root))
        {
            if (!Managed(path)) continue;
            if (!files.emplace(Key(path), mh2::FileHash(root / path)).second)
                throw std::runtime_error("Case-insensitive duplicate resource path: " + path.u8string());
        }
        return files;
    }
    void WriteOriginalManifest(const fs::path &root)
    {
        const auto fonts = mh2::ReadFontDat(root / "global/initscripts/font.dat");
        const auto textures = mh2::ReadTex(root / "global/pictures/global_pc.tex");
        if (fonts.sections.size() < 3 || textures.records.empty()) throw std::runtime_error("Original font resources do not match the supported Manhunt 2 PC layout");
        const auto files = HashManagedFiles(root);
        mh2::WriteBytes(root / OriginalManifest, Bytes(mh2::JsonValue::Object({
            {"format", mh2::JsonValue::String("mh2loc-original-v1")}, {"files", JsonRecords(files)}})));
    }
    void Game(const mh2::ProjectConfig &project)
    {
        mh2::RejectLinks(project.gameDir);
        if (!fs::is_regular_file(project.gameDir / "Manhunt2.exe"))
            throw std::runtime_error("gameDirectory must be the installed game folder containing Manhunt2.exe: " + project.gameDir.u8string());
    }
    void Target(const mh2::ProjectConfig &project, const fs::path &relative)
    {
        mh2::ValidateRelative(relative);
        const auto target = project.gameDir / relative;
        mh2::RejectLinks(target);
        if (!mh2::Within(target, project.gameDir)) throw std::runtime_error("Game target escapes the game directory");
        for (const auto &protectedRoot : {project.originalDir, project.jsonDir, project.fontDir, project.outputDir, project.cacheDir})
            if (mh2::Within(target, protectedRoot)) throw std::runtime_error("Game target overlaps a protected input/output directory: " + target.u8string());
    }
    Assets OutputFiles(const mh2::ProjectConfig &project, mh2::JsonValue *manifest = nullptr)
    {
        const auto document = Manifest(project.outputDir / BuildManifest, "mh2loc-build-v1");
        const auto files = Records(document);
        if (!files.count("global/initscripts/font.dat") || !files.count("global/pictures/global_pc.tex"))
            throw std::runtime_error("Build manifest lacks font resources");
        VerifyFiles(project.outputDir, files);
        for (const auto &path : mh2::ResourceFiles(project.outputDir))
            if (path != BuildManifest && !files.count(Key(path)))
                throw std::runtime_error("Unmanaged file in output directory; refusing to replace it: " + path.u8string());
        if (manifest) *manifest = document;
        return files;
    }
    std::string InputHash(const mh2::ProjectConfig &project, const mh2::LocalizationBuildOptions &options)
    {
        if (mh2::ReadText(project.file) != project.sourceText) throw std::runtime_error("Project configuration changed; read it again before building");
        std::string text = "mh2loc-" MH2LOC_GENERATOR_VERSION "-generator-v1\n" + project.sourceText;
        for (const auto &path : mh2::ResourceFiles(project.jsonDir, ".json"))
            text += "\n" + path.generic_u8string() + ":" + mh2::FileHash(project.jsonDir / path);
        for (const auto &path : {options.font1TtfPath, options.font2TtfPath, options.fallbackTtfPath, options.asciiTtfPath,
            project.originalDir / "global/initscripts/font.dat", project.originalDir / "global/pictures/global_pc.tex", options.texTemplatePath})
            if (!path.empty()) { mh2::RejectLinks(path); text += "\n" + mh2::FileHash(path); }
        return mh2::Sha256({text.begin(), text.end()});
    }
    std::vector<uint8_t> VerifiedBytes(const fs::path &path, const std::string &expected)
    {
        const auto bytes = mh2::ReadBytes(path);
        if (mh2::Sha256(bytes) != expected) throw std::runtime_error("Resource changed while preparing deployment: " + path.u8string());
        return bytes;
    }
}

namespace mh2
{
    void CaptureOriginals(const ProjectConfig &project)
    {
        OperationLock lock(project.originalDir);
        Game(project);
        if (fs::exists(project.originalDir / OriginalManifest)) { OriginalFiles(project); std::cout << "Original snapshot already sealed; not replaced.\n"; return; }
        if (fs::exists(project.originalDir) && !fs::is_empty(project.originalDir))
            throw std::runtime_error("Original directory is not empty. Use seal-originals only after verifying these are clean original resources.");
        if (fs::exists(project.gameDir / DeploymentManifest)) throw std::runtime_error("Restore the active deployment before capturing originals");
        const auto stage = UniqueSibling(project.originalDir, "snapshot");
        fs::create_directories(stage);
        try
        {
            std::vector<fs::path> paths{fs::u8path("global/initscripts/font.dat"), fs::u8path("global/pictures/global_pc.tex")};
            for (const auto &relative : ResourceFiles(project.gameDir, ".gxt"))
            {
                const auto path = project.gameDir / relative;
                if ((project.root != project.gameDir && Within(project.root, project.gameDir) && Within(path, project.root)) ||
                    Within(path, project.originalDir) || Within(path, stage) || Within(path, project.outputDir) ||
                    Within(path, project.cacheDir) || Within(path, project.jsonDir) || Within(path, project.fontDir)) continue;
                paths.push_back(relative);
            }
            for (const auto &relative : paths) WriteBytes(stage / relative, ReadBytes(project.gameDir / relative));
            WriteOriginalManifest(stage);
            CommitDirectory(stage, project.originalDir);
        }
        catch (...) { std::error_code ignored; fs::remove_all(stage, ignored); throw; }
        std::cout << "Captured clean originals. Keep this snapshot immutable.\n";
    }
    void SealOriginals(const ProjectConfig &project)
    {
        OperationLock lock(project.originalDir);
        RejectLinks(project.originalDir);
        if (fs::exists(project.originalDir / OriginalManifest)) { OriginalFiles(project); std::cout << "Original snapshot already sealed.\n"; return; }
        WriteOriginalManifest(project.originalDir);
        std::cout << "Sealed supplied original resources.\n";
    }
    bool BuildProject(const ProjectConfig &project, bool force)
    {
        OperationLock lock(project.outputDir);
        const auto translations = CompileTranslations(project.jsonDir, project.language);
        OriginalFiles(project);
        const auto stage = UniqueSibling(project.outputDir, "stage");
        const auto options = BuildOptions(project, stage);
        const auto fingerprint = InputHash(project, options);
        VerifyTranslationSources(translations, project.jsonDir);
        if (!force && fs::is_regular_file(project.outputDir / BuildManifest))
        {
            try
            {
                JsonValue manifest;
                OutputFiles(project, &manifest);
                if (String(manifest, "inputHash") == fingerprint) { VerifyTranslationSources(translations, project.jsonDir); std::cout << "Build cache hit: output hashes verified.\n"; return false; }
            }
            catch (const std::exception &) {} // Missing/modified known outputs cause a rebuild.
        }
        if (fs::exists(project.outputDir) && !fs::is_empty(project.outputDir))
        {
            // A disposable output directory must never absorb untracked user files.
            Assets files;
            try { files = Records(Manifest(project.outputDir / BuildManifest, "mh2loc-build-v1")); }
            catch (const std::exception &)
            {
                // A lost cache manifest can be recreated only when every existing
                // file is an output of the current, explicitly selected inputs.
                files["global/initscripts/font.dat"] = "";
                files["global/pictures/global_pc.tex"] = "";
                for (const auto &document : translations.documents) files[Key(document.relativePath)] = "";
            }
            for (const auto &path : ResourceFiles(project.outputDir))
                if (path != BuildManifest && !files.count(Key(path)))
                    throw std::runtime_error("Unmanaged file in output directory: " + path.u8string());
        }
        fs::create_directories(stage);
        try
        {
            BuildLocalizationPackage(options, translations);
            VerifyTranslationSources(translations, project.jsonDir);
            if (InputHash(project, BuildOptions(project, stage)) != fingerprint)
                throw std::runtime_error("Build inputs changed during generation; the previous package is unchanged. Run again.");
            const auto files = HashManagedFiles(stage);
            WriteBytes(stage / BuildManifest, Bytes(JsonValue::Object({{"format", JsonValue::String("mh2loc-build-v1")},
                {"inputHash", JsonValue::String(fingerprint)}, {"language", JsonValue::String(project.language)}, {"files", JsonRecords(files)}})));
            CommitDirectory(stage, project.outputDir);
        }
        catch (...) { std::error_code ignored; fs::remove_all(stage, ignored); throw; }
        std::cout << "Build complete: " << project.outputDir.u8string() << '\n';
        return true;
    }
    void DeployProject(const ProjectConfig &project)
    {
        OperationLock gameLock(project.gameDir);
        OperationLock outputLock(project.outputDir);
        Game(project);
        const auto original = OriginalFiles(project);
        const auto outputs = OutputFiles(project);
        const auto baseline = FileHash(project.originalDir / OriginalManifest);
        const auto journalPath = project.gameDir / DeploymentManifest;
        Assets previous;
        if (fs::exists(journalPath))
        {
            const auto journal = Manifest(journalPath, "mh2loc-deploy-v1");
            if (String(journal, "baseline") != baseline) throw std::runtime_error("Another original snapshot owns the active deployment; restore that project first");
            previous = Records(journal);
            VerifyFiles(project.gameDir, previous);
        }
        Assets all = previous;
        all.insert(outputs.begin(), outputs.end());
        std::vector<FileChange> changes;
        for (const auto &item : all)
        {
            const auto relative = fs::u8path(item.first);
            Target(project, relative);
            const auto target = project.gameDir / relative;
            if (!previous.count(item.first) && fs::exists(target))
            {
                const auto found = original.find(item.first);
                if (found == original.end() || FileHash(target) != found->second)
                    throw std::runtime_error("Game resource is not the captured original; refusing to overwrite another modification: " + target.u8string());
            }
            if (outputs.count(item.first)) changes.push_back({target, VerifiedBytes(project.outputDir / relative, outputs.at(item.first))});
            else if (original.count(item.first)) changes.push_back({target, VerifiedBytes(project.originalDir / relative, original.at(item.first))});
            else changes.push_back({target, std::nullopt});
        }
        changes.push_back({journalPath, Bytes(JsonValue::Object({{"format", JsonValue::String("mh2loc-deploy-v1")},
            {"baseline", JsonValue::String(baseline)}, {"files", JsonRecords(outputs)}}))});
        CommitFiles(changes);
        std::cout << "Deployed " << outputs.size() << " managed resources. Other game files were not changed.\n";
    }
    void RestoreProject(const ProjectConfig &project)
    {
        OperationLock lock(project.gameDir);
        Game(project);
        const auto path = project.gameDir / DeploymentManifest;
        if (!fs::exists(path)) { std::cout << "No deployment journal; nothing to restore.\n"; return; }
        const auto original = OriginalFiles(project);
        const auto journal = Manifest(path, "mh2loc-deploy-v1");
        if (String(journal, "baseline") != FileHash(project.originalDir / OriginalManifest))
            throw std::runtime_error("Original snapshot does not match the active deployment");
        const auto deployed = Records(journal);
        VerifyFiles(project.gameDir, deployed);
        std::vector<FileChange> changes;
        for (const auto &item : deployed)
        {
            const auto relative = fs::u8path(item.first);
            Target(project, relative);
            if (original.count(item.first)) changes.push_back({project.gameDir / relative, VerifiedBytes(project.originalDir / relative, original.at(item.first))});
            else changes.push_back({project.gameDir / relative, std::nullopt});
        }
        changes.push_back({path, std::nullopt});
        CommitFiles(changes);
        std::cout << "Restored original resources and removed added translation files. Other files were not changed.\n";
    }
}
