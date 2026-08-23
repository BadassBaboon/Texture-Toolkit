<p align="center">
  <img src="texture-toolkit.png" alt="Texture Toolkit" width="640">
</p>

Texture Toolkit dumps and replaces textures at runtime in 32-bit and 64-bit Direct3D 9 and Direct3D 11 games on Windows. It loads as an `.asi` plugin through Ultimate ASI Loader, or as a proxy DLL renamed to `dinput8.dll`, `d3d9.dll`, or `dxgi.dll`. An in-game panel lists the textures in the current scene, shows their format and memory size, and lets you dump or replace them without restarting. It has been tested against Bully: Scholarship Edition (Direct3D 9), Grand Theft Auto IV: Complete Edition (Direct3D 9), Total Overdose (Direct3D 9), Spec Ops: The Line (Direct3D 11), and Need for Speed: The Run (Direct3D 11).

## How it works

Texture Toolkit hooks the calls that create and upload textures: `LockRect`/`UnlockRect` on D3D9, and `Map`/`Unmap` plus `CreateTexture2D` on D3D11. When a texture's pixels are uploaded it computes a 64-bit hash over that data and writes the hash onto the resource as D3D private data. At draw time it reads the hash back from whatever texture the game binds (`SetTexture` on D3D9, `PSSetShaderResources` on D3D11); if a replacement exists for that hash, it substitutes it before the draw.

Storing the hash on the resource, instead of tracking raw pointers, keeps a replacement attached to the right texture after the driver frees an address and reuses it for something else. On D3D9 the tool also follows `UpdateTexture`, so art that the game loads into a `SYSTEMMEM` texture and copies into a `DEFAULT`-pool texture is matched by the copy the game actually renders.

> [!NOTE]
> This project exists mainly for the purpose of being used with games that don't have native or community-developed texture modding tools, games that have limitations with texture modding, such as only fixed-resolution imports, or games that may have memory issues with direct game file texture modding. Games such as NFS: The Run and Bully: Scholarship had such limitations that inspired the creation of this tool.

## Features

- Direct3D 9 and Direct3D 11, both x86 and x64.
- DDS replacement: put `<hash>.dds` in `TT/inject` and it loads without a restart.
- Mip handling: a replacement is created with the mip count its own file carries. A single-level file replacing a mipmapped texture has its chain filled in when the format is uncompressed, and loads at its top level with a warning when it is not.
- Dumping to `TT/dump` as `.dds`, with the full mip chain: automatically on load, one at a time from the panel, or every tracked texture at once.
- 64-bit content hashing, so two identical textures share one hash and one replacement. The hash covers the texture's tightly-packed rows, not the driver's row padding, so a hash means the same thing on every machine and an `inject` folder can be shared as a mod.
- Special K texture packs load unchanged: files named the way Special K names them (eight hex digits, the CRC-32C of the top mip) are recognised alongside our own, and show as "SK Injected" in the panel.
- Input isolation and a software cursor, so the game stops reading the mouse and keyboard while the panel is open.

## The in-game panel

Press `INSERT` to open it. The left pane lists tracked textures with hash, size, mip count, format, and status: injected (a replacement is bound), pending (an inject file exists but nothing is bound yet), dumped, or original. The filter box matches on hash, dimensions, or format. Hover the list and press `[` or `]` to step through it.

The right pane inspects the selected texture. The preview shows the injected replacement, the live original while it is on screen, or the dumped `.dds` read back from disk. Below it are the dimensions, mip count, data size, format, the compressed and sRGB flags, and the D3D11 bind, usage, and misc flags. You can copy the hash or dump the texture from here, and drag the divider to resize the two panes.

## Building

Requires CMake 3.20 or newer, a Visual Studio toolchain with the Windows SDK, and git on PATH. Dear ImGui and MinHook are fetched automatically at configure time and pinned to the tags at the top of `CMakeLists.txt`, so a clean clone builds with no further setup.

### 32-bit (x86)

```cmd
cmake -B build32 -A Win32
cmake --build build32 --config Release
```

Output: `build32/Release/TextureToolkit-x86.asi`.

### 64-bit (x64)

```cmd
cmake -B build64 -A x64
cmake --build build64 --config Release
```

Output: `build64/Release/TextureToolkit-x64.asi`.

Match the build to the game: a 32-bit game needs the x86 build.

## Installing

1. Copy `TextureToolkit-x86.asi` (32-bit games) or `TextureToolkit-x64.asi` (64-bit games) into the game folder, or into a `plugins/` or `scripts/` folder when using Ultimate ASI Loader. To load it as a proxy instead, rename it to `dinput8.dll`, `d3d9.dll`, or `dxgi.dll`.
2. Launch the game. Texture Toolkit writes `TextureToolkit.ini` and its log next to the `.asi`, and creates a `TT/` folder next to the executable containing `dump/`, `inject/`, and `imgui.ini`.
3. Press `INSERT` to open the panel.

To replace a texture, read its hash from the panel (or dump it first), edit the `.dds`, and place it in `TT/inject` named after the hash, for example `5D3E2CCE1A7740B2.dds` or `0x5D3E2CCE1A7740B2.dds`. Dumps are written with their full mip chain, so an edited dump can go straight back into `inject` unchanged; if you author a block-compressed replacement yourself, export it with mipmaps. The `TT` folder name can be changed with `ResourceRoot` in the ini.

## Configuration

`TextureToolkit.ini` is created next to the `.asi` on first run:

```ini
[TextureToolkit]
HotKey=0x2D
ResourceRoot=TT
EnableInjection=1
AutoDump=0
FilterSmallTextures=1
ShowCurrentFrameOnly=1
AcceptSpecialKNames=1
ShowOSDBanner=1
Verbose=0
```

- `HotKey`: virtual-key code that toggles the panel (`0x2D` INSERT, `0x24` HOME, `0x74` F5).
- `ResourceRoot`: folder holding `dump/`, `inject/`, and `imgui.ini`; relative to the game folder, or an absolute path.
- `EnableInjection`: load replacements from the `inject/` folder.
- `AutoDump`: dump every texture to the `dump/` folder as it loads.
- `FilterSmallTextures`: ignore textures under 16x16.
- `ShowCurrentFrameOnly`: list only textures drawn in the current scene.
- `AcceptSpecialKNames`: also load files named the way Special K names them. Our own naming always wins when both exist for the same texture.
- `ShowOSDBanner`: show the startup banner.
- `Verbose`: write per-texture debug lines to the log; leave off for normal use, since it slows the game.

Toggling a checkbox in the panel writes its new value back to this file.

## Sharing a texture mod

A texture is identified by a 64-bit hash of its original pixel data, so an `inject` folder works
on anyone else's copy of the same game. To publish a mod, ship the `.dds` files and tell people to
drop them in `TT/inject` with Texture Toolkit installed.

Two things decide whether a hash matches on someone else's machine:

- **Game version.** A patch that reships texture assets changes their contents, and therefore their
  hashes. State the version you built against.
- **Texture quality settings.** Some games upload a smaller top mip at lower settings, which is
  different pixel data and a different hash. State the setting you authored at.

Neither depends on the player's GPU or driver: the hash covers the texture's tightly-packed rows,
never the driver's row padding, so it means the same thing on every machine.

## Mip levels

**Texture Toolkit builds a replacement with the mip count your file carries.** Mip count is an
authoring decision and is treated as one: a chain you deliberately stopped early is applied as
authored, without complaint. The one case that is filled in for you is a *single-level* file
replacing a mipmapped texture, which is equally what an author who meant it and an author who
forgot the export checkbox would produce -- and only for uncompressed formats, where downsampling
the source is cheap and clean.

For most world art, export **with a full chain**. A block-compressed replacement without mips
samples its top level at every distance and shimmers in motion, and a higher-resolution replacement
aliases *more* than the original did, not less. Mips cost about a third more VRAM. Dumps are written
with their full chain, so an edited dump is already correct.

Stopping the chain early is the right call in specific places, and nothing here will argue with you:
UI and HUD art drawn at or near 1:1 never samples below level 0 and pays VRAM for every level it
carries, and some textures are authored against a known minimum on-screen size.

> Anisotropic filtering is not a reason to ship fewer mips. Aniso exists to *correct* the
> over-blurring that mip sampling causes at grazing angles; dropping levels does not buy sharpness,
> it buys shimmer.

### Why mips are not generated for compressed formats

Missing mips cannot be recovered from block-compressed data. Producing them means decompressing,
downsampling, and re-encoding -- a second lossy pass over data that already took one -- so the
generated levels come out visibly softer than the ones a proper exporter would have made from your
uncompressed source. Texture Toolkit could pull in a BC encoder (DirectXTex) to do this
automatically, and deliberately does not: it would spend a dependency and a quality penalty to
paper over an export mistake, while also making it impossible to tell a deliberate short chain from
a forgotten one. The assumption is that someone authoring texture replacements knows which
compression and how many levels their texture wants. Export the mips you want; you will get them.

## Compatibility

These are fixed. Changing any of them would rename every file in every published mod, so they are
treated as a contract rather than an implementation detail:

- The hash: 64-bit, computed over mip 0's tightly-packed rows, with the algorithm in `TextureHash.h`.
- The filename: 16 uppercase hex digits plus `.dds`. A `0x` prefix is also accepted.
- The layout: `<ResourceRoot>/inject` and `<ResourceRoot>/dump`.

Special K's naming is accepted as a **second** key, never as a replacement for ours: an SK pack
drops into `inject/` and works, while files named our way keep working exactly as before. Adding a
compatibility naming is additive by construction and cannot rename anything.

The set of pixel formats Texture Toolkit recognises is deliberately **additive**. A format it cannot
positively identify is skipped rather than guessed at, so adding support for one later can only make
new textures moddable -- it can never change a hash that already exists.

## Limitations

- Direct3D 9 and Direct3D 11 only. DirectX 8, 10, 12, and Vulkan are not hooked.
- A DirectX 8 game run through a `d3d8to9` wrapper renders as Direct3D 9, so the overlay appears, but its textures stay invisible. The wrapper feeds pixel data into the D3D9 textures through an internal path that never calls a `LockRect`, `UpdateSurface`, `UpdateTexture`, or `StretchRect` we can hook, so there is nothing to hash. Capturing those would require hooking Direct3D 8 directly, which is not implemented. With `Verbose=1` the log fills with `Hooked_CreateTexture` lines and never a `Tracked` line.
- Injection reads `.dds` only. Dumps are written as `.dds`.
- A D3D9 texture in the default pool cannot be read back with `LockRect`, so the panel's Dump button fails on those; Auto-dump captures them from the upload instead.
- Mips cannot be generated for block-compressed replacements; a single-level compressed file loads at its top level and aliases in motion. See [Mip levels](#mip-levels) for why this is not done automatically.

## License

MIT. See [LICENSE](LICENSE).
