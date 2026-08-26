# Tested games

**This list is not the compatibility boundary.** It records the games Texture Toolkit has actually
been run in, nothing more. Any 32-bit or 64-bit Direct3D 9 or Direct3D 11 game on Windows is in
scope, and most of them are expected to work without ever appearing here. A game missing from this
page has usually just never been tried.

If you get a game working, or find one that does not, a log with `Verbose=1` is worth more than a
description of what happened. It says which upload path the game uses, and that is what decides
whether textures can be seen at all.

## Confirmed

Textures appear in the panel, and dumping and replacement both work.

| Game | API | Notes |
|---|---|---|
| Bully: Scholarship Edition | Direct3D 9 | Main development target. Verified alongside ReShade. |
| Grand Theft Auto IV: Complete Edition | Direct3D 9 | Verified with a set of ASI and script mods loaded. |
| Street Racing Syndicate | Direct3D 9 | Needed the multi-vtable fix; see below. |
| Juiced | Direct3D 9 | |
| Total Overdose | Direct3D 9 | |
| Need for Speed: The Run | Direct3D 11 | |
| Spec Ops: The Line | Direct3D 11 | |

## Reported working

Reported by testers, on an older build than the current one, and not re-checked since. The hashing
was reworked after these runs, so their behaviour today is unverified rather than doubtful.

| Game | API | Notes |
|---|---|---|
| Saints Row 2 | Direct3D 9 | Plays, and textures are listed. See below. |
| Mad Max | Direct3D 11 | |
| Dead Rising 3 | Direct3D 11 | |

## Known issues

**Saints Row 2** stalled when the panel was opened during a load. The texture list was rebuilt from
scratch on every frame, which is expensive in a game that tracks a few thousand textures at once.
The list is now rebuilt a few times a second instead. Fixed, awaiting confirmation.

**Deus Ex: Mankind Divided** has never been confirmed. It reaches Direct3D 11 through
`CreateDXGIFactory2`, which is hooked, but nobody has run it since.

## What decides whether a game works

Texture Toolkit sees a texture at the moment its pixels are uploaded. Games differ in how they do
that, and the ones that fail usually fail for one of these reasons.

- **The upload path.** `LockRect` and `UnlockRect` on Direct3D 9, `Map`/`Unmap` and
  `CreateTexture2D` on Direct3D 11, plus `UpdateTexture`, `UpdateSurface`, `StretchRect` and the
  D3DX loaders. A game filling textures some other way has nothing for us to hash.
- **More than one texture vtable.** Direct3D 9 games were assumed to share a single
  `IDirect3DTexture9` vtable, and most do. Street Racing Syndicate does not: it created 1446
  textures whose pixels could only have arrived through a lock, and four of them were visible to us.
  Every distinct vtable is hooked now, which is what made that game work.
- **DirectX 8 through a wrapper.** The overlay appears because the wrapper renders with Direct3D 9,
  but the wrapper fills its textures internally without a call we can hook. See
  [Limitations](README.md#limitations).

With `Verbose=1`, a game that creates textures and never tracks any will fill the log with
`CreateTexture` lines and produce no `Tracked` line. That pattern is the signature of an upload path
we do not watch, and the `pool=` and `usage=` fields on those lines narrow down which one.
