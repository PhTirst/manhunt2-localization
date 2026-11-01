# Manhunt 2 Localization Generator

[简体中文](README.zh-CN.md) · [Binary formats](docs/FORMAT.md) · [Deployment safety](docs/SAFETY.md) · [Verification](docs/VERIFICATION.md)

[Download the Windows x64 tool](https://github.com/PhTirst/manhunt2-localization/releases/latest). Download `mh2loc-<version>-windows-x64.zip` under Assets, not the source-code ZIP, to use the precompiled tool.

**Windows x64, C++17.** Build Manhunt 2 PC translation resources from multilingual JSON and TTF fonts: GXT, `font.dat`, and the font textures inside `global_pc.tex`. Deploy a verified build and restore the captured originals through simple BAT entry points.

This is the **full offline resource generator**, not the already published [GXT/JSON text converter](https://github.com/PhTirst/manhunt2-gxt-json), Manhunt 1 MHLLJsonDev, or a TEX GUI editor. It does not load JSON while the game runs.

## Before You Start

- Supply your own clean installed game resources, translations and licensed fonts. None are included in the source or tool ZIP.
- A large character set may exceed the stock TEX font slots. In that case you must supply a compatible expanded-capacity TEX through `texTemplate`. The tool does not download a patch or silently repack an unknown TEX layout. See [format notes](docs/FORMAT.md).
- A compatible runtime Unicode/subtitle ASI may be required by your game/text setup. This project does not ship or install ASI/loader DLLs. Generating a Chinese font does not by itself implement Unicode movie subtitles, color controls or line breaking in the game renderer.
- Close the game before install/restore. Keep the original snapshot and deployment journal. See [safety boundaries](docs/SAFETY.md).

## Download Layout and Quick Workflow

Use the `mh2loc-<version>-windows-x64.zip` tool package, not the source archive. Extract it into a dedicated subfolder under the installed game, for example:

```text
Manhunt 2/
  Manhunt2.exe
  mh2loc/
    mh2loc.exe
    project.json
    00_capture_originals.bat
    01_build.bat
    02_install.bat
    03_restore.bat
    04_validate_json.bat
    JSON/                  <your translations>
    TTF/font1/             <one .ttf>
    TTF/font2/             <one .ttf>
    original/              <captured or supplied clean resources>
    dist/                  <generated final resources>
```

1. Confirm the installation is clean. `project.json` defaults `gameDirectory` to `..`, the parent game directory. For a tool folder elsewhere, set the actual installed game path explicitly.
2. Run `00_capture_originals.bat` once. It captures the original font DAT/TEX and game GXT files, then seals their SHA-256 hashes. It never replaces a sealed snapshot with a later localized installation.
3. Place translations under `JSON/`, preserving game-relative paths. Example: `JSON/levels/my-mod/dialogue.json` generates `dist/levels/my-mod/dialogue.gxt`. Use the separate GXT/JSON converter to create editable source templates if needed.
4. Put exactly one TTF in each of `TTF/font1/` and `TTF/font2/`. Keep the original font filenames; using the same TTF in both roles is allowed.
5. Set `language` in `project.json` to the column you want, such as `zh_CN`, `ja_JP`, `ko_KR`, `fr_FR` or `ru_RU`. Run `04_validate_json.bat` to validate text without rendering or writing to the game.
6. Run `01_build.bat` to generate resources only, or `02_install.bat` to build and then deploy. Errors leave the previous complete build unchanged. A capacity error means you must explicitly provide a compatible larger TEX template.
7. Run `03_restore.bat` to undo this tool's deployment. It restores original files and removes only the extra resource files this tool created; unrelated MODs, loaders and saves are not copied/deleted.

All relative configuration paths are resolved against **the project file**, not the launch directory. BAT files always use the adjacent `project.json` and preserve failure exit codes. Set `MH2LOC_NO_PAUSE=1` for automation. The EXE statically links the C++ runtime and zlib-ng; users need no .NET, Python, Visual Studio or CMake to run a built package.

## JSON and Languages

UTF-8 JSON uses the same Manhunt 2 object-array format as the text converter:

```json
[
  {
    "Key": "LINE0",
    "Extra": "0x000012A7",
    "en_US": "Hello!",
    "fr_FR": "Bonjour !",
    "zh_CN": "你好！"
  }
]
```

Only Key, Extra and the configured language are compiled. Other columns are ignored, not rewritten. A missing target column is an error, not silent fallback to English/Value/Text. Empty strings are permitted. Keys and Extra are preserved and validated; the real key `KEY` is not a header. Do not translate or normalize `~...~` control tokens. The generator does not clean punctuation, rewrite ellipses, add artificial spaces, or apply Manhunt 1's U+2007 workaround.

The character set comes from selected JSON values plus basic ASCII for untranslated/dynamic game values. Han, kana, Hangul, Cyrillic and other BMP text can be generated if the chosen sources contain the glyphs. Missing glyphs produce a codepoint diagnostic. Non-BMP display glyphs, complex shaping and bidirectional layout are outside the supported renderer scope. A font generator does not replace runtime layout support.

## Configuration

The tool ZIP includes `project.json` at its root (`distribution/project.json` in the source checkout). It is the full configuration example. Paths are directory conventions, not per-file mappings.

| Setting | Default / Meaning |
| --- | --- |
| `language` | `zh_CN`; any valid target-language column |
| `gameDirectory` | `..`; the **installed** game, not an installer/extracted setup payload |
| `originalDirectory` | `original`; immutable captured/supplied baseline |
| `jsonDirectory` | `JSON`; recursively scanned translation source tree |
| `fontDirectory` | `TTF`; contains font1 and font2 role directories |
| `outputDirectory` | `dist`; dedicated generated package, not a source/game root |
| `cacheDirectory` | `.cache`; only used for optional raw glyph disk cache |
| `texTemplate` | `null` uses original TEX; otherwise explicit compatible capacity-template file |
| `fallbackFont` | `null`, or a directory containing one fallback TTF |
| `asciiFont` | `null`, or a directory containing one independent ASCII-override TTF |
| `glyphSource` | `ttf-first`, or `original-first` |
| `font1`, `font2` | Object with `size`, `renderSize`, `outlineWidth`, `alphaGain` |
| `workerThreads` | `0` auto-selects by hardware/useful work; positive value is an explicit limit |
| `cacheGlyphs` | `false`; no per-character disk cache files by default |

Default font1 settings: game/render size 31px, outline 0px, alpha gain 2.6. Default font2: 30px, outline 0px, alpha gain 2.2. Change the JSON settings instead of editing source. `size` controls the game line scale; `renderSize` controls TTF rasterization. The original top safety margin is always retained. Both roles use variable advances; outlines and alpha gain do not change advances.

`ttf-first` prefers the main TTF, then fallback TTF, then original glyphs. `original-first` preserves original glyphs and renders only missing characters. ASCII override, when enabled, is a separate layer for used ASCII characters. Same TTF **and render size** can reuse raw glyph work within a build; layout, outline and alpha remain role-specific. Different TTFs/sizes are not incorrectly merged.

Each role/fallback/ASCII directory must contain exactly one regular `.ttf` file, case-insensitively; discovery is not recursive and multiple candidates are errors. There is no automatic font download or fallback directory guessing. Unknown JSON settings and wrong types are rejected.

## Build Outputs, Cache and Recovery

```text
dist/
  build.manifest.json
  global/initscripts/font.dat
  global/pictures/global_pc.tex
  <same relative paths as JSON>/*.gxt
```

The build manifest records input and output SHA-256 hashes. An unchanged input with verified complete outputs skips rendering. Changing JSON/TTF/template/options, editing a generated file, or deleting a generated font/TEX/GXT triggers a complete rebuild. In-memory glyph reuse avoids redundant work in the same run; optional `cacheGlyphs: true` adds a raw glyph cache under `.cache` for later runs. Those are rebuildable cache entries, not authoritative translation/font sources.

Intermediate glyph/atlas processing stays in memory by default; final resources are first staged, then the dedicated dist directory is replaced. A bad JSON/font, missing glyph or TEX capacity failure does not destroy prior dist. Unknown files placed in dist cause a refusal, not broad cleanup. Game deployment is a separate journaled transaction with SHA-256 conflict checks and rollback on ordinary file failures. These operations are not power-loss-safe database transactions.

Do not delete `original/original.manifest.json` or the game-side `.mh2loc-deployment.json` to force a rebuild. The former protects your baseline; the latter is needed to safely restore it. Use `mh2loc build --force` for regeneration.

## CLI and Diagnostics

```powershell
.\mh2loc.exe --help
.\mh2loc.exe validate --project .\project.json
.\mh2loc.exe build --project .\project.json --force
.\mh2loc.exe install --project .\project.json
.\mh2loc.exe deploy --project .\project.json
.\mh2loc.exe restore --project .\project.json
.\mh2loc.exe seal-originals --project .\project.json
.\mh2loc.exe inspect-fontdat .\original\global\initscripts\font.dat
.\mh2loc.exe inspect-tex .\original\global\pictures\global_pc.tex
.\mh2loc.exe preview font1 .\test-text.txt .\preview.bmp --project .\project.json
```

`seal-originals` is for user-supplied clean resources; it does not certify their provenance. Never seal the previous localization as "original". Preview reads generated DAT/TEX and is a raw glyph check, not a runtime color/wrapping/movie-subtitle simulator. JSON syntax diagnostics include the filename, line, column and nearby text; schema diagnostics identify the row/key/language.

## Developer Build, Tests and Tool ZIP

Use the local Visual Studio 2022 LLVM/clang-cl toolchain, Windows SDK and CMake 3.24+:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -T ClangCL
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cpack --config build/CPackConfig.cmake -C Release -B build/package
```

Use a separate build directory without `-T ClangCL` to build with MSVC. CMake fetches only a hash-pinned zlib-ng source release. For offline builds provide its unpacked source via `-DFETCHCONTENT_SOURCE_DIR_ZLIBNG=<path>`. No translation/font/game assets are downloaded. Packaging produces a resource-free ZIP and SHA-256 checksum. Do not send just the EXE without configuration, scripts and docs.

Tests synthesize DAT/TEX/GXT fixtures, copy an installed Windows font into temporary test folders and check codec structure, role mapping/UVs, top padding, both source strategies, serial/parallel determinism, non-font isolation, caching, errors, journaled deployment/restoration and BAT exit codes. They do not launch or deploy an actual game. Windows CI runs Debug and Release, then packages the tool; it does not automatically publish a GitHub Release.

See [NOTICE.md](NOTICE.md) for code/dependency provenance and asset boundaries. Tool code is under the included MIT license; that license does not grant rights to fonts, game resources or third-party patches.
