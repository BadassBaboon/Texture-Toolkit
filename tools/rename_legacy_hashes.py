#!/usr/bin/env python3
"""
Rename a texture mod built against the pre-release 32-bit hashes to the 1.0.0+ 64-bit names.

Why this needs your old dump folder
-----------------------------------
A file in inject/ is named after the hash of the ORIGINAL game texture, while the file itself holds
your replacement art, usually at a different resolution and format. So the new name cannot be worked
out from the replacement: nothing in it describes the texture it stands in for.

Your old dump/ folder does describe it. Each dump holds the original texture's pixels and is named
with its old hash, which is exactly the pair needed: old name from the file name, new name from the
contents. That makes the conversion exact and offline, with no need to replay the game.

Usage
-----
    python rename_legacy_hashes.py --dumps "C:\\path\\to\\old\\TT\\dump"

Run it from the folder holding the mod's .dds files, or point at them with --inject. Converted
copies are written to output/ and nothing already on disk is modified.
"""

import argparse
import shutil
import struct
import sys
from pathlib import Path

MASK64 = 0xFFFFFFFFFFFFFFFF
PRIME1 = 0x9E3779B185EBCA87
PRIME2 = 0xC2B2AE3D27D4EB4F


def _rotl64(x: int, r: int) -> int:
    return ((x << r) | (x >> (64 - r))) & MASK64


def _mix_word(h: int, word: int) -> int:
    h ^= (word * PRIME1) & MASK64
    h = _rotl64(h, 31)
    return (h * PRIME2) & MASK64


def hash64(data: bytes) -> int:
    """Port of TextureToolkit::Hash64 (src/TextureHash.cpp). Must match it byte for byte."""
    h = 0x27D4EB2F165667C5
    total = len(data)

    full = total - (total % 8)
    for i in range(0, full, 8):
        h = _mix_word(h, int.from_bytes(data[i:i + 8], "little"))

    if total != full:
        tail = data[full:] + b"\x00" * (8 - (total - full))
        h = _mix_word(h, int.from_bytes(tail, "little"))

    # Length fold, so trailing zero bytes cannot alias a shorter buffer.
    h ^= total & MASK64

    # Final avalanche.
    h ^= h >> 33
    h = (h * 0xFF51AFD7ED558CCD) & MASK64
    h ^= h >> 33
    h = (h * 0xC4CEB9FE1A85EC53) & MASK64
    h ^= h >> 33
    return h


# Bytes per pixel, or per 4x4 block for the compressed formats. Anything absent is refused rather
# than guessed at: a wrong size here would silently produce a wrong name.
BLOCK_FORMATS = {
    # DXGI_FORMAT values, as written into a DX10 header
    70: 8, 71: 8, 72: 8,             # BC1 typeless / unorm / unorm_srgb
    73: 16, 74: 16, 75: 16,          # BC2
    76: 16, 77: 16, 78: 16,          # BC3
    79: 8, 80: 8, 81: 8,             # BC4
    82: 16, 83: 16, 84: 16,          # BC5
    94: 16, 95: 16, 96: 16,          # BC6H
    97: 16, 98: 16, 99: 16,          # BC7
}

PIXEL_FORMATS = {
    2: 16, 3: 16, 4: 16,             # R32G32B32A32 typeless / float / uint
    9: 8, 10: 8, 11: 8,              # R16G16B16A16 typeless / float / unorm
    24: 4, 25: 4, 26: 4,             # R10G10B10A2 typeless / unorm / uint
    27: 4, 28: 4, 29: 4, 30: 4, 31: 4,   # R8G8B8A8 typeless / unorm / unorm_srgb / uint / snorm
    33: 4, 34: 4, 35: 4, 36: 4, 37: 4, 38: 4,   # R16G16 typeless / float / unorm / uint / snorm / sint
    40: 4, 41: 4, 42: 4,             # D32_FLOAT / R32_FLOAT / R32_UINT
    48: 2, 49: 2, 50: 2, 51: 2, 52: 2,   # R8G8 typeless / unorm / uint / snorm / sint (D3D9 A8L8 dumps as 49)
    53: 2, 54: 2, 56: 2,             # R16 typeless / float / unorm
    60: 1, 61: 1, 65: 1,             # R8 typeless / unorm (D3D9 L8), A8_UNORM (D3D9 A8)
    85: 2, 86: 2,                    # B5G6R5 / B5G5R5A1 (D3D9 R5G6B5 / A1R5G5B5)
    87: 4, 88: 4, 90: 4, 91: 4,      # B8G8R8A8 unorm / B8G8R8X8 unorm / B8G8R8A8 typeless / srgb
}

FOURCC_BLOCK = {b"DXT1": 8, b"DXT2": 16, b"DXT3": 16, b"DXT4": 16, b"DXT5": 16,
                b"ATI1": 8, b"BC4U": 8, b"ATI2": 16, b"BC5U": 16}


class UnsupportedDDS(Exception):
    pass


def mip0_payload(path: Path) -> bytes:
    """The tightly packed bytes of mip level 0, which is what the hash covers."""
    raw = path.read_bytes()
    if len(raw) < 128 or raw[:4] != b"DDS ":
        raise UnsupportedDDS("not a DDS file")

    height, width = struct.unpack_from("<II", raw, 12)
    pf_flags, fourcc, rgb_bits = struct.unpack_from("<I4sI", raw, 80)
    offset = 128

    block_bytes = None
    pixel_bytes = None

    if pf_flags & 0x4:  # DDPF_FOURCC
        if fourcc == b"DX10":
            dxgi_format = struct.unpack_from("<I", raw, 128)[0]
            offset = 148
            if dxgi_format in BLOCK_FORMATS:
                block_bytes = BLOCK_FORMATS[dxgi_format]
            elif dxgi_format in PIXEL_FORMATS:
                pixel_bytes = PIXEL_FORMATS[dxgi_format]
            else:
                raise UnsupportedDDS(f"unhandled DXGI format {dxgi_format}")
        elif fourcc in FOURCC_BLOCK:
            block_bytes = FOURCC_BLOCK[fourcc]
        else:
            raise UnsupportedDDS(f"unhandled FourCC {fourcc!r}")
    else:
        if rgb_bits % 8 != 0 or rgb_bits == 0:
            raise UnsupportedDDS(f"unhandled bit count {rgb_bits}")
        pixel_bytes = rgb_bits // 8

    if block_bytes is not None:
        tight_row = ((width + 3) // 4) * block_bytes
        rows = (height + 3) // 4
    else:
        tight_row = width * pixel_bytes
        rows = height

    size = tight_row * rows
    payload = raw[offset:offset + size]
    if len(payload) < size:
        raise UnsupportedDDS(f"truncated: expected {size} bytes of mip 0, found {len(payload)}")
    return payload


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dumps", required=True, type=Path,
                    help="the old TT/dump folder, whose files are named with the old 32-bit hashes")
    ap.add_argument("--inject", type=Path, default=Path("."),
                    help="folder holding the mod's .dds files (default: the current folder)")
    ap.add_argument("--out", type=Path, default=Path("output"),
                    help="where the renamed copies are written (default: output)")
    args = ap.parse_args()

    if not args.dumps.is_dir():
        print(f"error: --dumps is not a folder: {args.dumps}")
        return 2

    # Old name (from the dump's file name) -> new name (from the dump's contents).
    mapping = {}
    unreadable = []
    for dump in sorted(args.dumps.glob("*.dds")):
        try:
            mapping[dump.stem.upper().removeprefix("0X")] = f"{hash64(mip0_payload(dump)):016X}"
        except (UnsupportedDDS, OSError) as exc:
            unreadable.append((dump.name, str(exc)))

    print(f"read {len(mapping)} original texture(s) from {args.dumps}")
    if unreadable:
        print(f"  {len(unreadable)} dump(s) could not be read:")
        for name, why in unreadable[:10]:
            print(f"    {name}: {why}")
        if len(unreadable) > 10:
            print(f"    ... and {len(unreadable) - 10} more")

    sources = sorted(p for p in args.inject.glob("*.dds") if p.resolve() != args.out.resolve())
    if not sources:
        print(f"no .dds files found in {args.inject.resolve()}")
        return 1

    args.out.mkdir(parents=True, exist_ok=True)
    converted, missing = 0, []
    for src in sources:
        key = src.stem.upper().removeprefix("0X")
        new_name = mapping.get(key)
        if new_name is None:
            missing.append(src.name)
            continue
        shutil.copy2(src, args.out / f"{new_name}.dds")
        converted += 1

    print(f"\nconverted {converted} of {len(sources)} file(s) into {args.out.resolve()}")
    if missing:
        # Nothing is guessed at. A file with no matching dump keeps whatever name it had and is
        # listed here, because a wrong name is worse than an absent one: it would load silently
        # onto the wrong texture.
        print(f"\n{len(missing)} file(s) had no matching dump and were NOT converted:")
        for name in missing[:20]:
            print(f"    {name}")
        if len(missing) > 20:
            print(f"    ... and {len(missing) - 20} more")
        print("\nThose textures were not in the dump folder. Dump them with the version that made\n"
              "this mod, or find the missing originals, then run this again.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
