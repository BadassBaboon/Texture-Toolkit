#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <d3d9.h>
#include <d3d11.h>

namespace TextureToolkit
{
    // ================================================================================================
    // Legacy (Texture Toolkit v1.0.x) texture hash algorithm.
    //
    // Replaced in v1.1.0 ("Improved texture hashing", commit 97cfad7) by the 64-bit Hash64 in
    // TextureHash.h/.cpp. Reproduced here, unchanged in behaviour, ONLY so:
    //   a) a v1.0 inject/ folder (8-hex-digit filenames) still matches its textures under v1.1+, and
    //   b) a one-time hash-migration pass can print old-hash -> new-hash pairs so an existing mod's
    //      files can be renamed to the current naming instead of running the game with two hash
    //      algorithms live forever.
    //
    // Do not "fix" or tidy anything in TextureHashLegacy.cpp: any behavioural change here would
    // silently stop it from reproducing the value old files are named after, which is the entire
    // reason this file exists. Two things distinguish it from the current algorithm, both load-
    // bearing for reproducing it exactly:
    //
    //   * It is 32-bit CRC-32 (the reflected zlib/PKZIP polynomial 0xEDB88320, init 0xFFFFFFFF,
    //     xorout 0xFFFFFFFF), not the 64-bit avalanche mix used since v1.1.
    //   * On Direct3D 11 (the Map/Unmap path) it hashed the mapped buffer AS THE DRIVER LAID IT
    //     OUT: `pitch * rows` bytes read contiguously from the mapped pointer, padding and all. It
    //     did NOT strip row padding to a tight row the way both the v1.0 Direct3D 9 path and the
    //     current v1.1+ algorithm do -- that dependency on the driver's chosen pitch is exactly
    //     the bug v1.1 fixed. It means a legacy hash recomputed under a different runtime (for
    //     instance a Direct3D 11 call translated by a Vulkan-based compatibility layer, instead of
    //     the native Windows driver the original file was dumped with) can come out different if
    //     that runtime reports a different row pitch for the same texture. That is not a mistake
    //     being carried forward here -- it is the definition of the value old filenames carry, and
    //     reproducing the legacy hash faithfully means reproducing this dependency along with it.
    //   * Direct3D 9 (LockRect/UnlockRect) DID strip row padding in v1.0, the same as it does today.
    // ================================================================================================

    // The CRC-32 (reflected, polynomial 0xEDB88320) that every hash below is built from. Byte-for-
    // byte the same table and loop v1.0 shipped in TextureHash.cpp.
    uint32_t compute_crc32_legacy(const uint8_t *data, size_t size);

    // Direct3D 9 LockRect/UnlockRect path, exactly as v1.0's calculate_d3d9_pixel_hash computed it:
    // DXT1/2/3/4/5 hashed as row-stripped 4x4 blocks, every other format hashed as row-stripped
    // pixels, both CRC-32'd. `pitch` is the pitch LockRect handed back for this level.
    uint32_t compute_legacy_hash_d3d9(const void *pixel_data, UINT width, UINT height, D3DFORMAT format, UINT pitch);

    // Direct3D 11 Map/Unmap path, exactly as v1.0's register_unmap_texture11 computed it: CRC-32
    // over `pitch * rows` bytes taken AS-IS from the mapped pointer -- row padding included, never
    // stripped (see the note above). `pitch` is the pitch Map() handed back for this subresource.
    uint32_t compute_legacy_hash_d3d11(const void *pixel_data, UINT width, UINT height, DXGI_FORMAT format, UINT pitch);

    // 8-character hex string, the width v1.0 filenames carry (e.g. "66882833"). A "0x" prefix is
    // also accepted on read, the same as the current 16-hex naming accepts one.
    std::string format_legacy_hash_hex(uint32_t hash);
}
