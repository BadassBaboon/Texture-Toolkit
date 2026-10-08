# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Because a texture is identified by a hash of its pixels, a change to how that hash is computed
renames every file in every published mod. Those changes are called out under **Compatibility** and
are the reason this project treats the hash, the file naming, and the folder layout as a contract
rather than an implementation detail. See [Compatibility](README.md#compatibility).

## [Unreleased]

### Added
- **The panel scales with the resolution.** It was laid out for 1440p and drawn at that size
  everywhere, so it looked tiny at 4K and oversized at 1080p. It now takes the same share of the
  screen at any resolution (1440p 100%, 4K 150%, 1080p 75%), with text rasterised at the real
  pixel density rather than stretched, so it stays sharp. The Settings page can switch this off and
  set a size by hand, saved as `UIScale` in TextureToolkit.ini (`0` is automatic).

### Fixed
- **Direct3D 11 games that draw through deferred contexts showed no textures in the scene, and
  replacements never reached them.** Binding was hooked on the immediate context only, and a
  deferred context, which a game fills on worker threads and plays back later, is a separate
  implementation: on the same device every one of the eight hooked functions sits at a different
  address. The Sims 4 bound not one texture through the hooks in 45 seconds of play. Contexts of
  both kinds are now hooked, each call forwarded to the original of the context it was made on.
  Textures bound on those worker threads were also recorded in a per-thread buffer that only the
  render thread ever emptied each frame, so they flashed into the scene list and dropped out again.
  Each thread now hands its buffer over on its first bind of a new frame.
- **Direct3D 11 textures uploaded through a staging copy never showed in the scene.** A game that
  fills a staging texture with `Map`/`Unmap` and copies it into the texture it draws had only the
  staging copy hashed, and a staging texture can never be bound. The Sims 4 does this for every
  texture: 211 were tracked and "Current scene only" listed none. The content tag now follows
  `CopyResource` and a whole top-level `CopySubresourceRegion` onto the texture that is drawn,
  including the Direct3D 11.1 `CopySubresourceRegion1`, which is the one The Sims 4 uses. A copy
  only carries the tag into a texture of the same size, so an atlas built from tiles is never
  mistaken for its first tile.
- Textures written with `UpdateSubresource` or `UpdateSubresource1`, the other common Direct3D 11
  upload path, are tracked. A destination box that covers the whole texture counts as a full upload.
  L.A. Noire passes one on every texture, so its UI, fonts and world textures were never listed.
- **No overlay in a Direct3D 11 game that presents from a swapchain other than its first, or with
  `Present1`.** Only the first swapchain's `Present` was hooked, on the assumption that every later
  swapchain shares its code, and `IDXGISwapChain1::Present1` was not hooked at all. The Sims 4 makes
  a device and swapchain at startup and its real ones later, and tracked 207 textures without one
  frame ever passing through the hook, so the panel never opened. Every distinct `Present`,
  `Present1`, `CreateSwapChain` and `CreateSwapChainForHwnd` is hooked now, each call forwarded to
  the original of the object it was made on.
- The startup watchdog blamed OpenGL for a Direct3D 11 game that had created a device but not yet
  presented a frame through the hook. It now says that a device exists and names the likely causes.

### Changed
- **"Skip under 16 x 16" is now "Skip 16 x 16 and smaller".** Spec Ops: The Line has hundreds of
  16x16 placeholders that the old cut-off let through. They are hidden from the list and left out
  of Dump all, but still tracked, so mods that replace one keep working.
- **Controllers are no longer blocked while the panel is open.** The game used to get an all-zero
  input state from every DirectInput device, which is harmless for a keyboard or mouse, but on a
  controller a zero axis often means hard left or up. Only the keyboard and mouse are blocked now,
  so the game can still be played with a controller while the panel is open.

## [1.2.1] - 2026-10-05

### Changed
- **A newly installed mod goes in at the top of the load order.** New mods used to start at the
  bottom, below `TT/inject`, so any texture they shared with it came from `TT/inject` instead. A
  PlayStation button pack copied into an install whose `TT/inject` already held the same menu sprite
  sheet showed a mix of Xbox and PlayStation prompts until it was moved up by hand. The mod installed
  last now wins, and applies in full in one step. Its place is saved the first time it is seen, so
  anything arranged by hand stays where it was put. This replaces the 1.2.0 rule that `TT/inject`
  starts at the top: to keep your own edits ahead of a mod, move `inject` above it.
- A mod partly covered by one above it says so on its own line, in the warning colour, with what to
  do about it. It used to be a few faint words in the middle of the file count.
- Folders whose names start with `dump` are never loaded as mods, not just `dump` itself, so a dump
  renamed to `dump-garage` stays on disk without the next Reload injecting it back.

## [1.2.0] - 2026-10-04

A redesigned panel, texture mods with a load order, and fixes carried over from
[toptensoftware's fork](https://github.com/toptensoftware/Texture-Toolkit). Nothing here changes how
a texture is identified: hashes, file names and the `TT/inject` layout are exactly as 1.1.0 left
them, so every existing mod keeps working.

### Added
- **Flip V and Flip H on the inspector's preview**, for art a game stores upside down or mirrored.
  They change the preview only, never the texture.
- **Delete dump** in the inspector removes the selected texture's `.dds` from `TT/dump`, after a
  confirmation, and clears its Dumped status.
- **Special K's Direct3D 9 packs match textures a game loads through D3DX.** For a texture D3DX
  builds from a file, Special K's name is a CRC-32C of the file's bytes, not of the pixels, so its
  packs never matched those textures here. The same checksum is now taken where we already watch
  D3DX, and only while Special K-named files are present, as with the rest of our Special K
  matching.
- **Blink in game.** The texture selected on the Textures page blinks magenta in the game, so the
  thing it is drawn on can be found by eye instead of by elimination. On by default, switched in the
  inspector, and saved as `HighlightSelected`. Taken from Special K's "Highlight Selected Texture in
  Game", with one change: for half of each blink a solid magenta texture stands in for the selected
  one, where Special K binds nothing. Nothing reads as black in Direct3D 9 but as transparent in
  Direct3D 11, so there the texture only vanished; magenta looks the same everywhere and stands out
  against dark art. Textures magenta cannot stand in for (cube, volume and integer textures, and
  vertex-shader textures in Direct3D 9) get nothing bound instead, which both APIs allow. Runs only
  while the Textures page is open, and stops as soon as the panel closes. Direct3D 9 games that bind
  a texture once and leave it bound (Bully, for one) are re-applied once a frame, so the blink does
  not freeze there; the panel's own preview never blinks. With Verbose logging on, the log reports
  once a second how often the game drew the blinking texture: art the game draws once into an
  image it reuses (some HUDs and menus) shows zero there and cannot blink.
- **Texture mods.** Every folder in `TT` other than `dump` and `inject` is loaded as a mod of its
  own, subfolders included, so a downloaded mod no longer has to be merged into `inject`. The Mod
  files page lists each one with a switch and up and down buttons for the load order: where two
  ship the same texture, the higher one wins. `TT/inject` is part of that order and starts at the
  top, so your own edits win until you move a mod above them. An optional `mod.ini` gives a mod its
  name, author, version, description, and whether it is on by default; see
  [`tools/mod.ini.example`](tools/mod.ini.example). The panel's choices are kept in
  `TextureToolkit.ini` under `[Mods]` and `[ModEnabled]` and win over a mod's default. With
  `ResourceRoot` set to the game folder itself, only folders carrying a `mod.ini` count, so game
  data is never scanned. Builds on the `AdditionalSearchPath` overlay folders in toptensoftware's
  fork.
- **Build Info on Diagnostics**, covering what a bug report needs: the game and its folder, where
  the `.asi` loaded from, the Windows version (and Wine/Proton), the GPU with its driver or VRAM, the
  resolution, other software hooked into the game (ReShade, Special K, RivaTuner, the Steam, Discord
  and OBS overlays, proxy DLLs in the game folder), every setting, the replacement and mod counts,
  and the session length. **Copy** puts it on the clipboard as text and writes it to the log.
- **A Verbose logging switch** on Diagnostics, applied at once. It used to need an ini edit and a
  restart.
- **"Log this frame"** on Diagnostics. Writes every texture the game draws with in the next frame to
  the log, each with its hash, size, format, usage and pool. The ordinary per-texture logging is
  capped at a handful of textures per session, which cannot answer the question users actually
  arrive with: which of these hundreds is the thing on screen. Point the camera at it, press the
  button, and whatever it is drawn with is in that list.
- **A "Join Discord" button** at the foot of the sidebar opens Baboon's Workshop, the Texture
  Toolkit Discord, in the browser.
- The panel reports how many tracked textures the scene filter is hiding, with one click to show
  them. Art the game uploads but never draws with, such as livery pieces composited into a render
  target, is tracked and then filtered straight back out, which read as the tool failing to see it.
- The log records the configuration values in use, at startup and whenever a setting is saved, not
  only where the ini was read from. A texture hidden by `ShowCurrentFrameOnly` used to be
  indistinguishable in a log from one never tracked.
- With `Verbose=1`, a bound texture is described as it is bound: dimensions, format, usage and pool,
  and a plain statement when it is a render target that has no file behind it and cannot be
  replaced. This answers "the texture I can see is not in the panel" straight from a log.
- **The startup report names the likely cause** when a game shows nothing. Twenty seconds in, the
  log already gave device and frame counts; it now also lists the overlays and wrappers hooked into
  the game, says which unsupported graphics API is loaded when no Direct3D 9 or 11 device appears
  (Direct3D 12, Vulkan, DirectX 8 or 10, OpenGL), points at another overlay owning Present when
  frames never reach us, and catches a new case: frames presented but no texture ever seen, the
  mark of a wrapper or an upload path we do not watch. Software is named from each DLL's own
  version resource, so a proxy DLL in the game folder reads as what it is ("dinput8.dll in the game
  folder: Ultimate ASI Loader 9.7.2", "ReShade 6.8.0 (d3d9.dll)"), and ReShade add-ons by file.
- **Per-hook timings in the verbose log.** Every five seconds a `[Timing]` line gives the frame
  count, the average frame time, how many frames hitched (over twice the previous average and over
  20 ms, so a game capped at 30 fps does not read as all hitches) and the worst one, and for texture
  uploads, texture binds and the overlay on each API, how many calls there were and how long Texture
  Toolkit's own part of them took: total, average and worst. Only our work inside a hook is timed,
  never the game's or the driver's call it wraps, so the figures say whether a stutter is ours. With
  verbose logging off it costs one flag check per call. Measured with the system clock, not the
  game's: a frame-rate unlocker that hooks the game's timers ran its clock about 18x fast in NFS:
  The Run, which would otherwise have made nonsense of every figure. When the game's clock is off
  like that, the log says so once. Suggested by the diagnostics build in Aqvilinus's fork.

### Changed
- **A redesigned panel.** A sidebar splits it into Textures, Mod files, Settings and Diagnostics,
  with figures for tracked, injected, not applied and dumped textures, switches in place of
  checkboxes, statuses as coloured labels, and Segoe UI in place of the built-in pixel font
  (Consolas only for hashes). The startup banner matches it, and no longer takes a window of its own.
- **The Texture Toolkit logo, and a palette taken from it.** The brick-and-stone logo heads the
  sidebar and the startup banner, compiled into the `.asi` from `assets/logo.png` (regenerated into
  `src/LogoData.h` by `tools/embed_logo.py`), so there is still one file to install. The panel stays
  dark, in warm charcoal and stone with the logo's brick for selections, switches and primary
  buttons; brick that reads as text is a lighter shade of it, and the error red is pinker than the
  brick so the two cannot be confused.
- **Each control sits with what it acts on.** Textures has dumping (Auto-dump, Dump all, the dump
  folder) and the list filters (Current scene only, Skip under 16 x 16). Mod files is replacements
  and mods only. Settings is the overlay and the folder locations. Diagnostics has Log this frame,
  verbose logging and Build Info.
- Larger text throughout, on one scale: descriptions 15 px, body 17, card headings 18, page titles
  23. Paths are set in the body face rather than Consolas, which read as a different size beside it.
- The sidebar is as wide as its contents need and no wider, and credits BadassBaboon again, as
  before the redesign. Its status box appears only once there is something to report (a reload, a
  dump, a refused file) instead of reading "Ready", and failed replacement files show as a red count
  on Mod files.
- The texture list draws only the rows on screen. It used to submit every row every frame, which
  is a few thousand in a game like Saints Row 2.
- Column headers sort the list. The format column uses the short names modders use (RGBA8, BC3,
  BC7 sRGB, DXT5); the inspector still gives the full one, and search matches both.
- The inspector previews on a checkerboard, so transparent pixels read as transparent.

### Fixed
- **The panel's own font was listed as one of the game's textures.** ImGui's font atlas went
  through the same hooks as the game's textures, so it appeared in the texture list (a 512x128
  entry) and could be dumped, replaced, or picked for Blink in game, which then blinked the whole
  panel. The overlay's own creation and drawing now bypass the hooks entirely.
- **Special K-named replacements stayed off after Reload, or after switching any mod on or off.**
  Both drop every replacement and rebuild each one the next time its texture is drawn, but only
  files named our way were looked for, so a replacement from a Special K-named file came back only
  when the game happened to upload that texture again. A Special K file waiting to apply also
  showed as Original instead of Pending.
- **Timers ran on the game's clock.** A frame-rate unlocker that speeds up the game's timers (one
  ran them 18x and 250x fast in NFS: The Run) made a key typed into the panel repeat at once, the
  startup banner vanish in a blink, tooltips and the switches' animation jump, a double-click all
  but impossible, and the checks for unused and constantly rewritten textures misjudge their age.
  All of them use the real clock now, as the timing reports already did.
- **A game that replaced its Direct3D 9 device, instead of resetting it, lost the panel.** The
  overlay kept drawing with the old device, and replacements built on it were bound to the new
  one. The panel moves to the new device once the old one stops presenting, and a replacement
  made on a device that is gone is rebuilt for the new one when its texture loads again, on
  Direct3D 11 too. Dumps and render-target read-backs use the texture's own device.
- Dumping an injected texture from the inspector showed it as Dumped, hiding that it is injected;
  the other dump paths already kept the status.
- A dump that could not be written in full (a full disk, say) was reported as written and left a
  truncated `.dds` behind. It is reported as failed and the partial file removed. A volume (3D)
  `.dds` in `inject` was read as if it were 2D, mips from the wrong place; it is refused with a
  reason now.
- A first-run `TextureToolkit.ini` is written as UTF-16, so a mod folder (or `ResourceRoot`) named
  outside the system code page is no longer saved as question marks and forgotten. A
  `ResourceRoot` longer than 260 characters was cut short, and an ini folder that could not be
  read could throw at startup.
- `tools/rename_legacy_hashes.py` used the wrong pixel size for R16G16 and R8G8 dumps and did not
  know A8, B5G6R5 or B5G5R5A1, which would have produced wrong names for those textures.
- The texture memory figure could wrap around on a very large texture.
- The README described renaming the `.asi` into a `dinput8.dll`, `d3d9.dll` or `dxgi.dll` proxy.
  It exports nothing a game imports, so that never worked: Ultimate ASI Loader is the proxy, and
  loads the `.asi`.
- **A Direct3D 9 preview showing one flat colour instead of the texture.** The overlay drew with
  whatever minimum mip level, LOD bias or texture-coordinate transform the game had left on the
  first texture stage, so a mipmapped texture could preview as its smallest mip. Those are now set
  for the overlay's draw and handed back to the game afterwards.
- **A Direct3D 9 device reset failing while a texture was selected.** The inspector's live preview
  and queued dumps held references to the game's textures, and `Reset` fails while anything holds
  a `D3DPOOL_DEFAULT` resource; on an Alt-Tab or a resolution change the game could lose its
  device. Those references are now let go before the reset, and queued dumps are taken again on
  the texture's next draw.
- Addresses in the log were printed in decimal after a `0x`, and two were cut to 32 bits on the
  x64 build. They are hex now, at full width on both builds.
- **A Direct3D 11 crash reading a stale `Map()` pointer.** The bookkeeping between `Map` and `Unmap`
  was keyed on the resource's raw pointer with no reference held. A texture the game released while
  still mapped could be destroyed, its address reused by another, and that one's `Unmap` would hash
  memory that no longer belonged to anything. A reference is now held while the entry exists, the
  entry is always cleaned up (it used to be left behind while a dump was reading back), the
  bookkeeping is shared across threads, and buffers are no longer recorded at all. Ported from
  toptensoftware's fork.
- **A crash or error when quitting a game.** The dump worker thread was still attached when the
  runtime destroyed the texture manager at process exit, which aborts the process. Ported from
  toptensoftware's fork.
- **An inject file refused when its texture first loaded was never counted as failed.** Only a
  file retried later by hot reload was recorded, but most files exist before the game starts and
  fail on the first try, so the panel said "0 failed" while the texture sat at Pending for good. It
  is counted now, and a known-bad file is no longer retried on every re-upload of its texture.
  Reload still clears the record, so a fixed file is tried again.
- A refused file shows as **Failed** rather than Pending. Pending said it would apply, and it never
  would.
- **The panel's close button did nothing on the Mod files, Settings and Diagnostics pages.** The
  scrolling page region was a window drawn over it and took the clicks.
- Switching "Accept Special K names" off left Special K replacements on screen until the next
  Reload; it rescans at once now. Switching "Skip under 16 x 16" on left tiny textures that were
  already tracked in the list; they are hidden at once now.
- A folder path containing a character outside the system code page could throw while the panel
  drew it.
- **Nothing could be typed into the panel's search box.** The panel took its keys from the
  game's window messages, and a game reading its keyboard through DirectInput, Bully among them,
  may never turn a keystroke into a message at all; where messages did arrive, they were taken
  before the game's own `TranslateMessage` could make characters of them. The panel now reads the
  keyboard the way it already read the mouse, from the hardware state each frame, and turns key
  presses into characters with the active keyboard layout (Shift, Caps Lock and AltGr included),
  with key repeat. Window messages are still kept from the game but no longer fed to the panel, so
  nothing is typed twice where both exist. Alt+key no longer makes Windows beep while the panel is
  open, and `[` and `]` type into the search box instead of stepping through the list.
- **Clicks could reach ImGui twice** when a game peeked at its queue without removing the message;
  they are handed over only when the message is actually taken.
- **The mouse cursor could stay hidden after the panel closed.** The OS cursor's display count was
  pinned to hidden while the panel was open and never put back, so a game that shows the Windows
  cursor had none afterwards. The count it had is restored when the panel closes, and ImGui's
  answer to `WM_SETCURSOR` now stands on Direct3D 9 as on Direct3D 11, instead of the game
  replacing it.
- **A game thread polling the keyboard could read keys typed into the panel.** The exemption that
  lets our own code read real key state was one flag for the whole process, raised while the panel
  was built each frame, so any other thread calling `GetAsyncKeyState` in that window read straight
  through. It is per thread now. Games that read the keyboard on their render thread, which is most
  of them, were never affected. Found in Aqvilinus's fork, as was the cursor fix.
- The panel's open/closed state is atomic, since the hotkey changes it on the render thread while
  the input hooks read it on others.
- **Special K packs whose files start with `Compressed_` were ignored.** Only the `Uncompressed_`
  prefix was recognised.
- **Files whose names only begin with a hash loaded as that hash.** The name was read up to its
  first non-hex character, so `5D3E2CCEbackup.dds` replaced texture `5D3E2CCE` and `0.dds` claimed
  hash 0. A name has to be exactly a hash now (16 hex digits, optionally `0x`, or one of Special K's
  forms), and any `.dds` that is not is listed in the log instead of being skipped silently.
- **A game installed in a folder whose path has characters outside the Windows code page** (a
  Cyrillic or Japanese folder name on an English Windows, say) could crash at startup, and
  replacements, dumps and the panel's layout file under such a path failed. Paths now travel as
  UTF-8 and are opened through the wide Windows API.
- **An install path longer than 260 characters crashed the game at startup**: copying it into a
  fixed buffer tripped the runtime's overflow check. Paths of any length are read in full.
- **Mod files and Build Info said "0 found" for a folder of Special K-named files**: the count
  only included our own naming. Build Info's session length is also measured in real time now, not
  by the game's clock, which a frame-rate unlocker can speed up.
- **A Special K-named file in a higher mod lost to our own naming in a lower one.** The two namings
  are looked up in separate tables, and ours was always tried first, so load order only held
  between files named the same way. Each file now carries its source's place in the load order,
  and where both namings match a texture the higher source wins (within one folder, ours still
  does). Replacements rebuilt after Reload make the same choice.
- The panel's keyboard polling looks up scan codes once per keyboard layout and tells ImGui only
  about keys that changed, instead of several hundred calls a frame while the panel is open.
- Unloading the `.asi` tore the texture manager down while its hooks could still call into it; the
  hooks are removed first now.
- **Every switch flipped in the panel rewrote `TextureToolkit.ini` from scratch**, discarding any
  comment or key a user had added by hand. An existing ini is updated a key at a time now, and
  `HotKey` and `ResourceRoot`, which the panel never changes, are left exactly as written. A first
  run still writes the full commented file.
- **The file preview could stay stale or blank.** It was cached by texture hash alone, so a dump
  rewritten under the same name, or one still being written when the panel first showed it, kept
  the old picture for as long as that texture stayed selected. The file's timestamp is part of the
  key now.
- **DDS files with a wrong size in their header were refused without a word.** Some old exporters
  write it incorrectly; the header is the same 124 bytes whatever it claims, so it is read anyway.
  `DXT2` and `DXT4` (premultiplied DXT3 and DXT5) load too.
- **A DDS in an unrecognised format was read as RGBA8**, putting garbage on screen; a 16-bit or
  luminance file did this. It is refused now, and the log says which format it was.

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

[Unreleased]: https://github.com/BadassBaboon/Texture-Toolkit/compare/v1.2.1...HEAD
[1.2.1]: https://github.com/BadassBaboon/Texture-Toolkit/compare/v1.2.0...v1.2.1
[1.2.0]: https://github.com/BadassBaboon/Texture-Toolkit/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/BadassBaboon/Texture-Toolkit/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/BadassBaboon/Texture-Toolkit/releases/tag/v1.0.0
