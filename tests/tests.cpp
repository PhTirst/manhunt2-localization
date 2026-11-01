#include "package.hpp"
#include "file_io.hpp"
#include "mh2_dds.hpp"
#include "mh2_font_dat.hpp"
#include "mh2_font_preview.hpp"
#include "mh2_tex.hpp"
#include "safe_files.hpp"
#include "translations.hpp"
#include <windows.h>
#include <zlib.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
namespace
{
    size_t assertions = 0;
    void Check(bool success, const std::string &message) { ++assertions; if (!success) throw std::runtime_error(message); }
    template <class F> void Reject(F action, const std::string &message)
    {
        try { action(); }
        catch (const std::exception &error)
        {
            Check(std::string(error.what()).find(message) != std::string::npos, std::string("Unexpected diagnostic: ") + error.what());
            return;
        }
        throw std::runtime_error("Expected failure containing: " + message);
    }
    void Put(std::vector<uint8_t> &bytes, size_t at, uint32_t value)
    {
        Check(at + 4 <= bytes.size(), "Fixture bounds");
        for (int i = 0; i < 4; ++i) bytes[at + i] = (uint8_t)(value >> (8 * i));
    }
    struct Fixture
    {
        fs::path root, game, projectFile;
        explicit Fixture(fs::path path) : root(std::move(path)), game(root / "game"), projectFile(root / "project.json")
        {
            fs::create_directories(root);
            mh2::WriteText(game / "Manhunt2.exe", "synthetic marker, not executable");
            mh2::WriteText(projectFile, R"({"language":"fr_FR","gameDirectory":"game","workerThreads":1})");
        }
        mh2::ProjectConfig Config() const { return mh2::ReadProject(projectFile); }
        void Set(const std::string &name, mh2::JsonValue value)
        {
            auto config = mh2::ParseJson(mh2::ReadText(projectFile));
            config.objectValue[name] = std::move(value);
            mh2::WriteText(projectFile, mh2::WriteJson(config));
        }
    };
    std::vector<uint8_t> SolidDds(uint32_t size, uint8_t intensity = 255)
    {
        std::vector<uint8_t> bgra((size_t)size * size * 4, intensity);
        return mh2::EncodeDxt5Dds(bgra, size, size, 0, 1);
    }
    std::vector<uint8_t> TexFixture(bool smallFonts = false, bool advertisement = false)
    {
        const std::vector<std::string> names{"font1", "font2", "FE_Title_Small", "same", "same"};
        std::vector<std::vector<uint8_t>> payloads;
        std::vector<size_t> capacities;
        for (size_t i = 0; i < names.size(); ++i)
        {
            const uint32_t size = i < 2 ? (smallFonts ? 4 : 256) : 8;
            auto pixels = std::vector<uint8_t>((size_t)size * size * 4, 0);
            if (i < 2 && !smallFonts)
            {
                for (int y = 3; y < 25; ++y)
                    for (int x = 9; x < 14; ++x)
                    {
                        const size_t at = ((size_t)y * size + x) * 4;
                        pixels[at] = pixels[at + 1] = pixels[at + 2] = pixels[at + 3] = 255;
                    }
            }
            else for (size_t at = 0; at < pixels.size(); at += 4)
            {
                pixels[at] = advertisement ? 255 : 32;
                pixels[at + 1] = advertisement ? 16 : 200;
                pixels[at + 2] = (uint8_t)(40 + i);
                pixels[at + 3] = 255;
            }
            payloads.push_back(mh2::EncodeDxt5Dds(pixels, size, size, 0, 1));
            capacities.push_back(i < 2 && !smallFonts ? 1024 * 1024 + 128 : payloads.back().size());
        }
        size_t offset = 64 + names.size() * 128;
        size_t total = offset;
        for (auto capacity : capacities) total += capacity;
        std::vector<uint8_t> bytes(total, 0);
        Put(bytes, 0, 0x54444354); Put(bytes, 32, (uint32_t)names.size()); Put(bytes, 36, 64 + 8);
        for (size_t i = 0; i < names.size(); ++i)
        {
            const size_t record = 64 + i * 128;
            Put(bytes, record + 8, i + 1 == names.size() ? 36 : (uint32_t)(record + 128 + 8));
            Put(bytes, record + 12, i ? (uint32_t)(record - 128 + 8) : 36);
            std::copy(names[i].begin(), names[i].end(), bytes.begin() + record + 16);
            const auto info = mh2::ReadDdsInfo(payloads[i]);
            Put(bytes, record + 80, info.width); Put(bytes, record + 84, info.height);
            Put(bytes, record + 88, 32); Put(bytes, record + 92, info.width);
            Put(bytes, record + 100, 0); Put(bytes, record + 104, (uint32_t)offset); Put(bytes, record + 112, (uint32_t)capacities[i]);
            std::copy(payloads[i].begin(), payloads[i].end(), bytes.begin() + offset);
            offset += capacities[i];
        }
        return bytes;
    }
    void Originals(Fixture &fixture)
    {
        mh2::FontDatFile font;
        for (uint32_t id = 0; id < 3; ++id)
        {
            mh2::FontDatSection section;
            section.fontId = id; section.fontSize = id == 1 ? 30 : 31; section.heightScale = (float)section.fontSize / 512;
            section.chars.push_back({32, 7.0f / 512, 0, 0, 7.0f / 256, 42.0f / 256});
            section.chars.push_back({'A', 9.0f / 512, 8.0f / 256, 0, 16.0f / 256, 42.0f / 256});
            font.sections.push_back(section);
        }
        mh2::WriteFontDat(fixture.game / "global/initscripts/font.dat", font);
        mh2::WriteBytes(fixture.game / "global/pictures/global_pc.tex", TexFixture());
        mh2::GxtFile gxt;
        gxt.hasTdat = true;
        gxt.entries.push_back({0, "KEY", 0x12A7, u"Original text"});
        mh2::WriteGxt(fixture.game / "global/game.gxt", gxt);
        mh2::WriteText(fixture.game / "d3d9.dll", "unrelated loader marker");
        mh2::WriteText(fixture.game / "other-mod.txt", "unrelated modification");
    }
    void Inputs(Fixture &fixture, bool many = false)
    {
        std::u16string text = u"ABC café ~blue~bleu~colour~~nl~Next line";
        if (many)
        {
            for (char16_t c = 0x0410; c <= 0x044F; ++c) text += c;
            for (char16_t c = 0x03B1; c <= 0x03C9; ++c) text += c;
        }
        mh2::WriteText(fixture.root / "JSON/global/game.json", mh2::WriteJson(mh2::JsonValue::Array({mh2::JsonValue::Object({
            {"Key", mh2::JsonValue::String("KEY")}, {"Extra", mh2::JsonValue::String("0x000012A7")},
            {"en_US", mh2::JsonValue::String("Original text")}, {"fr_FR", mh2::JsonValue::String(mh2::Utf16ToUtf8(text))}}),
            mh2::JsonValue::Object({{"Key", mh2::JsonValue::String("INPRO")}, {"Extra", mh2::JsonValue::String("0x1234")},
                {"fr_FR", mh2::JsonValue::String("")}})})));
        wchar_t windowsDirectory[MAX_PATH]{};
        Check(GetWindowsDirectoryW(windowsDirectory, MAX_PATH) != 0, "Locate Windows font directory");
        const auto font = fs::path(windowsDirectory) / "Fonts/arial.ttf";
        Check(fs::is_regular_file(font), "Windows test font available; never redistributed");
        for (const auto &role : {"font1", "font2"})
        {
            const auto folder = fixture.root / "TTF" / role;
            fs::create_directories(folder);
            fs::copy_file(font, folder / fs::u8path(u8"测试字体.TtF"), fs::copy_options::overwrite_existing);
        }
    }
    std::map<std::string, std::vector<uint8_t>> GameResources(const Fixture &fixture)
    {
        std::map<std::string, std::vector<uint8_t>> files;
        for (const auto &path : mh2::ResourceFiles(fixture.game)) files[path.generic_u8string()] = mh2::ReadBytes(fixture.game / path);
        return files;
    }
    void ConfigTests(const fs::path &root)
    {
        Fixture fixture(root);
        auto config = fixture.Config();
        Check(config.language == "fr_FR" && config.font1.size == 31 && config.font2.size == 30, "Default role sizes and configured language");
        Check(config.font1.outlineWidth == 0 && config.font2.outlineWidth == 0 && !config.cacheGlyphs, "Default no outline/tiny glyph files");
        Check(config.gameDir == fixture.game && config.jsonDir == root / "JSON", "Paths are relative to project config");
        fixture.Set("font1", mh2::JsonValue::Object({{"size", mh2::JsonValue::Number(24)}}));
        Check(fixture.Config().font1.renderSize == 24, "Size controls rendering unless overridden");
        fixture.Set("workerThreads", mh2::JsonValue::Number(-1));
        Reject([&] { fixture.Config(); }, "workerThreads");
        fixture.Set("workerThreads", mh2::JsonValue::Number(0));
        fixture.Set("cacheGlyphs", mh2::JsonValue::String("true"));
        Reject([&] { fixture.Config(); }, "cacheGlyphs");
        fixture.Set("cacheGlyphs", mh2::JsonValue::Bool(false));
        fixture.Set("language", mh2::JsonValue::String("Key"));
        Reject([&] { fixture.Config(); }, "Reserved");
        fixture.Set("language", mh2::JsonValue::String("ja_JP"));
        Check(fixture.Config().language == "ja_JP", "No hardcoded Chinese target");
        fixture.Set("outputDirectory", mh2::JsonValue::String("JSON/dist"));
        Reject([&] { fixture.Config(); }, "overlap");
        fixture.Set("outputDirectory", mh2::JsonValue::String("dist"));
        fixture.Set("typo", mh2::JsonValue::Bool(true));
        Reject([&] { fixture.Config(); }, "unknown option");
        Reject([&] { mh2::SingleTtf(root / "missing"); }, "found 0");
        mh2::WriteText(root / "fonts/one.TTF", "fixture");
        Check(mh2::SingleTtf(root / "fonts").filename() == "one.TTF", "Mixed case TTF suffix");
        mh2::WriteText(root / "fonts/two.ttf", "fixture");
        Reject([&] { mh2::SingleTtf(root / "fonts"); }, "found 2");
        Reject([] { mh2::ValidateRelative("../escape.gxt"); }, "Unsafe");
        Reject([] { mh2::ValidateRelative("C:/escape.gxt"); }, "relative");
        Reject([] { mh2::ValidateRelative(fs::path(std::wstring(L"control\x01.gxt"))); }, "Unsafe");
        Check(mh2::Sha256({'a','b','c'}) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 known vector");
        {
            mh2::OperationLock lock(root / "locked-output");
            std::string failure;
            std::thread concurrent([&] {
                try { mh2::OperationLock other(root / "locked-output"); }
                catch (const std::exception &error) { failure = error.what(); }
            });
            concurrent.join();
            Check(failure.find("Another mh2loc operation") != std::string::npos, "Concurrent operations fail instead of sharing a destination");
        }
        mh2::OperationLock released(root / "locked-output");
    }
    void CodecTests(const fs::path &root)
    {
        fs::create_directories(root);
        const auto dds = SolidDds(8);
        uint32_t width = 0, height = 0;
        const auto pixels = mh2::DecodeDxt5DdsFirstMip(dds, width, height);
        Check(width == 8 && height == 8 && pixels[3] == 255, "DXT5 decode");
        auto large = std::vector<uint8_t>(512 * 512 * 4, 230);
        Check(mh2::EncodeDxt5Dds(large, 512, 512, 0, 1) == mh2::EncodeDxt5Dds(large, 512, 512, 0, 4), "Serial/parallel DXT5 equality");
        Reject([&] { mh2::DecodeDxt5DdsFirstMip({1,2,3}, width, height); }, "DDS");
        const auto texPath = root / "fixture.tex";
        auto raw = TexFixture();
        mh2::WriteBytes(texPath, raw);
        auto tex = mh2::ReadTex(texPath);
        Check(tex.records.size() == 5 && tex.records[3].name == tex.records[4].name, "TEX list and genuine duplicate names");
        const auto replaced = root / "record.tex";
        mh2::WriteTexReplacingDdsByRecordOffset(texPath, replaced, {{tex.records[3].recordOffset, SolidDds(8, 0)}});
        const auto result = mh2::ReadTex(replaced);
        const auto &other = tex.records[4];
        Check(std::equal(raw.begin() + other.dataOffset, raw.begin() + other.dataOffset + other.dataSize, result.decompressed.begin() + other.dataOffset), "Physical replacement does not change a same-name sibling");
        std::vector<uint8_t> z2(8 + compressBound((uLong)raw.size()));
        Put(z2, 0, 0x4D48325A); Put(z2, 4, (uint32_t)raw.size());
        uLongf compressedSize = (uLongf)(z2.size() - 8);
        Check(compress2(z2.data() + 8, &compressedSize, raw.data(), (uLong)raw.size(), 1) == Z_OK, "Fixture Z2HM compression");
        z2.resize(8 + compressedSize);
        mh2::WriteBytes(root / "wrapped.tex", z2);
        Check(mh2::ReadTex(root / "wrapped.tex").decompressed == raw, "Z2HM roundtrip");
        Put(z2, 4, 0xFFFFFFFF);
        mh2::WriteBytes(root / "bad.tex", z2);
        Reject([&] { mh2::ReadTex(root / "bad.tex"); }, "limit");
        Put(raw, 64 + 8, 64 + 8);
        mh2::WriteBytes(root / "cycle.tex", raw);
        Reject([&] { mh2::ReadTex(root / "cycle.tex"); }, "loops");
        Fixture fixture(root / "font"); Originals(fixture);
        const auto fontPath = fixture.game / "global/initscripts/font.dat";
        const auto fonts = mh2::ReadFontDat(fontPath);
        mh2::WriteFontDat(root / "copy.dat", fonts);
        Check(mh2::ReadBytes(root / "copy.dat") == mh2::ReadBytes(fontPath), "DAT count-minus-one roundtrip");
        auto badDat = mh2::ReadBytes(fontPath); Put(badDat, 6, 0xFFFFFFFF);
        mh2::WriteBytes(root / "bad.dat", badDat);
        Reject([&] { mh2::ReadFontDat(root / "bad.dat"); }, "counts");
        mh2::WriteText(root / "JSON/test.json", u8R"([{"Key":"KEY","Extra":"0x12A7","en_US":"Old","ja_JP":"かな~blue~青~colour~~nl~文","ko_KR":"한국어"},{"Key":"INPRO","ja_JP":""}])");
        const auto input = mh2::CompileTranslations(root / "JSON", "ja_JP");
        Check(input.documents[0].gxt.entries[0].key == "KEY" && input.documents[0].gxt.entries[0].extra == 0x12A7, "KEY and Extra preserved");
        Check(input.documents[0].gxt.entries[1].value.empty(), "Empty translations retained");
        Check(input.codepoints.count(0x304B) && input.codepoints.count(0x6587) && !input.codepoints.count(0x97D3), "Selected-language codepoints only");
        mh2::VerifyTranslationSources(input, root / "JSON");
        Reject([&] { mh2::CompileTranslations(root / "JSON", "de_DE"); }, "de_DE");
        mh2::WriteText(root / "JSON/test.json", "[\n{\"Key\":\"A\",\"fr_FR\":\"a\",}\n]");
        Reject([&] { mh2::VerifyTranslationSources(input, root / "JSON"); }, "changed");
        Reject([&] { mh2::CompileTranslations(root / "JSON", "fr_FR"); }, "test.json:2:");
        mh2::WriteText(root / "JSON/test.json", R"([{"Key":"A","Value":"legacy"}])");
        Reject([&] { mh2::CompileTranslations(root / "JSON", "fr_FR"); }, "fr_FR");
        mh2::WriteText(root / "JSON/test.json", R"([{"Key":"A","fr_FR":"\uD83D\uDE00"}])");
        Reject([&] { mh2::CompileTranslations(root / "JSON", "fr_FR"); }, "non-BMP");
    }
    void BuilderTests(const fs::path &root)
    {
        Fixture fixture(root); Originals(fixture); Inputs(fixture, true);
        auto config = fixture.Config();
        const auto originals = GameResources(fixture);
        mh2::CaptureOriginals(config);
        Check(mh2::BuildProject(config), "Initial build");
        Check(GameResources(fixture) == originals, "Build never writes the game");
        Check(!fs::exists(config.cacheDir), "No per-glyph cache files by default");
        const auto datPath = config.outputDir / "global/initscripts/font.dat";
        const auto texPath = config.outputDir / "global/pictures/global_pc.tex";
        const auto dat = mh2::ReadFontDat(datPath);
        Check(dat.sections.size() == 3 && dat.sections[1].fontSize == 30 && dat.sections[2].fontSize == 31, "Crossed role/section mapping preserved");
        Check(dat.sections[0].chars.size() == 2, "Untouched base section");
        for (size_t index : {size_t(1), size_t(2)})
        {
            for (const auto &glyph : dat.sections[index].chars)
            {
                Check(glyph.code < dat.sections[index].fmap.size() && dat.sections[index].fmap[glyph.code] >= 0, "Generated codepoint mapping");
                Check(glyph.x1 >= 0 && glyph.y1 >= 0 && glyph.x2 <= 1 && glyph.y2 <= 1 && glyph.x2 > glyph.x1 && glyph.y2 > glyph.y1, "Generated glyph UV bounds");
            }
            const auto inspection = mh2::InspectFontGlyph(datPath, texPath, dat.sections[index].fontId, u'A');
            Check(!inspection.missing && inspection.inkMaxY >= inspection.inkMinY, "DAT UV finds nonempty A in the matching TEX");
            Check(inspection.topRowMaxAlpha == 0, "Transparent top safety margin retained");
        }
        const auto beforeTex = mh2::ReadTex(config.originalDir / "global/pictures/global_pc.tex"), afterTex = mh2::ReadTex(texPath);
        Check(beforeTex.records.size() == afterTex.records.size(), "TEX records not added/reordered");
        for (size_t i = 0; i < beforeTex.records.size(); ++i)
        {
            const auto &before = beforeTex.records[i], &after = afterTex.records[i];
            Check(before.recordOffset == after.recordOffset && before.dataOffset == after.dataOffset && before.dataSize == after.dataSize, "Fixed TEX slot layout unchanged");
            if (before.name != "font1" && before.name != "font2")
                Check(std::equal(beforeTex.decompressed.begin() + before.dataOffset, beforeTex.decompressed.begin() + before.dataOffset + before.dataSize,
                    afterTex.decompressed.begin() + after.dataOffset), "Non-font textures unchanged");
            else
            {
                const std::vector<uint8_t> payload(afterTex.decompressed.begin() + after.dataOffset, afterTex.decompressed.begin() + after.dataOffset + after.dataSize);
                const auto info = mh2::ReadDdsInfo(payload);
                Check(info.mipCount == 0 && info.width == info.height && (info.width & (info.width - 1)) == 0, "One physical mip and power-of-two square atlas");
            }
        }
        const auto gxt = mh2::ReadGxt(config.outputDir / "global/game.gxt");
        Check(gxt.entries[0].value.find(u"~blue~bleu~colour~~nl~") != std::u16string::npos && gxt.entries[0].value.find(u' ') != std::u16string::npos,
            "Color/newline tags and normal spaces remain exactly in GXT (no MH1 escaping)");
        Check(!mh2::BuildProject(config), "Output cache hit");
        const auto serialDat = mh2::ReadBytes(datPath), serialTex = mh2::ReadBytes(texPath);
        fixture.Set("workerThreads", mh2::JsonValue::Number(4)); config = fixture.Config();
        Check(mh2::BuildProject(config) && mh2::ReadBytes(datPath) == serialDat && mh2::ReadBytes(texPath) == serialTex, "Serial/parallel generator byte equality");
        fs::remove(texPath);
        Check(mh2::BuildProject(config) && fs::is_regular_file(texPath), "Deleting one output rebuilds without deleting other files");
        fs::remove(config.outputDir / "build.manifest.json");
        Check(mh2::BuildProject(config), "A deleted output-cache manifest can be recreated without deleting known outputs");
        mh2::WriteText(config.jsonDir / "bad.json", "not json");
        Reject([&] { mh2::BuildProject(config); }, "bad.json");
        Check(mh2::ReadBytes(datPath) == serialDat, "Bad JSON keeps last complete output");
        fs::remove(config.jsonDir / "bad.json");
        mh2::WriteBytes(root / "too-small.tex", TexFixture(true));
        fixture.Set("texTemplate", mh2::JsonValue::String("too-small.tex")); config = fixture.Config();
        Reject([&] { mh2::BuildProject(config); }, "slot");
        Check(mh2::ReadBytes(datPath) == serialDat && mh2::ReadBytes(texPath) == serialTex, "Capacity failure keeps complete old package");
        mh2::WriteBytes(root / "template.tex", TexFixture(false, true));
        fixture.Set("texTemplate", mh2::JsonValue::String("template.tex")); config = fixture.Config();
        Check(mh2::BuildProject(config), "Explicit capacity template");
        const auto withTemplate = mh2::ReadTex(texPath);
        const auto &title = withTemplate.records[2];
        Check(std::equal(beforeTex.decompressed.begin() + title.dataOffset, beforeTex.decompressed.begin() + title.dataOffset + title.dataSize,
            withTemplate.decompressed.begin() + title.dataOffset), "Template advertisement never leaks into output");
        fixture.Set("glyphSource", mh2::JsonValue::String("original-first")); config = fixture.Config();
        Check(mh2::BuildProject(config), "Original-first strategy remains available");
        const auto originalFirst = mh2::ReadFontDat(datPath);
        const auto &section = originalFirst.sections[2];
        Check(section.chars[(size_t)section.fmap['A']].widthScale == 9.0f / 512, "Original-first preserves original A advance");
        mh2::RenderFontPreview(datPath, texPath, 2, u"ABC café 123", root / "preview.bmp", 2);
        Check(mh2::ReadBytes(root / "preview.bmp").size() > 54, "Offline preview produced");

        fixture.Set("glyphSource", mh2::JsonValue::String("ttf-first"));
        fixture.Set("font1", mh2::JsonValue::Object({{"size", mh2::JsonValue::Number(31)},
            {"outlineWidth", mh2::JsonValue::Number(1)}, {"alphaGain", mh2::JsonValue::Number(1.6)}}));
        fixture.Set("font2", mh2::JsonValue::Object({{"size", mh2::JsonValue::Number(30)},
            {"renderSize", mh2::JsonValue::Number(31)}, {"alphaGain", mh2::JsonValue::Number(2.2)}}));
        config = fixture.Config();
        mh2::BuildProject(config);
        const auto sharedDat = mh2::ReadBytes(datPath), sharedTex = mh2::ReadBytes(texPath);
        const auto font1Path = mh2::SingleTtf(config.fontDir / "font1");
        auto fontBytes = mh2::ReadBytes(font1Path);
        fontBytes.push_back(0); // Same TTF outlines, different SHA-256: bypass cross-role raw-glyph reuse.
        mh2::WriteBytes(font1Path, fontBytes);
        mh2::BuildProject(config);
        Check(mh2::ReadBytes(datPath) == sharedDat && mh2::ReadBytes(texPath) == sharedTex,
            "Same-font/size reuse never carries alpha or outline styles into another role");
        fixture.Set("cacheGlyphs", mh2::JsonValue::Bool(true)); config = fixture.Config();
        mh2::BuildProject(config);
        Check(fs::exists(config.cacheDir) && mh2::ReadBytes(texPath) == sharedTex, "Opt-in disk glyph cache preserves output");
        Check(mh2::BuildProject(config, true) && mh2::ReadBytes(datPath) == sharedDat && mh2::ReadBytes(texPath) == sharedTex,
            "Warm glyph cache preserves complete generated bytes");
    }
    void DeploymentTests(const fs::path &root)
    {
        Fixture fixture(root); Originals(fixture); Inputs(fixture);
        auto config = fixture.Config();
        const auto pristine = GameResources(fixture);
        mh2::CaptureOriginals(config);
        const auto snapshot = mh2::ReadBytes(config.originalDir / "original.manifest.json");
        Check(mh2::ReadBytes(config.originalDir / "global/game.gxt") == pristine.at("global/game.gxt"), "Capture includes GXT when the configured game is a child of the project root");
        mh2::WriteText(config.jsonDir / "levels/new-mod.json", R"([{"Key":"NEW","Extra":"0x1234","fr_FR":"New file"}])");
        mh2::BuildProject(config);
        const auto locked = CreateFileW((fixture.game / "global/initscripts/font.dat").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(locked != INVALID_HANDLE_VALUE, "Lock fixture font against deployment");
        bool refused = false;
        try { mh2::DeployProject(config); } catch (const std::exception &) { refused = true; }
        CloseHandle(locked);
        Check(refused && GameResources(fixture) == pristine, "Failed deployment rolls back all previously replaced files");
        mh2::DeployProject(config);
        Check(fs::is_regular_file(fixture.game / "levels/new-mod.gxt"), "New mod resource deployed");
        Check(mh2::ReadText(fixture.game / "other-mod.txt") == "unrelated modification" && mh2::ReadText(fixture.game / "d3d9.dll") == "unrelated loader marker", "Unrelated mod/loader untouched");
        mh2::CaptureOriginals(config);
        Check(mh2::ReadBytes(config.originalDir / "original.manifest.json") == snapshot, "Repeated capture never replaces originals with localized assets");
        const auto deployed = mh2::ReadBytes(fixture.game / "global/game.gxt");
        mh2::WriteText(fixture.game / "global/game.gxt", "external modification");
        Reject([&] { mh2::RestoreProject(config); }, "modified");
        Check(fs::exists(fixture.game / ".mh2loc-deployment.json"), "Modified deployed file keeps recovery journal");
        mh2::WriteBytes(fixture.game / "global/game.gxt", deployed);
        fs::remove(config.jsonDir / "levels/new-mod.json");
        mh2::BuildProject(config);
        mh2::DeployProject(config);
        Check(!fs::exists(fixture.game / "levels/new-mod.gxt"), "Upgrade removes only a previously created stale mod file");
        mh2::RestoreProject(config);
        Check(GameResources(fixture) == pristine, "Restore exactly reproduces original game fixture");
        mh2::RestoreProject(config);
        Check(GameResources(fixture) == pristine, "Repeated restore is harmless");
        mh2::WriteText(config.outputDir / "private-note.txt", "do not remove");
        Reject([&] { mh2::BuildProject(config, true); }, "Unmanaged");
        Check(mh2::ReadText(config.outputDir / "private-note.txt") == "do not remove", "Unknown files are not discarded");
        fs::remove(config.outputDir / "private-note.txt");
        mh2::WriteText(fixture.game / "global/game.gxt", "another patch");
        Reject([&] { mh2::DeployProject(config); }, "another modification");
        mh2::WriteText(config.originalDir / "global/game.gxt", "tampered original");
        Reject([&] { mh2::BuildProject(config); }, "modified");
    }
}

int main(int argc, char **argv)
{
    fs::path root;
    try
    {
        if (argc != 3) throw std::runtime_error("Usage: tests <suite> <test-root>");
        const std::string suite = argv[1];
        root = fs::u8path(argv[2]) / fs::u8path(suite + "-" + std::to_string(GetCurrentProcessId())) / fs::u8path(u8"路径 空格 !");
        Check(!fs::exists(root), "Test path must be new");
        if (suite == "config") ConfigTests(root);
        else if (suite == "codecs") CodecTests(root);
        else if (suite == "builder") BuilderTests(root);
        else if (suite == "deployment") DeploymentTests(root);
        else throw std::runtime_error("Unknown suite");
        std::cout << assertions << " assertions passed; artifacts: " << root.u8string() << '\n';
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << "\nArtifacts: " << root.u8string() << '\n';
        return 1;
    }
}
