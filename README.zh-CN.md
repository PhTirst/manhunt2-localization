# Manhunt 2 本地化资源生成器

[English](README.md) · [格式与字体几何](docs/FORMAT.md) · [安装、恢复安全规则](docs/SAFETY.md) · [验证记录](docs/VERIFICATION.md)

[下载 Windows x64 工具包](https://github.com/PhTirst/manhunt2-localization/releases/latest)：请选择 Assets 中的 `mh2loc-<版本>-windows-x64.zip`，不是 Source code 源码压缩包。

**Windows x64 / C++17。**读取多语言 JSON 和 TTF，生成二代的 GXT、`font.dat` 及 `global_pc.tex` 中的字体贴图，并通过 BAT 部署、撤回。

这是二代的**完整离线资源生成器**，不是已经发布的 [GXT/JSON 文本转换器](https://github.com/PhTirst/manhunt2-gxt-json)，不是一代 MHLLJsonDev，也不包含 TEX 图形编辑器。它不会在游戏运行时加载 JSON。

## 使用前必须知道

- 游戏原版资源、译文、字体都由用户自行提供，源码和工具 ZIP 不附带这些内容。
- 原版 TEX 的字体槽位可能装不下大字库。遇到容量不足，必须通过 `texTemplate` 明确提供**兼容且容量足够的 TEX 模板**。程序不下载成熟汉化包，不偷偷到父目录找旧文件，也不重排未经验证的 TEX 结构。
- 游戏可能仍需要兼容的 Unicode/字幕 ASI。这个项目不附带、不安装 ASI 或加载器 DLL；生成中文字形本身不等于实现电影字幕、颜色和换行的运行时绘制。
- 安装、恢复前退出游戏。原版快照和游戏目录内的部署清单不能随意删除，具体边界见安全说明。

## 放在哪里，怎样使用

使用 `mh2loc-<版本>-windows-x64.zip` 工具包，不是源码压缩包。完整解压到已安装游戏的独立子目录：

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
    JSON/                  <自行提供译文>
    TTF/font1/             <唯一一个 TTF>
    TTF/font2/             <唯一一个 TTF>
    original/              <首次保存或自行提供的干净原版>
    dist/                  <生成的最终资源>
```

1. 确认游戏资源是干净原版。`project.json` 的 `gameDirectory` 默认是 `..`，指向父目录的已安装游戏；工具放在其它位置时，明确填写实际安装路径。不要指向安装包或 setup 解包目录。
2. 首次双击 `00_capture_originals.bat`，保存原版字体 DAT/TEX 和 GXT，并写入 SHA-256 清单。以后重复运行只核验，不把上一版汉化覆盖成“原版”。
3. 把 JSON 译文放进 `JSON/`，保留游戏相对路径。例如 `JSON/levels/my-mod/dialogue.json` 会生成 `dist/levels/my-mod/dialogue.gxt`。需要从原 GXT 提取双语模板时，使用单独发布的 GXT/JSON 工具。
4. 在 `TTF/font1/`、`TTF/font2/` 各放一个 TTF。保留原字体文件名即可；两个角色可以用同一个字体。
5. 配置 `language` 为目标列，如 `zh_CN`、`ja_JP`、`ko_KR`、`fr_FR`。`04_validate_json.bat` 只检查译文，不渲染、不写游戏。
6. `01_build.bat` 只生成资源；`02_install.bat` 先构建再部署。构建失败会保留上一套完整产物。槽位容量不足时，需自行提供兼容的大容量 TEX 并填写 `texTemplate`。
7. `03_restore.bat` 撤回本工具的部署：原来存在的文件恢复原版，工具新增的资源文件才会被删除；其它 MOD、加载器、存档不动。

相对路径始终基于 **project.json 所在目录**，不依赖当前终端目录。BAT 使用旁边的配置并正确返回错误码；自动化时可设置 `MH2LOC_NO_PAUSE=1` 取消按键暂停。生成后的 EXE 静态链接 C++ 运行库和 zlib-ng，使用者不需要安装 .NET、Python、VS 或 CMake。

## JSON 与语言列

沿用二代文本工具的 UTF-8 对象数组格式：

```json
[
  {
    "Key": "LINE0",
    "Extra": "0x000012A7",
    "en_US": "Hello!",
    "zh_CN": "你好！",
    "ja_JP": "こんにちは！"
  }
]
```

只编译 Key、Extra 和选中的语言列。其它语言列不改写；缺目标列会报错，不偷偷回退英文、Value 或 Text。允许空译文。Key、Extra 需要保留；真正叫 `KEY` 的条目不会被当表头删掉。

`~...~` 控制标记原样保留，不翻译、不自动修正。生成器不清洗标点、不把省略号改句号、不塞人工空格，也**不套用一代的 U+2007/零宽空格方案**。

用字由所选语言的 JSON 正文统计，另加入基本 ASCII 供英文、数字和动态值使用。字体源覆盖时可生成汉字、假名、韩文、西里尔字母等 BMP 字符；缺字会报告码点，不静默塞方框。非 BMP、复杂塑形及双向排版不在支持范围，字体工具也不替代游戏的运行时排版。

## 配置与字体策略

工具包根目录的 `project.json`（源码中是 `distribution/project.json`）使用目录约定，不逐个映射文本文件。

| 字段 | 默认值与用途 |
| --- | --- |
| `language` | `zh_CN`，可换成其它目标语言列 |
| `gameDirectory` | `..`，真正安装后的游戏目录 |
| `originalDirectory` | `original`，不再覆盖的原版快照 |
| `jsonDirectory` | `JSON`，递归扫描译文 |
| `fontDirectory` | `TTF`，下有 font1、font2 |
| `outputDirectory` | `dist`，专用生成目录，不能是源目录或游戏根目录 |
| `cacheDirectory` | `.cache`，仅用于可选的原始 glyph 磁盘缓存 |
| `texTemplate` | `null` 使用原版 TEX；或显式指定兼容容量模板文件 |
| `fallbackFont` | `null`；或含唯一 TTF 的后备字体目录 |
| `asciiFont` | `null`；或含唯一 TTF 的 ASCII 独立覆盖目录 |
| `glyphSource` | 默认 `ttf-first`；可改 `original-first` |
| `font1`、`font2` | 各有 size、renderSize、outlineWidth、alphaGain |
| `workerThreads` | `0` 按硬件及实际工作量选；正数为显式上限 |
| `cacheGlyphs` | `false`，默认不生成每个字符一个的缓存小文件 |

默认 font1：31px 游戏字号、31px TTF 渲染、无描边、alpha 增益 2.6；font2：30px、30px、无描边、alpha 增益 2.2。改配置即可调大小，不需要改源码。`size` 是游戏行框缩放，`renderSize` 是 TTF 光栅化大小，不是一个参数。两种策略都保留原版顶部安全留白，字宽为非固定方案。描边和透明度增益不改变 advance。

`ttf-first` 优先主 TTF，缺字再用后备 TTF、原版 glyph；`original-first` 保留原版已有字形，只用 TTF 补缺字。ASCII 覆盖层另行指定时，只处理使用到的英文、数字、英文标点等。**同一 TTF 且同一渲染字号**可以在内存复用原始 glyph，角色各自的留白、alpha、描边仍分别应用；不同字体、不同字号不会误合并。

每个角色及启用的可选字体目录都只允许一个普通 `.ttf`，扩展名不分大小写、不递归。多文件会列出候选并报错，不选第一个。不会下载字体、猜字体名或自动搜索后备目录。配置字段拼错、类型不对也会报错。

## 构建结果与缓存

```text
dist/
  build.manifest.json
  global/initscripts/font.dat
  global/pictures/global_pc.tex
  <与 JSON 相同相对路径>/*.gxt
```

清单保存输入指纹和最终文件 SHA-256。输入未变且输出完整、校验一致时跳过渲染。改 JSON、TTF、模板、字体策略，或改动/删除生成的 DAT、TEX、GXT，都会重新构建。默认在内存复用同一次构建的原始 glyph；需要跨次增量 glyph 缓存时再开启 `cacheGlyphs`。

默认不落盘单字 PNG、atlas 工作区等中间格式。最终资源先写暂存目录，全部成功后替换专用 dist；JSON、TTF、缺字或容量错误不破坏旧产物。dist 里放了未登记的私人文件时会拒绝清理，不会顺手删掉。

不要为了强制重建而删 `original/original.manifest.json` 或游戏中的 `.mh2loc-deployment.json`：前者保护原版，后者用于安全恢复。使用 `mh2loc build --force`。目录替换和文件部署会处理普通失败回滚，但不是能抵御突然断电的数据库事务，异常中断后的恢复文件要先检查再处理。

## 命令行和排错

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

自行复制原版资源时可用 `seal-originals` 建立清单，但程序不能替你证明它们真的是原版，更不能把上一版汉化当原版封存。离线 preview 用生成后的 DAT/TEX 检查 glyph，不模拟游戏颜色、自动换行或电影字幕。JSON 语法报错有文件、行列和附近文字；条目错误会指出 Key、行或目标语言。

## 开发构建、测试与打包

使用本机 Visual Studio 2022 的 LLVM/clang-cl、Windows SDK、CMake 3.24+：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -T ClangCL
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cpack --config build/CPackConfig.cmake -C Release -B build/package
```

要用 MSVC，另建构建目录并省略 `-T ClangCL`。CMake 仅获取固定 SHA-256 的 zlib-ng 源码，不下载游戏、译文或字体；离线构建可传 `-DFETCHCONTENT_SOURCE_DIR_ZLIBNG=<已解压源码目录>`。

CPack 输出不含资源的工具 ZIP 和校验文件，不能只发 EXE 而漏掉配置、BAT 与说明。测试使用合成 DAT/TEX/GXT、临时复制的本机 Windows 字体，覆盖角色映射、UV、顶部留白、策略、并行一致、非字体贴图隔离、缓存、报错、部署恢复与 BAT 错误码；不会启动或部署真实游戏。CI 负责 Debug/Release 构建测试和工具打包，不自动发布 GitHub Release。

工具自身代码使用随附 MIT 许可；它不授予字体、游戏资源或其他汉化包的分发权。来源和第三方许可见 [NOTICE.md](NOTICE.md)。
