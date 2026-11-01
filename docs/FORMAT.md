# Format and Compatibility Notes / 格式与兼容性

## Manhunt 2, Not Manhunt 1

The GXT codec is the Manhunt 2 PC variant: `TKEY` entries are `offset:u32 + key[12] + extra:u32`, followed by UTF-16LE NUL-terminated text in `TDAT`. Extra is preserved as opaque metadata. Do not substitute a Manhunt 1/GTA codec or translate Key/Extra.

Only the configured language column is compiled. Other language columns remain editable source data. Missing columns, duplicate keys, overlong/damaged keys, malformed UTF-8/JSON and embedded NUL are errors. Selected non-BMP display characters are rejected because the font mapper uses BMP codepoints. Empty translated values are allowed.

Control tokens enclosed by `~...~` do not contribute font requirements; the actual GXT text is preserved. This includes unknown button tokens. A lone unmatched `~` remains an ordinary display character; this generator does not repair typoed tags. It does not rewrite punctuation, add U+2007 or insert zero-width breaks. Color/newline rendering requires compatible runtime support, not just glyphs.

## DAT and Glyph Geometry

Manhunt 2's `<FONT>` header stores the **map count minus one**, unlike the one-game-only assumptions that would make a copied writer corrupt another format. The source sections and their IDs stay in order. The supported PC role mapping is crossed:

| Source section | Texture | Default game size | Default TTF render size |
| --- | --- | --- | --- |
| 1 | font2 | 30px | 30px |
| 2 | font1 | 31px | 31px |

Other sections and regional font1r/font2r textures are not rebuilt. Game heightScale is derived from configured game size / 512. Render size and game size are independent settings.

TTF glyphs use GDI's metrics and actual glyph coverage; original-first preserves original advances. Full glyph cells and the original top safety padding are retained even under TTF-first. The generator does not silently crop an oversized glyph into a smaller cell. ASCII override, fallback fonts, alpha gain and black outline remain optional layers. Outline defaults to zero for both roles and does not change advances. Same-font/raw-size work can be reused in memory; role-specific layout/style is applied after that reuse.

## DDS / TEX

- Input font DDS must be DXT5. Output font DDS is DXT5, not Manhunt 1's DXT3.
- The atlas is a square power of two sized for the glyph set. Do not silently switch to NPOT textures; earlier game tests showed startup failures.
- Generated font DDS uses the established header convention of mipCount=0 with **one physical base level**. The builder does not generate a full mip chain.
- `global_pc.tex` can be raw `TCDT` or wrapped `Z2HM`/zlib. The reader follows physical texture records, preserving genuine duplicate names.
- Font replacement preserves the selected template's record offsets, DDS slots and surrounding layout. Payloads that exceed a slot cause an actionable error before any package is replaced.
- Some TEX record dimensions/mip metadata differ from the DDS header in existing compatible files. The generator keeps record metadata and writes the real generated dimensions in DDS, matching the previously tested path.
- A stock TEX may have insufficient font capacity for a large translation. Set `texTemplate` to a **user-supplied, compatible, larger-capacity TEX**. There is no hidden template search, automatic download or unverified TEX repacking fallback. The template must have the same texture names/occurrence counts as the original.
- Non-font images always come from the original snapshot. A capacity template is not permission to copy another patch's title/advertisement textures. If a template cannot represent the original non-font image layout, generation stops.

The DDS reader limits image dimensions to 8192; decompressed TEX is bounded to 512 MiB. These limits are defensive resource checks, not claims that the game supports every texture size within them.

## 中文要点

这是二代 PC 格式：GXT 的 Key 为 12 字节并有独立 Extra；DAT 的 fmap 字段存数量减一。第 1 个目标 section 使用 font2，第 2 个目标 section 使用 font1，不要按名字想当然对调。TTF 字形保留原版顶部安全空白，不沿用一代的零宽空格改写。

字体 DDS 使用 DXT5、合适大小的二次幂正方形、一个物理 mip。TEX 保留固定槽位；容量不足必须由用户明确提供兼容容量模板，不会自动找旧汉化包，也不会重排未知结构。模板中的非字体图片会恢复为原版图片，不会夹带旧汉化组广告。
