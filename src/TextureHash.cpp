#include "TextureHash.h"
#include <cstdio>
#include <cstring>

namespace TextureToolkit
{
    namespace
    {
        constexpr uint64_t kPrime1 = 0x9E3779B185EBCA87ull;
        constexpr uint64_t kPrime2 = 0xC2B2AE3D27D4EB4Full;

        inline uint64_t rotl64(uint64_t x, int r)
        {
            return (x << r) | (x >> (64 - r));
        }

        // Little-endian load. Windows is always little-endian, so this is stable across the
        // x86 and x64 builds and matches byte order in the file the hash names.
        inline uint64_t load64(const uint8_t *p)
        {
            uint64_t v;
            std::memcpy(&v, p, sizeof(v));
            return v;
        }

        inline uint64_t mix_word(uint64_t h, uint64_t word)
        {
            h ^= word * kPrime1;
            h = rotl64(h, 31);
            h *= kPrime2;
            return h;
        }
    }

    void Hash64::update(const uint8_t *data, size_t size)
    {
        if (data == nullptr || size == 0)
            return;

        m_total += size;

        // Top up a partial word left over from the previous call, so the result depends only on
        // the byte sequence and not on how it was split across update() calls.
        if (m_buf_len != 0)
        {
            const size_t need = 8 - m_buf_len;
            const size_t take = (size < need) ? size : need;
            std::memcpy(m_buf + m_buf_len, data, take);
            m_buf_len += take;
            data += take;
            size -= take;

            if (m_buf_len < 8)
                return;

            m_h = mix_word(m_h, load64(m_buf));
            m_buf_len = 0;
        }

        while (size >= 8)
        {
            m_h = mix_word(m_h, load64(data));
            data += 8;
            size -= 8;
        }

        if (size != 0)
        {
            std::memcpy(m_buf, data, size);
            m_buf_len = size;
        }
    }

    uint64_t Hash64::finish() const
    {
        uint64_t h = m_h;

        if (m_buf_len != 0)
        {
            uint8_t tail[8] = {};
            std::memcpy(tail, m_buf, m_buf_len);
            h = mix_word(h, load64(tail));
        }

        // Length is folded in so that trailing zero bytes cannot alias a shorter buffer.
        h ^= static_cast<uint64_t>(m_total);

        // Final avalanche, so a one-bit change in the input reaches every output bit.
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 33;
        h *= 0xC4CEB9FE1A85EC53ull;
        h ^= h >> 33;
        return h;
    }

    uint64_t compute_hash64_rows(const uint8_t *data, uint32_t src_pitch, uint32_t tight_row, uint32_t rows)
    {
        if (data == nullptr || tight_row == 0 || rows == 0)
            return 0;

        if (src_pitch < tight_row)
            src_pitch = tight_row; // a source cannot be narrower than its own content

        Hash64 h;
        for (uint32_t y = 0; y < rows; ++y)
            h.update(data + static_cast<size_t>(y) * src_pitch, tight_row);
        return h.finish();
    }

    uint64_t compute_hash64(const uint8_t *data, size_t size)
    {
        Hash64 h;
        h.update(data, size);
        return h.finish();
    }

    namespace
    {
        // Reflected CRC-32C polynomial, the one the SSE4.2 crc32 instruction implements.
        struct Crc32cTable
        {
            uint32_t v[256];
            constexpr Crc32cTable() : v()
            {
                for (uint32_t i = 0; i < 256; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
                    v[i] = c;
                }
            }
        };
        constexpr Crc32cTable kCrc32c{};

        // Matches Special K's append convention: the running value is carried un-inverted between
        // calls, so chaining chunks equals hashing the concatenation.
        inline uint32_t crc32c_append(uint32_t crc, const uint8_t *data, size_t size)
        {
            crc = ~crc;
            while (size-- != 0)
                crc = kCrc32c.v[(crc ^ *data++) & 0xFFu] ^ (crc >> 8);
            return ~crc;
        }
    }

    uint32_t compute_crc32c_rows(const uint8_t *data, uint32_t src_pitch, uint32_t tight_row, uint32_t rows)
    {
        if (data == nullptr || tight_row == 0 || rows == 0)
            return 0;

        if (src_pitch < tight_row)
            src_pitch = tight_row;

        uint32_t crc = 0;
        for (uint32_t y = 0; y < rows; ++y)
            crc = crc32c_append(crc, data + static_cast<size_t>(y) * src_pitch, tight_row);
        return crc;
    }

    std::string format_sk_hash_hex(uint32_t hash)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08X", hash);
        return std::string(buf);
    }

    std::string format_hash_hex(uint64_t hash)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(hash));
        return std::string(buf);
    }
}
