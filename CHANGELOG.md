# Changelog

## 0.1.0 - 2026-11-01

- Extract the Manhunt 2 JSON/TTF generator without the TEX editor, ASI plugins, fonts, translations, game resources or historical experiments.
- Add a language-neutral `project.json`, role-based TTF discovery and simple build/install/restore BAT entry points.
- Keep the proven crossed font-role mapping, transparent top padding, variable advances and power-of-two DXT5 atlases; keep both TTF-first and original-first strategies.
- Make capacity templates explicit and retain fixed TEX slots; restore all non-font images from the original snapshot.
- Reuse glyphs in memory during one build; optional per-glyph disk cache is off by default. Verify complete output hashes before cache reuse.
- Preserve the last complete build on errors; capture/seal immutable original snapshots and journal deployment/restoration with conflict checks and rollback.
- Add standalone synthetic DAT/TEX/GXT/Windows-font regression tests, bilingual docs, CMake/clang-cl build, CPack ZIP packaging and Windows CI.

This release is not a new certification of in-game compatibility. No game was deployed or started while preparing this independent project. Existing local GXT/DAT/TEX outputs were verified byte-for-byte against the legacy generator; see `docs/VERIFICATION.md`.
