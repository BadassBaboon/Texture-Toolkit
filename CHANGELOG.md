# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Because a texture is identified by a hash of its pixels, a change to how that hash is computed
renames every file in every published mod. Those changes are called out under **Compatibility** and
are the reason this project treats the hash, the file naming, and the folder layout as a contract
rather than an implementation detail. See [Compatibility](README.md#compatibility).

## [Unreleased]

## [1.1.0] - 2026-08-26

Games that never showed a texture now work, and two ways a game could stall are gone. Nothing here
changes how a texture is identified: hashes, file names and folder layout are exactly as 1.0.0 left
them, so every mod built against 1.0.0 keeps working untouched.

### Added
- **Direct3D 9 textures whose vtable differed from the first one created are seen.** Only the first
  created texture's `LockRect` and `UnlockRect` were hooked, on the assumption that every
  `IDirect3DTexture9` shares one vtable. Most games do. Street Racing Syndicate does not: it created
  1446 textures whose pixels could only have arrived through a lock, and four were visible to us.
  Every distinct vtable is hooked as soon as a texture using it appears, which made that game work.
- [GAMES.md](GAMES.md), recording the games Texture Toolkit has been run in and what decides whether
  a game works at all. It is a record of what has been tried, not a supported-hardware list.
- **Textures bound to vertex and compute shaders are seen.** Only the pixel stage was hooked, so
  anything sampled by a vertex shader (terrain displaced from a heightmap, for one) or read by a
  compute pass never appeared in the panel and could not be replaced at all.
- **Direct3D 9 textures loaded through D3DX are tracked.** A game can hand a file to
  `D3DXCreateTextureFromFile*` and never lock the texture itself, in which case its art was created
  but never seen. All six D3DX texture loaders are hooked, in whatever `d3dx9_*.dll` the game has
  already loaded; nothing is ever loaded by us, so a game shipping no D3DX is untouched. No game is
  yet known to need this. It is in because the loaders are a real upload path we did not watch.
- `UpdateSurface` and `StretchRect` carry the content tag from a staged surface to the one the game
  renders, the way `UpdateTexture` already did. Only whole-surface copies qualify; a partial or
  scaled blit resamples the pixels and is genuinely a different texture.

### Fixed
- **An injected texture showed as "Pending" once the game re-uploaded it.** The tracked record is
  rebuilt whenever a texture's pixels arrive again, and the rebuilt one carried no replacement even
  though the replacement itself was untouched and still on screen. The texture went on rendering
  replaced while the panel said it was not, which is the panel lying about the one thing it exists
  to report. Seen in Bully, where art is re-uploaded routinely.
- Opening the panel could stall a game that tracks thousands of textures. The list was rebuilt from
  the whole tracked map on every frame, under the lock every texture upload also needs, copying ten
  strings per row. Saints Row 2 reaches 2615 textures and stopped dead when the panel was opened
  during a load. The list is rebuilt a few times a second, and at once after Reload or a dump.
- The panel title, the startup banner and the log all said `INSERT` no matter what `HotKey` was set
  to. They name the key actually configured.
- On Direct3D 11 the overlay fetched the back buffer and created a render target view on every
  presented frame even with nothing to draw, which is a driver-side resource creation per frame for
  the whole time the panel is closed. The pass is skipped when neither the panel nor the banner is
  on screen.
- **Games could stall or freeze while video played or a save loaded.** A texture the game rewrites
  constantly, such as a video frame, was hashed in full on the game's own thread every time, and
  every distinct result became another row in the texture list. Saints Row 2's intro produced 268
  uploads of one 640x360 surface and 134 of a 1280x720 one, 479 distinct hashes in all. None of
  that work can pay off, because a replacement is matched by content and content that changes every
  frame can never match a file on disk. A resource rewritten several times in quick succession is
  now left alone. The test is the rate of change, not the count, so an engine that recycles texture
  objects between levels keeps working.
- The Special K hash meant a second full pass over the pixels of every texture uploaded, whether or
  not any SK-named file was present. It is computed only when one is.
- Diagnostic logging budgets were per call site and counted calls, so one texture re-locked every
  frame could spend the whole budget. In the log that prompted this, a 640x480 video surface locked
  19 times in under a second used every `LockRect` line available and hid the following 30 seconds
  entirely. Budgets are spent per distinct texture now, and are checked against the verbose setting
  before any message is built.
- A surface lock past mip 0 returned from the level-0 guard before its log line, so mip uploads
  never appeared in the log at all.
- A lock on a surface with no parent texture (offscreen-plain or render-target) returned silently,
  which is exactly the case that is hardest to diagnose from a log. It is reported now.

### Compatibility
- The hash, the 16-hex file naming and the `<ResourceRoot>/inject|dump` layout are unchanged from
  1.0.0. A mod published against 1.0.0 needs no renaming and no re-export.

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

[Unreleased]: https://github.com/BadassBaboon/Texture-Toolkit/compare/v1.1.0...HEAD
[1.1.0]: https://github.com/BadassBaboon/Texture-Toolkit/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/BadassBaboon/Texture-Toolkit/releases/tag/v1.0.0
