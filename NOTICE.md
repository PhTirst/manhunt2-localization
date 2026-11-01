# Sources and Distribution Boundaries

The font/DAT/DDS/TEX generation and preview modules were extracted from the author's local `manhunt2_tools` project. The JSON/Unicode/Manhunt 2 GXT and single-file I/O modules reuse the author's [manhunt2-gxt-json](https://github.com/PhTirst/manhunt2-gxt-json) implementation. This is a separate font/localization generator, not a replacement for that standalone text converter.

The project uses Windows GDI/GDI+ for rasterization and CNG for SHA-256. It does not bundle the Windows SDK, commercial RenderWare SDK, any game executable or shader implementation.

The only downloaded build dependency is **zlib-ng 2.2.5**, from the [official tagged source](https://github.com/zlib-ng/zlib-ng/releases/tag/2.2.5). CMake pins the source archive SHA-256 to `5b3b022489f3ced82384f06db1e13ba148cbce38c7941e424d6cb414416acd18`. The archive is not a game resource. Its license is preserved in `third_party/zlib-ng-LICENSE.md` and included in tool packages; zlib-ng is linked statically.

No ASI plugin, loader DLL, third-party localization patch, real translation corpus, TTF font, original DAT/TEX/GXT or expanded TEX template is included. Users provide assets and fonts they are entitled to use, and must separately confirm redistribution terms for their resulting patches. A clean snapshot means the user-verified original resources, not automatically the files currently installed after another patch.

The optional capacity TEX template is a user input, never downloaded or silently found in a parent directory. The generator restores non-font textures from the original snapshot; it must not import another patch's title image, advertisement or credits texture. Runtime Unicode/movie-subtitle handling belongs to separately supplied compatible ASI plugins and is not implemented by this offline generator.

本项目提取自作者自己的二代工具代码，复用已公开文本工具的解析与编解码实现；不包含其他汉化组的资源或插件。第三方源码依赖只有固定版本、固定校验值的 zlib-ng，其许可随包保留。游戏资源、容量模板、字体、译文和运行时 ASI 均由使用者自行提供，并自行确认使用和分发权限。
