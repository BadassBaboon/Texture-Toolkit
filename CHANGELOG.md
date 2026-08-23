# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Because a texture is identified by a hash of its pixels, a change to how that hash is computed
renames every file in every published mod. Those changes are called out under **Compatibility** and
are the reason this project treats the hash, the file naming, and the folder layout as a contract
rather than an implementation detail. See [Compatibility](README.md#compatibility).

## [Unreleased]

## [1.0.0] - 2026-08-24

First public release.

### Added
- Texture dumping and replacement at runtime for Direct3D 9 and Direct3D 11, x86 and x64, loaded as
  an `.asi` through Ultimate ASI Loader or as a `dinput8` / `d3d9` / `dxgi` proxy.
- In-game panel listing the textures in the current scene with hash, dimensions, mip count, format
  and status, a preview pane, filtering, and per-texture or bulk dumping.
- Replacement from `TT/inject` without a restart, including files added while the game is running.
- Replacements are built with the mip count their own file carries, so a deliberately short chain
  (UI art, or a texture authored against a known on-screen size) is applied as authored. Only a
  single-level file replacing a mipmapped texture has its chain filled in, and only for uncompressed
  formats. Mips are not generated for block-compressed replacements by design; see
  [Mip levels](README.md#mip-levels).
- Dumps written with their full mip chain, and with every slice of a texture array or cubemap, so an
  edited dump can be injected straight back.
- Special K texture packs load unchanged: files named the way Special K names them are recognised
  alongside our own and reported as "SK Injected" in the panel. Controlled by `AcceptSpecialKNames`,
  on by default.
- `ResourceRoot` in the ini, so `dump/`, `inject/` and `imgui.ini` can live wherever the user wants.
- Startup diagnostics: version, architecture, build time, host process and load path in the log, and
  a watchdog that reports status if the game has not presented a frame.
- Injection health in the panel: how many files were found, applied, and refused.
- Continuous integration building both architectures, publishing a draft release on a `v*` tag.

### Security
- A malformed or corrupt `.dds` in `inject/` could size an allocation from unvalidated header
  fields. Files downloaded from modding sites are untrusted input, and this ran underneath a draw
  call where a failed allocation ends the process. Dimensions, mip count and array size are now
  bounded, subresource sizes are recomputed in 64-bit to catch a wrapped row pitch, header reads are
  checked for truncation, and the whole parse is exception-guarded so a bad file is refused with a
  logged reason instead of taking the game down.

### Fixed
- A use-after-free when the inject folder was rescanned while the game was rendering: the bind fast
  path reads its cached replacement without the lock, so replacements are now retired for a couple
  of frames instead of being released underneath a draw call.
- A Direct3D 9 surface lock past mip 0 stamped that mip's hash on the parent texture, so mipmapped
  textures never matched a replacement.
- An unrecognised Direct3D 9 format read past the end of the locked rectangle. Unknown formats are
  now skipped and logged rather than guessed at.
- A recursive lock of a non-recursive mutex crashed the dump path on Direct3D 11.
- Textures whose only inject file used Special K's naming could be evicted from the panel.
- Games reaching Direct3D 11 through `CreateDXGIFactory2` got no overlay.
- A replacement's mip chain was clamped to the original texture's level count, silently discarding
  levels an author had deliberately exported.
- The short-chain warning fired on any chain shorter than the original's and told authors that
  compressed replacements "must ship a full mip chain", which the loader never actually required.
  It now fires only for a single-level file, where the shimmering it describes is real.

### Changed
- The C runtime is linked statically, so the plugin no longer needs a Visual C++ redistributable
  present in the host process to load at all.
- Built files carry their architecture: `TextureToolkit-x86.asi` and `TextureToolkit-x64.asi`.
- Dear ImGui and MinHook are fetched and pinned at configure time, so a clean clone builds without
  any sibling checkout. The handful of pixel-format helpers previously taken from ReShade are
  vendored verbatim under `deps/reshade` (dual-licensed BSD-3-Clause OR MIT, taken here under MIT).
- Texture eviction is driven by wall clock rather than frame count, so it behaves the same at any
  framerate.

### Compatibility
- The hash is 64-bit and covers mip 0's tightly-packed rows, never the driver's row padding, so a
  hash means the same thing on every machine. Files are named with 16 uppercase hex digits.
- Nothing published predates this release, so no existing mod needs renaming. From 1.0.0 the hash,
  the 16-hex file naming and the `<ResourceRoot>/inject|dump` layout are fixed. The set of
  recognised pixel formats may only grow: a format Texture Toolkit cannot identify is skipped
  rather than guessed at, so adding one later makes new textures moddable without changing a hash
  that already exists.

[Unreleased]: https://github.com/BadassBaboon/Texture-Toolkit/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/BadassBaboon/Texture-Toolkit/releases/tag/v1.0.0
