#include "package.hpp"
#include "file_io.hpp"
#include "mh2_font_dat.hpp"
#include "mh2_font_preview.hpp"
#include "mh2_tex.hpp"
#include "translations.hpp"
#include "unicode.hpp"
#include <windows.h>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace
{
    fs::path ExecutableDirectory()
    {
        std::wstring name(512, L'\0');
        for (;;)
        {
            const auto count = GetModuleFileNameW(nullptr, name.data(), (DWORD)name.size());
            if (!count) throw std::runtime_error("Cannot locate executable directory");
            if (count < name.size()) { name.resize(count); return fs::path(name).parent_path(); }
            name.resize(name.size() * 2);
        }
    }
    void Usage()
    {
        std::cout << "mh2loc " << MH2LOC_VERSION << " - Manhunt 2 JSON/TTF localization generator\n\n"
            "Commands (project paths are relative to project.json, not the current directory):\n"
            "  capture-originals [--project project.json]  Capture a CLEAN installed game once\n"
            "  seal-originals    [--project project.json]  Seal user-supplied clean resources\n"
            "  validate          [--project project.json]  Validate the selected JSON language\n"
            "  build             [--project project.json] [--force]\n"
            "  install           [--project project.json] [--force]  Build then deploy\n"
            "  deploy            [--project project.json]  Deploy verified prebuilt resources\n"
            "  restore           [--project project.json]  Restore this tool's deployment only\n"
            "  inspect-fontdat <font.dat>\n"
            "  inspect-tex <global_pc.tex>\n"
            "  preview <font1|font2> <UTF8-text.txt> <output.bmp> [--project project.json]\n"
            "\nDefault config: project.json next to this EXE. No game assets or fonts are bundled.\n";
    }
}

int wmain(int argc, wchar_t **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    try
    {
        const std::wstring command = argc > 1 ? argv[1] : L"--help";
        if (command == L"--help" || command == L"-h") { Usage(); return 0; }
        if (command == L"--version") { std::cout << MH2LOC_VERSION << '\n'; return 0; }
        if (command == L"inspect-fontdat")
        {
            if (argc != 3) throw std::runtime_error("Usage: mh2loc inspect-fontdat <font.dat>");
            const auto fonts = mh2::ReadFontDat(argv[2]);
            for (size_t i = 0; i < fonts.sections.size(); ++i)
            {
                const auto &font = fonts.sections[i];
                std::cout << "section=" << i << " id=" << font.fontId << " size=" << font.fontSize << " heightScale=" << font.heightScale
                    << " chars=" << font.chars.size() << " map=" << font.fmap.size() << '\n';
            }
            return 0;
        }
        if (command == L"inspect-tex")
        {
            if (argc != 3) throw std::runtime_error("Usage: mh2loc inspect-tex <file.tex>");
            const auto tex = mh2::ReadTex(argv[2]);
            for (const auto &record : tex.records)
                std::cout << record.name << " record=" << record.recordOffset << " slot=" << record.dataSize << " bytes\n";
            return 0;
        }
        if (command != L"capture-originals" && command != L"seal-originals" && command != L"validate" &&
            command != L"build" && command != L"install" && command != L"deploy" && command != L"restore" && command != L"preview")
            throw std::runtime_error("Unknown command. Run mh2loc --help");
        fs::path projectPath = ExecutableDirectory() / "project.json";
        bool force = false;
        int start = command == L"preview" ? 5 : 2;
        if (command == L"preview" && argc < 5) throw std::runtime_error("Usage: mh2loc preview <font1|font2> <UTF8-text.txt> <output.bmp> [--project project.json]");
        for (int i = start; i < argc; ++i)
        {
            const std::wstring option = argv[i];
            if (option == L"--project" && i + 1 < argc) projectPath = argv[++i];
            else if (option == L"--force" && (command == L"build" || command == L"install")) force = true;
            else throw std::runtime_error("Unknown/missing option: " + mh2::Utf16ToUtf8(std::u16string(option.begin(), option.end())));
        }
        const auto config = mh2::ReadProject(projectPath);
        if (command == L"capture-originals") mh2::CaptureOriginals(config);
        else if (command == L"seal-originals") mh2::SealOriginals(config);
        else if (command == L"validate")
        {
            const auto input = mh2::CompileTranslations(config.jsonDir, config.language);
            std::cout << "Valid: " << input.documents.size() << " JSON files, " << input.codepoints.size()
                << " display codepoints including basic ASCII, language " << config.language << '\n';
        }
        else if (command == L"build") mh2::BuildProject(config, force);
        else if (command == L"install") { mh2::BuildProject(config, force); mh2::DeployProject(config); }
        else if (command == L"deploy") mh2::DeployProject(config);
        else if (command == L"restore") mh2::RestoreProject(config);
        else if (command == L"preview")
        {
            const std::wstring role = argv[2];
            if (role != L"font1" && role != L"font2") throw std::runtime_error("Preview role must be font1 or font2");
            const fs::path output = argv[4];
            if (output.extension() != ".bmp") throw std::runtime_error("Preview output must use the .bmp extension");
            const auto dat = config.outputDir / "global/initscripts/font.dat";
            const auto fonts = mh2::ReadFontDat(dat);
            const size_t index = role == L"font1" ? 2 : 1;
            if (fonts.sections.size() <= index) throw std::runtime_error("Unsupported font section layout");
            mh2::RenderFontPreview(dat, config.outputDir / "global/pictures/global_pc.tex", fonts.sections[index].fontId,
                mh2::Utf8ToUtf16(mh2::ReadText(argv[3])), output, 2, role == L"font1" ? "font1" : "font2");
            std::cout << "Preview: " << output.u8string() << '\n';
        }
        else throw std::runtime_error("Unknown command. Run mh2loc --help");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
