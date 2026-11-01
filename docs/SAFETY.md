# Safe Build, Deploy and Restore

## Immutable Original Snapshot

`capture-originals` reads `gameDirectory`, which must point to an installed game containing `Manhunt2.exe`. It copies the clean font DAT/TEX and GXT files, then seals the snapshot with SHA-256 hashes in `original.manifest.json`. It never replaces an existing sealed snapshot. Repeated capture verifies the snapshot instead of capturing the previous localization.

The tool cannot prove that a manually installed game is unmodified. Start from a known-clean installation. If you supply the resources yourself, `seal-originals` explicitly records their hashes; it does not magically certify provenance. Changing a sealed original file is an error. There is no silent "replace my originals with whatever is installed now" option.

## Build Is Offline

`build` reads original resources, JSON, TTF and an optional capacity template. It writes a staging package, validates/records the final hashes, and replaces the dedicated output directory only after success. A failed JSON parse, missing glyph, invalid font or too-small TEX slot keeps the previous output package. Unknown user files in an existing output directory are not silently deleted.

Default intermediate work stays in memory. A complete build manifest lives in `dist/`; optional raw glyph cache files live separately under `cacheDirectory` only when `cacheGlyphs` is true. Deleting a generated font/GXT/TEX or changing its bytes invalidates output reuse. A forced build does not bypass input validation or overwrite unrelated output files.

Ordinary directory replacement failures restore the previous output directory. This is not a power-loss-safe multi-file database transaction. Unexpected termination may leave `.mh2loc-*` recovery siblings; inspect them before deleting anything.

## Managed Deployment

Only `global/initscripts/font.dat`, `global/pictures/global_pc.tex` and GXT files listed in a verified build manifest can be deployed. The tool does not copy arbitrary files from dist, install ASI/DLL/EXE files, change save games or overwrite unrelated MOD files.

The first deployment checks that existing targets match the captured originals. Added resources must not already be occupied by an unrelated modification. Re-deployment checks the current journal-owned files against their previously deployed hashes. Editing those files externally causes a conflict rather than silent overwrite.

`.mh2loc-deployment.json` in the game directory records owned resource paths and the original snapshot identity. Updates restore removed managed paths from the original snapshot, or remove files previously added by this tool. Existing untracked game files remain unchanged.

`restore` uses this journal and the same original snapshot. It restores original files and deletes only journal-owned additions. If a managed file was edited, restoration stops so the edit is not silently lost. No journal means nothing to restore, not permission to overwrite the whole game tree.

Files are staged before replacement and replacement failures trigger rollback. Close the game before install/restore, keep the original snapshot and deployment journal, and ensure the tool can write to the game directory. An unwritable/locked resource is a reported failure, not success. Symlinks/junctions within managed resource trees and unsafe relative paths are rejected.

## What Is Not Included

- A clean game distribution or automatic source-of-truth detection.
- A capacity TEX resource or a new general-purpose TEX repacker.
- A runtime Unicode ASI or ASI loader.
- A translation corpus or a redistributable font collection.
- Deployment or launches against the author's actual installed game during tests.

## 中文说明

首次从已确认干净的安装目录保存原版资源并生成 SHA-256 清单，以后不覆盖这个快照。构建先在独立暂存目录完成，失败保留旧产物。安装只覆盖生成清单中的字体和 GXT，不复制其它 MOD、ASI、DLL 或 EXE。

恢复依赖游戏目录中的部署清单：原来存在的文件从原版快照还原，工具新增的文件才会被删除。如果文件被其它工具改过，会报冲突而不是覆盖；没有清单就不做恢复。请保留原版快照和部署清单，并在安装、恢复前退出游戏。
