#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace TextureToolkit
{
    // 64-bit content hash used to identify textures.
    //
    // Two properties matter, because the hash is the filename a texture mod ships under and so
    // must mean the same thing on every machine:
    //   * It is computed over the texture's TIGHT rows only. Row padding is chosen by the driver
    //     (and can even be uninitialised), so hashing it would make the same texture hash
    //     differently on another GPU, or between runs.
    //   * It is 64-bit. A 32-bit hash collides with meaningful probability once a game reaches a
    //     few thousand textures (~5% at 20k), and a collision means one texture silently gets
    //     another's replacement.
    class Hash64
    {
    public:
        void update(const uint8_t *data, size_t size);
        uint64_t finish() const;

    private:
        uint64_t m_h = 0x27D4EB2F165667C5ull;
        uint8_t m_buf[8] = {};
        size_t m_buf_len = 0;
        size_t m_total = 0;
    };

    // Hashes `rows` rows of `tight_row` bytes each, stepping `src_pitch` bytes between them, so
    // any padding between rows is skipped. Streams straight from the mapped pointer: no copy.
    uint64_t compute_hash64_rows(const uint8_t *data, uint32_t src_pitch, uint32_t tight_row, uint32_t rows);

    // Hashes a contiguous buffer. Identical to compute_hash64_rows over the same bytes.
    uint64_t compute_hash64(const uint8_t *data, size_t size);

    // Formats a hash as the 16-character hex string used for dump/inject filenames.
    std::string format_hash_hex(uint64_t hash);

    // CRC-32C (Castagnoli) over the same tight rows, for reading texture packs named the way
    // Special K names them. This is NOT our identity -- it is a second key we also accept, so an
    // SK pack drops into inject/ and works. SK seeds with 0 and accumulates across mip levels,
    // snapshotting after level 0; that snapshot is the value its filenames carry, so mip 0 alone
    // reproduces it.
    uint32_t compute_crc32c_rows(const uint8_t *data, uint32_t src_pitch, uint32_t tight_row, uint32_t rows);

    // 8-character hex, the width Special K uses.
    std::string format_sk_hash_hex(uint32_t hash);
}
