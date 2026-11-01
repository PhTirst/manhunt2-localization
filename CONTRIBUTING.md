# Contributing

- Use Windows x64 with Visual Studio 2022 C++ tools, a Windows SDK, CMake 3.24+ and clang-cl (the Visual Studio LLVM toolchain). MSVC builds are also supported.
- Read `README.md`, `docs/FORMAT.md` and `docs/SAFETY.md` before changing binary layouts or installation behavior.
- Configure/build/test with the README commands. Tests generate their own DAT/TEX/GXT fixtures and temporarily copy a locally installed Windows font. Never commit real game files, fonts, translated game dialogue or another patch's files.
- Keep JSON source fields and control tags unchanged. Do not silently normalize punctuation, apply Manhunt 1's space workaround, fall back to a different language column or reinterpret GXT Extra metadata.
- Preserve font section 1 -> font2, section 2 -> font1. Changes to TEX layout/offset handling need synthetic regression tests and separate explicit in-game verification. A successful offline preview is not proof of runtime compatibility.
- Build must not write to the installed game. Deployment must validate original/output manifests, refuse unrelated modifications and be recoverable. Restore may change only journal-owned resources.
- Update both language READMEs for user-visible behavior. Increment the build fingerprint version in `package.cpp` when changing rasterization, geometry, mapping, encoding or cache semantics.
- No automatic GitHub upload, release or real game deployment is performed by the test suite.
