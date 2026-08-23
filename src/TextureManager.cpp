#include "TextureManager.h"
#include "Config.h"
#include "D3D9Hook.h"
#include "D3D11Hook.h"
#include "DDSLoader.h"
#include "ScopedFlag.h"
#include "Logger.h"
#include <windows.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

namespace TextureToolkit
{
    // Private-data GUID used to tag every original game resource with its content
    // hash. The driver clears private data when the object is destroyed, so a reused
    // pointer belonging to a new resource never carries a stale hash -- this is what
    // makes replacement lookups immune to driver pointer reuse.
    // {6B7A4C10-3F2E-4D9A-9E21-8C0A5B1D2E34}
    static const GUID TT_HASH_GUID =
        { 0x6b7a4c10, 0x3f2e, 0x4d9a, { 0x9e, 0x21, 0x8c, 0x0a, 0x5b, 0x1d, 0x2e, 0x34 } };

    // Per-view cache of the bind-time decision. Resolving it from scratch cost four COM calls and
    // the manager lock FOR EVERY BOUND TEXTURE, EVERY FRAME -- thousands of times a second for an
    // answer that only changes when something below bumps m_bind_generation.
    // {2F1D8A44-9C63-4E77-B0A5-7E4C11D9F326}
    static const GUID TT_SRV_CACHE_GUID =
        { 0x2f1d8a44, 0x9c63, 0x4e77, { 0xb0, 0xa5, 0x7e, 0x4c, 0x11, 0xd9, 0xf3, 0x26 } };

    struct SrvBindCache
    {
        uint32_t generation;
        uint32_t tracked;                       // 0 = this view carries no texture of ours
        uint64_t hash;
        uint64_t last_seen_frame;
        ID3D11ShaderResourceView *replacement;  // null = leave the original in place
    };

    // Hashes drawn this frame, accumulated per thread and merged in batches, so the bind hook does
    // not contend on the manager lock for every bound texture.
    static thread_local std::vector<uint64_t> s_seen_this_frame;

    static bool is_block_compressed(reshade::api::format format);
    static D3DFORMAT dxgi_to_d3d9_format(reshade::api::format format);

    // Bytes per pixel for the uncompressed formats we can box-downsample. 0 = not supported.
    static uint32_t uncompressed_bpp(reshade::api::format f)
    {
        switch (f)
        {
        case reshade::api::format::r8g8b8a8_unorm:
        case reshade::api::format::r8g8b8a8_unorm_srgb:
        case reshade::api::format::b8g8r8a8_unorm:
        case reshade::api::format::b8g8r8a8_unorm_srgb:
        case reshade::api::format::b8g8r8x8_unorm:
        case reshade::api::format::b8g8r8x8_unorm_srgb:
            return 4;
        case reshade::api::format::r8g8_unorm: return 2;
        case reshade::api::format::r8_unorm:
        case reshade::api::format::a8_unorm:  return 1;
        default: return 0;
        }
    }

    // Total GPU byte size of a texture across all its mip levels.
    static uint32_t compute_texture_bytes(reshade::api::format fmt, uint32_t w, uint32_t h, uint32_t mips)
    {
        uint32_t total = 0;
        for (uint32_t m = 0; m < (mips == 0 ? 1u : mips); ++m)
        {
            uint32_t rp = reshade::api::format_row_pitch(fmt, w);
            uint32_t sp = reshade::api::format_slice_pitch(fmt, rp, h);
            if (sp == 0)
                sp = rp * h;
            total += sp;
            w = (std::max)(1u, w / 2);
            h = (std::max)(1u, h / 2);
        }
        return total;
    }

    // Number of mip levels in a full chain down to 1x1 for the given dimensions.
    static uint32_t full_mip_count(uint32_t w, uint32_t h)
    {
        uint32_t levels = 1;
        while (w > 1 || h > 1)
        {
            w = (std::max)(1u, w / 2);
            h = (std::max)(1u, h / 2);
            ++levels;
        }
        return levels;
    }

    // 2x2 box-filter downsample of a tightly-addressed uncompressed image.
    static std::vector<uint8_t> downsample_2x(const uint8_t *src, uint32_t src_w, uint32_t src_h, uint32_t src_pitch, uint32_t bpp)
    {
        uint32_t dw = (std::max)(1u, src_w / 2);
        uint32_t dh = (std::max)(1u, src_h / 2);
        std::vector<uint8_t> dst(static_cast<size_t>(dw) * dh * bpp);

        for (uint32_t y = 0; y < dh; ++y)
        {
            uint32_t sy0 = y * 2;
            uint32_t sy1 = (std::min)(sy0 + 1, src_h - 1);
            const uint8_t *r0 = src + static_cast<size_t>(sy0) * src_pitch;
            const uint8_t *r1 = src + static_cast<size_t>(sy1) * src_pitch;
            uint8_t *drow = dst.data() + static_cast<size_t>(y) * dw * bpp;

            for (uint32_t x = 0; x < dw; ++x)
            {
                uint32_t sx0 = (x * 2) * bpp;
                uint32_t sx1 = ((std::min)(x * 2 + 1, src_w - 1)) * bpp;
                for (uint32_t c = 0; c < bpp; ++c)
                {
                    uint32_t sum = r0[sx0 + c] + r0[sx1 + c] + r1[sx0 + c] + r1[sx1 + c];
                    drow[x * bpp + c] = static_cast<uint8_t>((sum + 2) / 4);
                }
            }
        }
        return dst;
    }

    // One resolved mip level ready to upload: 'ptr' points either into the DDS payload
    // (for levels the file supplies) or into 'data' (for auto-generated levels).
    struct MipLevel
    {
        std::vector<uint8_t> data;
        const uint8_t *ptr = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t row_pitch = 0;
        uint32_t slice_pitch = 0;
    };

    // How many mip levels the replacement should be created with.
    //
    // A file that ships more than one level is taken at its word. Mip count is an authoring
    // decision, not a defect to be corrected: a UI texture drawn at 1:1 never samples below level 0
    // and pays VRAM for every level it carries, and an author who stopped a 2K texture's chain
    // early did so knowing what it costs. Clamping that to the original's level count -- which is a
    // property of the art we are replacing, not of the replacement -- would silently discard the
    // author's choice, and silently discarding it is worse than any chain they might have picked.
    //
    // A single-level file is the ambiguous case: it is equally what you get from an author who
    // meant it and from one who forgot the checkbox. Only there do we fill the chain in, and only
    // for uncompressed formats, where downsampling the source is lossless enough to be worth doing.
    // Missing levels cannot be recovered from block-compressed data at all.
    static uint32_t choose_replacement_levels(const DDSImage &dds, uint32_t original_levels)
    {
        if (dds.mip_levels > 1)
            return dds.mip_levels;

        return (original_levels == 1) ? 1u : full_mip_count(dds.width, dds.height);
    }

    // Resolve levels [0, target_levels) for a replacement of dds.width x dds.height.
    // Uses DDS subresources where present; auto-generates the rest for uncompressed
    // formats. For compressed formats missing a level, the chain stops early (a shorter
    // but fully-valid chain) rather than leaving an uninitialised level.
    static std::vector<MipLevel> build_replacement_mips(const DDSImage &dds, uint32_t target_levels)
    {
        std::vector<MipLevel> levels;
        const uint32_t bpp = uncompressed_bpp(dds.format);
        const bool compressed = is_block_compressed(dds.format);

        uint32_t w = dds.width, h = dds.height;
        for (uint32_t i = 0; i < target_levels; ++i)
        {
            MipLevel lvl;
            lvl.width = w;
            lvl.height = h;

            if (i < dds.subresources.size())
            {
                lvl.ptr = dds.subresources[i].data();
                lvl.row_pitch = dds.row_pitches[i];
                lvl.slice_pitch = dds.slice_pitches[i];
            }
            else
            {
                // Auto-generate this level by downsampling the previous one.
                if (compressed || bpp == 0 || levels.empty())
                    break; // Cannot synthesise; keep the shorter valid chain.

                const MipLevel &prev = levels.back();
                lvl.data = downsample_2x(prev.ptr, prev.width, prev.height, prev.row_pitch, bpp);
                lvl.ptr = lvl.data.data();
                lvl.row_pitch = w * bpp;
                lvl.slice_pitch = static_cast<uint32_t>(lvl.data.size());
            }

            levels.push_back(std::move(lvl));
            w = (std::max)(1u, w / 2);
            h = (std::max)(1u, h / 2);
        }
        return levels;
    }

    // Copies one mip level into a tightly-packed buffer, dropping any row padding the source had.
    static void append_tight_level(std::vector<std::vector<uint8_t>> &out, const void *src, UINT src_pitch,
                                   reshade::api::format fmt, bool compressed, UINT w, UINT h)
    {
        if (src == nullptr || w == 0 || h == 0)
            return;

        const UINT tight_row = reshade::api::format_row_pitch(fmt, w);
        const UINT rows = compressed ? ((h + 3) / 4) : h;
        if (tight_row == 0 || rows == 0)
            return;

        if (src_pitch < tight_row)
            src_pitch = tight_row;

        std::vector<uint8_t> buf(static_cast<size_t>(tight_row) * rows);
        const uint8_t *s = static_cast<const uint8_t *>(src);
        for (UINT y = 0; y < rows; ++y)
            std::memcpy(buf.data() + static_cast<size_t>(y) * tight_row,
                        s + static_cast<size_t>(y) * src_pitch, tight_row);
        out.push_back(std::move(buf));
    }

    // Bytes per pixel for uncompressed D3D9 formats. Returns 0 for anything we do not positively
    // recognise, and the caller then SKIPS the texture instead of guessing.
    //
    // This used to default to 4. That was both a correctness bug and a memory-safety one: for a
    // 1- or 2-byte format (A8L8, A4L4, R3G3B2, X4R4G4B4, V8U8, R16F...) the computed row is 2-4x
    // the real row, so hashing walked off the end of the locked buffer. It also means the set of
    // formats we recognise is part of the compatibility contract: adding a format here later only
    // makes NEW textures moddable, it never changes a hash that already exists.
    static UINT d3d9_bytes_per_pixel(D3DFORMAT format)
    {
        switch (static_cast<uint32_t>(format))
        {
        case D3DFMT_R8G8B8:            return 3;
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        case D3DFMT_A2B10G10R10:
        case D3DFMT_A8B8G8R8:
        case D3DFMT_X8B8G8R8:
        case D3DFMT_G16R16:
        case D3DFMT_A2R10G10B10:
        case D3DFMT_X8L8V8U8:
        case D3DFMT_Q8W8V8U8:
        case D3DFMT_V16U16:
        case D3DFMT_A2W10V10U10:
        case D3DFMT_G16R16F:
        case D3DFMT_R32F:              return 4;
        case D3DFMT_R5G6B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
        case D3DFMT_A4R4G4B4:
        case D3DFMT_A8R3G3B2:
        case D3DFMT_X4R4G4B4:
        case D3DFMT_A8P8:
        case D3DFMT_A8L8:
        case D3DFMT_V8U8:
        case D3DFMT_L6V5U5:
        case D3DFMT_L16:
        case D3DFMT_R16F:
        case D3DFMT_CxV8U8:            return 2;
        case D3DFMT_R3G3B2:
        case D3DFMT_A8:
        case D3DFMT_P8:
        case D3DFMT_L8:
        case D3DFMT_A4L4:              return 1;
        case D3DFMT_A16B16G16R16:
        case D3DFMT_A16B16G16R16F:
        case D3DFMT_G32R32F:           return 8;
        case D3DFMT_A32B32G32R32F:     return 16;
        default:                       return 0; // unknown: do not guess
        }
    }

    static uint64_t calculate_d3d9_pixel_hash(const void *pixel_data, UINT width, UINT height, D3DFORMAT format, UINT pitch)
    {
        if (pixel_data == nullptr || width == 0 || height == 0)
            return 0;

        const uint8_t *src = static_cast<const uint8_t *>(pixel_data);

        // Hash tight rows only, so the lock pitch the driver happened to hand us never reaches
        // the hash (see TextureHash.h). Block-compressed formats step a row of 4-pixel blocks.
        const uint32_t fmt4cc = static_cast<uint32_t>(format);
        const bool is_bc = (format == D3DFMT_DXT1 || format == D3DFMT_DXT2 || format == D3DFMT_DXT3 ||
                            format == D3DFMT_DXT4 || format == D3DFMT_DXT5 ||
                            fmt4cc == MAKEFOURCC('A','T','I','1') || fmt4cc == MAKEFOURCC('B','C','4','U') ||
                            fmt4cc == MAKEFOURCC('A','T','I','2') || fmt4cc == MAKEFOURCC('B','C','5','U'));
        if (is_bc)
        {
            // 8 bytes per 4x4 block for the one-channel/1-bit-alpha formats, 16 for the rest.
            const bool small_block = (format == D3DFMT_DXT1 ||
                                      fmt4cc == MAKEFOURCC('A','T','I','1') || fmt4cc == MAKEFOURCC('B','C','4','U'));
            const UINT block_size = small_block ? 8 : 16;
            const UINT tight_row = ((width + 3) / 4) * block_size;
            const UINT rows = (height + 3) / 4;
            return compute_hash64_rows(src, pitch, tight_row, rows);
        }

        const UINT bpp = d3d9_bytes_per_pixel(format);
        if (bpp == 0)
        {
            static int s_logged_unknown = 0;
            if (s_logged_unknown < 8)
            {
                ++s_logged_unknown;
                Logger::get().warn("[TextureManager] Skipping D3D9 texture in unrecognised format " +
                                   std::to_string(static_cast<uint32_t>(format)) +
                                   "; its pixel layout is unknown, so it cannot be hashed safely.");
            }
            return 0;
        }

        return compute_hash64_rows(src, pitch, width * bpp, height);
    }

    // Special K's name for the same D3D9 texture: CRC-32C over exactly the rows our own hash walks.
    // Kept beside calculate_d3d9_pixel_hash so the two can never disagree about which bytes count.
    static uint32_t calculate_d3d9_sk_hash(const void *pixel_data, UINT width, UINT height, D3DFORMAT format, UINT pitch)
    {
        if (pixel_data == nullptr || width == 0 || height == 0)
            return 0;

        const uint8_t *src = static_cast<const uint8_t *>(pixel_data);
        const uint32_t fmt4cc = static_cast<uint32_t>(format);
        const bool is_bc = (format == D3DFMT_DXT1 || format == D3DFMT_DXT2 || format == D3DFMT_DXT3 ||
                            format == D3DFMT_DXT4 || format == D3DFMT_DXT5 ||
                            fmt4cc == MAKEFOURCC('A','T','I','1') || fmt4cc == MAKEFOURCC('B','C','4','U') ||
                            fmt4cc == MAKEFOURCC('A','T','I','2') || fmt4cc == MAKEFOURCC('B','C','5','U'));

        if (is_bc)
        {
            const bool small_block = (format == D3DFMT_DXT1 ||
                                      fmt4cc == MAKEFOURCC('A','T','I','1') || fmt4cc == MAKEFOURCC('B','C','4','U'));
            const UINT block_size = small_block ? 8 : 16;
            return compute_crc32c_rows(src, pitch, ((width + 3) / 4) * block_size, (height + 3) / 4);
        }

        const UINT bpp = d3d9_bytes_per_pixel(format);
        if (bpp == 0)
            return 0;

        return compute_crc32c_rows(src, pitch, width * bpp, height);
    }

    // Human-readable DXGI_FORMAT name. Covers the formats games actually ship textures in;
    // anything else falls back to the numeric id.
    static std::string dxgi_format_name(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:        return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return "R8G8B8A8_TYPELESS";
        case DXGI_FORMAT_B8G8R8A8_UNORM:        return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8X8_UNORM:        return "B8G8R8X8_UNORM";
        case DXGI_FORMAT_R10G10B10A2_UNORM:     return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R8_UNORM:              return "R8_UNORM";
        case DXGI_FORMAT_R8G8_UNORM:            return "R8G8_UNORM";
        case DXGI_FORMAT_A8_UNORM:              return "A8_UNORM";
        case DXGI_FORMAT_BC1_UNORM:             return "BC1_UNORM";
        case DXGI_FORMAT_BC1_UNORM_SRGB:        return "BC1_UNORM_SRGB";
        case DXGI_FORMAT_BC1_TYPELESS:          return "BC1_TYPELESS";
        case DXGI_FORMAT_BC2_UNORM:             return "BC2_UNORM";
        case DXGI_FORMAT_BC2_UNORM_SRGB:        return "BC2_UNORM_SRGB";
        case DXGI_FORMAT_BC3_UNORM:             return "BC3_UNORM";
        case DXGI_FORMAT_BC3_UNORM_SRGB:        return "BC3_UNORM_SRGB";
        case DXGI_FORMAT_BC3_TYPELESS:          return "BC3_TYPELESS";
        case DXGI_FORMAT_BC4_UNORM:             return "BC4_UNORM";
        case DXGI_FORMAT_BC4_SNORM:             return "BC4_SNORM";
        case DXGI_FORMAT_BC5_UNORM:             return "BC5_UNORM";
        case DXGI_FORMAT_BC5_SNORM:             return "BC5_SNORM";
        case DXGI_FORMAT_BC6H_UF16:             return "BC6H_UF16";
        case DXGI_FORMAT_BC6H_SF16:             return "BC6H_SF16";
        case DXGI_FORMAT_BC7_UNORM:             return "BC7_UNORM";
        case DXGI_FORMAT_BC7_UNORM_SRGB:        return "BC7_UNORM_SRGB";
        case DXGI_FORMAT_BC7_TYPELESS:          return "BC7_TYPELESS";
        default:                                return std::to_string(static_cast<uint32_t>(f));
        }
    }

    static bool dxgi_format_is_compressed(DXGI_FORMAT f)
    {
        return f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM
            || f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB;
    }

    // TYPELESS formats cannot back a shader resource view, and most DDS tools cannot read
    // them. Map them to the matching UNORM view format. Concrete formats, including the
    // _SRGB variants, pass through unchanged so sRGB intent is preserved.
    static DXGI_FORMAT dxgi_concrete_format(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_BC1_TYPELESS:          return DXGI_FORMAT_BC1_UNORM;
        case DXGI_FORMAT_BC2_TYPELESS:          return DXGI_FORMAT_BC2_UNORM;
        case DXGI_FORMAT_BC3_TYPELESS:          return DXGI_FORMAT_BC3_UNORM;
        case DXGI_FORMAT_BC4_TYPELESS:          return DXGI_FORMAT_BC4_UNORM;
        case DXGI_FORMAT_BC5_TYPELESS:          return DXGI_FORMAT_BC5_UNORM;
        case DXGI_FORMAT_BC6H_TYPELESS:         return DXGI_FORMAT_BC6H_UF16;
        case DXGI_FORMAT_BC7_TYPELESS:          return DXGI_FORMAT_BC7_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R8G8_TYPELESS:         return DXGI_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8_TYPELESS:           return DXGI_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_UNORM;
        case DXGI_FORMAT_R16_TYPELESS:          return DXGI_FORMAT_R16_UNORM;
        default:                                return f;
        }
    }

    static bool dxgi_format_is_srgb(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
        }
    }

    TextureManager &TextureManager::get()
    {
        static TextureManager instance;
        return instance;
    }

    void TextureManager::init()
    {
        wchar_t exe_path[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exe_path, ARRAYSIZE(exe_path));

        m_game_dir = std::filesystem::path(exe_path).parent_path();

        // Load settings from config
        const auto& cfg = ConfigManager::get().get_config();

        // Resolve dump/inject under the resource root. A relative root (the default, "TT") is
        // taken relative to the game directory; an absolute root is used as-is (operator/
        // returns the right-hand path when it is absolute).
        std::filesystem::path root = m_game_dir / cfg.resource_root;
        m_dump_dir = root / "dump";
        m_inject_dir = root / "inject";

        auto_dump = cfg.auto_dump;
        enable_injection = cfg.enable_injection;
        filter_small_textures = cfg.filter_small_textures;
        show_current_frame_only = cfg.show_current_frame_only;
        accept_sk_names = cfg.accept_sk_names;

        std::error_code ec;
        std::filesystem::create_directories(m_dump_dir, ec);
        std::filesystem::create_directories(m_inject_dir, ec);

        Logger::get().info("[TextureManager] Standalone Texture Toolkit initialized.");
        Logger::get().info("[TextureManager] Dump directory: " + m_dump_dir.string());
        Logger::get().info("[TextureManager] Inject directory: " + m_inject_dir.string());

        rescan_injected();

        // Start background dump worker
        m_dump_thread_running = true;
        m_dump_thread = std::thread(&TextureManager::dump_worker_loop, this);
    }

    void TextureManager::shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(m_dump_mutex);
            m_dump_thread_running = false;
        }
        m_dump_cv.notify_one();
        if (m_dump_thread.joinable())
        {
            m_dump_thread.join();
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_bind_generation.fetch_add(1, std::memory_order_relaxed);
        release_replacements();
        for (IUnknown *p : m_retired_replacements)
            p->Release();
        m_retired_replacements.clear();
        release_preview();

        for (auto &rb : m_readback_queue)
        {
            if (rb.tex9) rb.tex9->Release();
            if (rb.srv11) rb.srv11->Release();
        }
        m_readback_queue.clear();
    }

    void TextureManager::set_preview_target(uint64_t hash)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (hash == m_preview_target_hash)
            return;
        release_preview();
        m_preview_target_hash = hash;
        m_bind_generation.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t TextureManager::get_original_preview_handle()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_preview_srv11 != nullptr)
            return reinterpret_cast<uint64_t>(m_preview_srv11);
        if (m_preview_tex9 != nullptr)
            return reinterpret_cast<uint64_t>(m_preview_tex9);
        return 0;
    }

    // Releases the pinned original preview and the cached file preview. Caller MUST hold m_mutex.
    void TextureManager::release_preview()
    {
        if (m_preview_tex9 != nullptr)
        {
            m_preview_tex9->Release();
            m_preview_tex9 = nullptr;
        }
        if (m_preview_srv11 != nullptr)
        {
            m_preview_srv11->Release();
            m_preview_srv11 = nullptr;
        }
        if (m_file_preview_tex9 != nullptr)
        {
            m_file_preview_tex9->Release();
            m_file_preview_tex9 = nullptr;
        }
        if (m_file_preview_srv11 != nullptr)
        {
            m_file_preview_srv11->Release();
            m_file_preview_srv11 = nullptr;
        }
        m_file_preview_hash = 0;
    }

    uint64_t TextureManager::get_file_preview_handle(uint64_t hash, const std::string &dds_path, bool is_dx11)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (hash == m_file_preview_hash)
            return m_file_preview_srv11 ? reinterpret_cast<uint64_t>(m_file_preview_srv11)
                                        : reinterpret_cast<uint64_t>(m_file_preview_tex9);

        // Drop any previously cached file preview.
        if (m_file_preview_tex9)  { m_file_preview_tex9->Release();  m_file_preview_tex9 = nullptr; }
        if (m_file_preview_srv11) { m_file_preview_srv11->Release(); m_file_preview_srv11 = nullptr; }
        m_file_preview_hash = hash;

        DDSImage dds;
        if (!load_dds(dds_path, dds) || dds.subresources.empty())
            return 0;

        if (is_dx11)
        {
            ID3D11Device *dev = D3D11Hook::get().get_device();
            if (dev == nullptr)
                return 0;

            DXGI_FORMAT view_fmt = dxgi_concrete_format(static_cast<DXGI_FORMAT>(dds.format));

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = dds.width;
            desc.Height = dds.height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = view_fmt;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA sd = {};
            sd.pSysMem = dds.subresources[0].data();
            sd.SysMemPitch = dds.row_pitches[0];
            sd.SysMemSlicePitch = dds.slice_pitches[0];

            ID3D11Texture2D *tex = nullptr;
            ScopedFlag no_reentry(D3D11Hook::s_inside_injection);
            HRESULT hr = dev->CreateTexture2D(&desc, &sd, &tex);
            if (SUCCEEDED(hr) && tex != nullptr)
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
                svd.Format = desc.Format;
                svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                svd.Texture2D.MipLevels = 1;
                hr = dev->CreateShaderResourceView(tex, &svd, &m_file_preview_srv11);
                tex->Release();
            }
            return reinterpret_cast<uint64_t>(m_file_preview_srv11);
        }

        // DX9
        IDirect3DDevice9 *dev = D3D9Hook::get().get_device();
        if (dev == nullptr)
            return 0;

        D3DFORMAT fmt = dxgi_to_d3d9_format(dds.format);
        if (fmt == D3DFMT_UNKNOWN)
            return 0;

        IDirect3DTexture9 *tex = nullptr;
        ScopedFlag no_reentry(D3D9Hook::s_inside_injection);
        if (SUCCEEDED(dev->CreateTexture(dds.width, dds.height, 1, 0, fmt, D3DPOOL_MANAGED, &tex, nullptr)) && tex != nullptr)
        {
            D3DLOCKED_RECT r = {};
            if (SUCCEEDED(tex->LockRect(0, &r, nullptr, 0)))
            {
                bool needs_swizzle = (dds.format == reshade::api::format::r8g8b8a8_unorm || dds.format == reshade::api::format::r8g8b8a8_unorm_srgb);
                UINT block_h = is_block_compressed(dds.format) ? 4 : 1;
                UINT rows = (dds.height + block_h - 1) / block_h;
                UINT src_pitch = dds.row_pitches[0];
                const uint8_t *src = dds.subresources[0].data();
                uint8_t *dst = static_cast<uint8_t *>(r.pBits);

                for (UINT y = 0; y < rows; ++y)
                {
                    if (needs_swizzle)
                    {
                        const uint8_t *sr = src + y * src_pitch;
                        uint8_t *dr = dst + y * r.Pitch;
                        for (UINT x = 0; x < dds.width; ++x)
                        {
                            dr[x * 4 + 0] = sr[x * 4 + 2];
                            dr[x * 4 + 1] = sr[x * 4 + 1];
                            dr[x * 4 + 2] = sr[x * 4 + 0];
                            dr[x * 4 + 3] = sr[x * 4 + 3];
                        }
                    }
                    else
                    {
                        std::memcpy(dst + y * r.Pitch, src + y * src_pitch, (std::min)(static_cast<UINT>(r.Pitch), src_pitch));
                    }
                }
                tex->UnlockRect(0);
                m_file_preview_tex9 = tex;
            }
            else
            {
                tex->Release();
            }
        }
        return reinterpret_cast<uint64_t>(m_file_preview_tex9);
    }

    // Releases the COM reference we hold for every stored replacement. Caller MUST
    // already hold m_mutex (the mutex is non-recursive).
    void TextureManager::release_replacements(bool defer)
    {
        for (auto &p : m_d3d9_replacements)
        {
            if (p.second == nullptr)
                continue;
            if (defer)
                m_retired_replacements.push_back(p.second);
            else
                p.second->Release();
        }
        m_d3d9_replacements.clear();

        for (auto &p : m_d3d11_replacements)
        {
            if (p.second == nullptr)
                continue;
            if (defer)
                m_retired_replacements.push_back(p.second);
            else
                p.second->Release();
        }
        m_d3d11_replacements.clear();

        if (defer)
            m_retire_after_frame = m_frame_count + 2;
    }

    // Caller MUST hold m_mutex.
    void TextureManager::drain_retired_replacements()
    {
        if (m_retired_replacements.empty() || m_frame_count < m_retire_after_frame)
            return;

        for (IUnknown *p : m_retired_replacements)
            p->Release();
        m_retired_replacements.clear();
        m_retire_after_frame = 0;
    }

    // Caller MUST hold m_mutex.
    void TextureManager::flush_seen_locked()
    {
        for (uint64_t hash : s_seen_this_frame)
            m_current_frame_hashes.insert(hash);
        s_seen_this_frame.clear();
    }

    void TextureManager::on_frame()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        flush_seen_locked();
        m_frame_count++;
        m_frame_count_atomic.store(m_frame_count, std::memory_order_relaxed);
        const uint64_t now_ticks = GetTickCount64();

        m_active_frame_hashes = m_current_frame_hashes;
        m_current_frame_hashes.clear();

        for (uint64_t hash : m_active_frame_hashes)
        {
            auto it = m_tracked_textures.find(hash);
            if (it != m_tracked_textures.end())
            {
                it->second.last_seen_frame = m_frame_count;
                it->second.last_seen_ticks = now_ticks;
            }
        }

        // Trim the tracked list on a timer, not a frame count: both the interval and the age below
        // are meant in seconds, and a frame count sweeps eight times as often at 240 fps as at 30.
        if (now_ticks >= m_next_eviction_ticks)
        {
            m_next_eviction_ticks = now_ticks + 5000; // every 5 seconds
            evict_stale_textures(now_ticks);
        }

        drain_retired_replacements();
        process_pending_injections();
        process_readback_queue();
    }

    void TextureManager::evict_stale_textures(uint64_t now_ticks)
    {
        // Remove textures not drawn for a while. Keep anything with a loaded replacement, the
        // texture currently selected for preview, and any hash that has an inject file, so injected
        // and selected textures never disappear from the panel.
        constexpr uint64_t kEvictAgeMs = 60 * 1000; // one minute, at any framerate
        if (now_ticks < kEvictAgeMs)
            return;

        for (auto it = m_tracked_textures.begin(); it != m_tracked_textures.end();)
        {
            const TextureDetails &d = it->second;
            const bool stale = d.last_seen_ticks + kEvictAgeMs < now_ticks;
            // A texture with an inject file waiting for it stays in the list under either naming;
            // dropping the SK-named ones would make an SK pack's pending entries disappear.
            const bool has_inject_file =
                m_injected_files.find(it->first) != m_injected_files.end() ||
                (accept_sk_names && d.sk_hash != 0 &&
                 m_sk_injected_files.find(d.sk_hash) != m_sk_injected_files.end());

            const bool keep = d.replacement_handle != 0 ||
                              it->first == m_preview_target_hash ||
                              has_inject_file;

            if (stale && !keep)
                it = m_tracked_textures.erase(it);
            else
                ++it;
        }
    }

    void TextureManager::rescan_injected()
    {
        // Walk the directory WITHOUT the manager lock. The bind hooks take that lock on the render
        // thread, so scanning a slow disk (or a resource root on a network share) while holding it
        // stalls texture tracking for as long as the scan takes. Build the new map first, then swap.
        std::unordered_map<uint64_t, std::filesystem::path> found;
        std::unordered_map<uint32_t, std::filesystem::path> found_sk;

        std::error_code scan_ec;
        if (std::filesystem::exists(m_inject_dir, scan_ec) && !scan_ec)
        {
            for (std::filesystem::directory_iterator it(m_inject_dir, scan_ec), end_it; it != end_it && !scan_ec; it.increment(scan_ec))
            {
                const std::filesystem::directory_entry &entry = *it;
                if (!entry.is_regular_file(scan_ec) || scan_ec)
                    continue;

                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext != ".dds") // DDS-only injection, for format safety
                    continue;

                std::string stem = entry.path().stem().string();
                const bool prefixed = (stem.rfind("0x", 0) == 0 || stem.rfind("0X", 0) == 0);
                if (prefixed)
                    stem = stem.substr(2);

                // Special K names a pack <topCRC>.dds or <topCRC>_<fullCRC>.dds, optionally
                // prefixed "Uncompressed_" and/or suffixed "_TYPELESS". We key on the top-LOD CRC,
                // which is the part we can reproduce, and ignore the rest of the name.
                if (stem.size() != 16)
                {
                    std::string sk = stem;
                    if (sk.rfind("Uncompressed_", 0) == 0)
                        sk = sk.substr(13);
                    const size_t underscore = sk.find('_');
                    if (underscore != std::string::npos)
                        sk = sk.substr(0, underscore);

                    if (sk.size() == 8 && sk.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
                    {
                        try
                        {
                            found_sk.emplace(static_cast<uint32_t>(std::stoul(sk, nullptr, 16)), entry.path());
                        }
                        catch (...)
                        {
                        }
                        continue;
                    }
                }

                try
                {
                    const uint64_t hash = std::stoull(stem, nullptr, 16);
                    // Two files naming one hash must resolve the same way every run, not by
                    // directory order: the unprefixed spelling wins.
                    auto existing = found.find(hash);
                    if (existing == found.end())
                        found.emplace(hash, entry.path());
                    else if (!prefixed)
                        existing->second = entry.path();
                }
                catch (...)
                {
                    // Ignore non-hex filenames
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // Bump first: a bind that has already passed the generation check must not go on to
            // read a pointer we are about to drop. Raising it here sends every later bind down the
            // slow path, which takes this lock and so waits for the swap below to finish.
            m_bind_generation.fetch_add(1, std::memory_order_relaxed);
            release_replacements(true);

            m_injected_files.swap(found);
            m_sk_injected_files.swap(found_sk);
            m_failed_injections.clear(); // retry files that were bad last time; they may be fixed now
            m_pending_injections.clear();

            // Replacements are rebuilt after the next time each texture is drawn (flagged by
            // note_pending_injection, built by process_pending_injections), so newly added DDS
            // files apply without a restart.
            for (auto &pair : m_tracked_textures)
            {
                pair.second.replacement_handle = 0;
                pair.second.repl_width = 0;
                pair.second.repl_height = 0;
                pair.second.filepath_injected.clear();
                if (pair.second.status == TextureStatus::INJECTED)
                    pair.second.status = TextureStatus::ORIGINAL;
            }

            Logger::get().info("[TextureManager] Scanned " + std::to_string(m_injected_files.size()) + " DDS replacement file(s) in TT/inject.");
            if (!m_sk_injected_files.empty())
                Logger::get().info("[TextureManager] Also found " + std::to_string(m_sk_injected_files.size()) +
                                   " Special K-named file(s); these match on the top-mip CRC-32C.");
        }
    }

    std::filesystem::path TextureManager::find_injection_path(uint64_t hash, uint32_t sk_hash, bool *via_sk_name)
    {
        if (via_sk_name != nullptr)
            *via_sk_name = false;

        auto it = m_injected_files.find(hash);
        if (it != m_injected_files.end())
            return it->second;

        // Fall back to Special K's naming, so an SK texture pack works without being renamed.
        if (accept_sk_names && sk_hash != 0)
        {
            auto sit = m_sk_injected_files.find(sk_hash);
            if (sit != m_sk_injected_files.end())
            {
                if (via_sk_name != nullptr)
                    *via_sk_name = true;
                return sit->second;
            }
        }

        return std::filesystem::path();
    }

    void TextureManager::copy_tag9(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst)
    {
        if (src == nullptr || dst == nullptr)
            return;

        uint64_t hash = 0;
        DWORD size = sizeof(hash);
        if (SUCCEEDED(src->GetPrivateData(TT_HASH_GUID, &hash, &size)) && size == sizeof(hash))
            dst->SetPrivateData(TT_HASH_GUID, &hash, sizeof(hash), 0);
    }

    IDirect3DBaseTexture9 *TextureManager::get_replacement_texture9(IDirect3DBaseTexture9 *orig)
    {
        if (orig == nullptr)
            return orig;

        // Resolve the texture's content hash from its private-data tag. Untracked
        // textures carry no tag, so this fails fast for the common case.
        uint64_t hash = 0;
        DWORD size = sizeof(hash);
        if (FAILED(orig->GetPrivateData(TT_HASH_GUID, &hash, &size)) || size != sizeof(hash))
            return orig;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_current_frame_hashes.insert(hash);

        // Pin the live original for preview if this is the selected texture.
        if (hash == m_preview_target_hash && m_preview_tex9 == nullptr)
        {
            orig->AddRef();
            m_preview_tex9 = orig;
        }

        // Bulk dump: take a reference the first time a queued texture is drawn.
        if (!m_pending_dumps.empty())
        {
            auto pit = m_pending_dumps.find(hash);
            if (pit != m_pending_dumps.end())
            {
                m_pending_dumps.erase(pit);
                orig->AddRef();
                m_readback_queue.push_back({hash, orig, nullptr});
            }
        }

        // Replacements are 2D textures. Never hand a cube or volume texture slot a 2D texture
        // (mirrors the view-dimension guard on the D3D11 side).
        if (!enable_injection || orig->GetType() != D3DRTYPE_TEXTURE)
            return orig;

        auto it = m_d3d9_replacements.find(hash);
        if (it == m_d3d9_replacements.end())
        {
            // Hot reload: a DDS added after this texture was uploaded has no replacement yet, and
            // the game will not upload it again. Only flag it here; the build happens in on_frame,
            // never inside this draw call (see process_pending_injections).
            note_pending_injection(hash, false);
        }
        if (it != m_d3d9_replacements.end() && it->second != nullptr)
            return it->second;

        return orig;
    }

    // Flags a drawn texture that has an inject file but no live replacement yet. Cheap: this runs
    // inside the game's draw call, so it only records the hash. Caller MUST hold m_mutex.
    void TextureManager::note_pending_injection(uint64_t hash, bool is_dx11)
    {
        if (!enable_injection)
            return;
        if (m_failed_injections.find(hash) != m_failed_injections.end())
            return;
        if (m_injected_files.find(hash) == m_injected_files.end())
            return;

        m_pending_injections[hash] = is_dx11;
    }

    // Builds replacements flagged by note_pending_injection. Runs once per frame from on_frame,
    // outside any draw call, and only a couple per frame: each one reads a DDS off disk and
    // creates a GPU texture, which must never happen in the middle of the game's rendering.
    // Caller MUST hold m_mutex.
    void TextureManager::process_pending_injections()
    {
        if (m_pending_injections.empty())
            return;

        int budget = 2;
        while (!m_pending_injections.empty() && budget-- > 0)
        {
            auto pit = m_pending_injections.begin();
            const uint64_t hash = pit->first;
            const bool is_dx11 = pit->second;
            m_pending_injections.erase(pit);

            auto fit = m_injected_files.find(hash);
            auto tit = m_tracked_textures.find(hash);
            if (fit == m_injected_files.end() || tit == m_tracked_textures.end())
                continue;

            const bool have = is_dx11
                ? m_d3d11_replacements.find(hash) != m_d3d11_replacements.end()
                : m_d3d9_replacements.find(hash) != m_d3d9_replacements.end();
            if (have)
                continue;

            // No device yet is a "not now", not a "never": leave the flag off and let the next
            // draw re-raise it, instead of blacklisting a file that was never actually tried.
            ID3D11Device *dev11 = is_dx11 ? D3D11Hook::get().get_device() : nullptr;
            IDirect3DDevice9 *dev9 = is_dx11 ? nullptr : D3D9Hook::get().get_device();
            if (is_dx11 ? (dev11 == nullptr) : (dev9 == nullptr))
                continue;

            TextureDetails &details = tit->second;
            const bool ok = is_dx11
                ? build_replacement11(dev11, hash, fit->second, details.mip_levels, details)
                : build_replacement9(dev9, hash, fit->second, details.mip_levels, details);

            if (!ok)
                m_failed_injections.insert(hash); // do not retry a broken file every frame
        }
    }

    static DXGI_FORMAT d3d9_format_to_dxgi(D3DFORMAT format)
    {
        switch (static_cast<uint32_t>(format))
        {
        case D3DFMT_A8R8G8B8: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case D3DFMT_X8R8G8B8: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case D3DFMT_A1R5G5B5: return DXGI_FORMAT_B5G5R5A1_UNORM;
        case D3DFMT_R5G6B5:   return DXGI_FORMAT_B5G6R5_UNORM;
        case D3DFMT_A8:       return DXGI_FORMAT_A8_UNORM;
        case D3DFMT_L8:       return DXGI_FORMAT_R8_UNORM;
        case D3DFMT_A8L8:     return DXGI_FORMAT_R8G8_UNORM;
        case D3DFMT_DXT1:     return DXGI_FORMAT_BC1_UNORM;
        case D3DFMT_DXT2:
        case D3DFMT_DXT3:     return DXGI_FORMAT_BC2_UNORM;
        case D3DFMT_DXT4:
        case D3DFMT_DXT5:     return DXGI_FORMAT_BC3_UNORM;
        default:              return DXGI_FORMAT_UNKNOWN;
        }
    }

    static std::string d3d9_format_to_string(D3DFORMAT format)
    {
        switch (static_cast<uint32_t>(format))
        {
        case D3DFMT_A8R8G8B8: return "A8R8G8B8";
        case D3DFMT_X8R8G8B8: return "X8R8G8B8";
        case D3DFMT_R5G6B5:   return "R5G6B5";
        case D3DFMT_A8:       return "A8";
        case D3DFMT_L8:       return "L8";
        case D3DFMT_DXT1:     return "DXT1";
        case D3DFMT_DXT3:     return "DXT3";
        case D3DFMT_DXT5:     return "DXT5";
        default:              return "D3DFMT_" + std::to_string(static_cast<uint32_t>(format));
        }
    }

    static bool is_block_compressed(reshade::api::format format)
    {
        switch (format)
        {
        case reshade::api::format::bc1_unorm:
        case reshade::api::format::bc1_unorm_srgb:
        case reshade::api::format::bc2_unorm:
        case reshade::api::format::bc2_unorm_srgb:
        case reshade::api::format::bc3_unorm:
        case reshade::api::format::bc3_unorm_srgb:
        case reshade::api::format::bc4_unorm:
        case reshade::api::format::bc4_snorm:
        case reshade::api::format::bc5_unorm:
        case reshade::api::format::bc5_snorm:
        case reshade::api::format::bc6h_ufloat:
        case reshade::api::format::bc6h_sfloat:
        case reshade::api::format::bc7_unorm:
        case reshade::api::format::bc7_unorm_srgb:
            return true;
        default:
            return false;
        }
    }

    static D3DFORMAT dxgi_to_d3d9_format(reshade::api::format format)
    {
        switch (format)
        {
        case reshade::api::format::b8g8r8a8_unorm: 
        case reshade::api::format::b8g8r8a8_unorm_srgb: return D3DFMT_A8R8G8B8;
        case reshade::api::format::b8g8r8x8_unorm: 
        case reshade::api::format::b8g8r8x8_unorm_srgb: return D3DFMT_X8R8G8B8;
        case reshade::api::format::r8g8b8a8_unorm: 
        case reshade::api::format::r8g8b8a8_unorm_srgb: return D3DFMT_A8R8G8B8; // Map to A8R8G8B8 and swizzle during copy
        case reshade::api::format::b5g6r5_unorm:   return D3DFMT_R5G6B5;
        case reshade::api::format::b5g5r5a1_unorm: return D3DFMT_A1R5G5B5;
        case reshade::api::format::a8_unorm:       return D3DFMT_A8;
        case reshade::api::format::r8_unorm:       return D3DFMT_L8;
        case reshade::api::format::r8g8_unorm:     return D3DFMT_A8L8;
        case reshade::api::format::bc1_unorm:      
        case reshade::api::format::bc1_unorm_srgb:  return D3DFMT_DXT1;
        case reshade::api::format::bc2_unorm:      
        case reshade::api::format::bc2_unorm_srgb:  return D3DFMT_DXT3;
        case reshade::api::format::bc3_unorm:      
        case reshade::api::format::bc3_unorm_srgb:  return D3DFMT_DXT5;
        default:                                   return D3DFMT_UNKNOWN;
        }
    }

    void TextureManager::register_unmap_texture9(IDirect3DDevice9 *device, IDirect3DTexture9 *texture, const void *pixel_data, UINT width, UINT height, D3DFORMAT format, UINT pitch)
    {
        if (device == nullptr || texture == nullptr || pixel_data == nullptr || width == 0 || height == 0)
            return;

        if (filter_small_textures && (width < 16 || height < 16))
            return;

        uint64_t hash = calculate_d3d9_pixel_hash(pixel_data, width, height, format, pitch);
        if (hash == 0)
            return;

        const uint32_t sk_hash = accept_sk_names
            ? calculate_d3d9_sk_hash(pixel_data, width, height, format, pitch)
            : 0u;

        UINT original_levels = texture->GetLevelCount();

        std::lock_guard<std::mutex> lock(m_mutex);

        // Tag the original resource with its content hash. Used both for active-scene
        // tracking and for resolving replacements at bind time (SetTexture), and is
        // immune to driver pointer reuse.
        texture->SetPrivateData(TT_HASH_GUID, &hash, sizeof(hash), 0);

        bool dx9_compressed = (format == D3DFMT_DXT1 || format == D3DFMT_DXT2 || format == D3DFMT_DXT3 ||
                               format == D3DFMT_DXT4 || format == D3DFMT_DXT5);

        TextureDetails details;
        details.hash = hash;
        details.hash_hex = format_hash_hex(hash);
        details.sk_hash = sk_hash;
        details.width = width;
        details.height = height;
        details.mip_levels = original_levels;
        details.format_id = static_cast<uint32_t>(format);
        details.format_str = "D3DFMT_" + d3d9_format_to_string(format);
        details.format_short = "D3D9_" + d3d9_format_to_string(format);
        details.is_compressed = dx9_compressed;
        details.is_srgb = false; // D3D9 sRGB is a sampler state, not part of the format
        details.is_dx11 = false;
        {
            DXGI_FORMAT ddx = d3d9_format_to_dxgi(format);
            details.data_size = (ddx != DXGI_FORMAT_UNKNOWN)
                ? compute_texture_bytes(static_cast<reshade::api::format>(ddx), width, height, original_levels)
                : width * height * 4;
        }
        details.last_seen_frame = m_frame_count;
        details.last_seen_ticks = GetTickCount64();

        bool via_sk_name = false;
        std::filesystem::path inject_path = find_injection_path(hash, sk_hash, &via_sk_name);
        details.injected_via_sk_name = via_sk_name;
        if (enable_injection && !inject_path.empty() &&
            m_d3d9_replacements.find(hash) == m_d3d9_replacements.end())
        {
            build_replacement9(device, hash, inject_path, original_levels, details);
        }

        m_tracked_textures[hash] = details;
        Logger::get().debug("[TextureManager] Tracked D3D9 texture: 0x" + details.hash_hex + " (" + std::to_string(width) + "x" + std::to_string(height) + ")");

        if (auto_dump)
        {
            DXGI_FORMAT dxgi_fmt = d3d9_format_to_dxgi(format);
            if (dxgi_fmt != DXGI_FORMAT_UNKNOWN)
            {
                // Only level 0 exists at this moment: the game uploads mips one lock at a time,
                // so the rest are not filled yet. Manual Dump reads the finished chain back.
                std::vector<std::vector<uint8_t>> levels;
                append_tight_level(levels, pixel_data, pitch, static_cast<reshade::api::format>(dxgi_fmt),
                                   dxgi_format_is_compressed(dxgi_fmt), width, height);
                dump_texture(hash, width, height, dxgi_fmt, std::move(levels));
                if (m_tracked_textures[hash].status != TextureStatus::INJECTED)
                    if (m_tracked_textures[hash].status != TextureStatus::INJECTED)
                m_tracked_textures[hash].status = TextureStatus::DUMPED;
            }
        }
    }

    // Builds the DX9 replacement for one hash and stores it in m_d3d9_replacements.
    // original_levels is the original texture's level count. Caller MUST hold m_mutex.
    // Returns true when a replacement was created.
    bool TextureManager::build_replacement9(IDirect3DDevice9 *device, uint64_t hash, const std::filesystem::path &inject_path, UINT original_levels, TextureDetails &details)
    {
        if (device == nullptr)
            return false;

        bool created = false;
        {
            DDSImage dds;
            if (load_dds(inject_path.string(), dds) && !dds.subresources.empty())
            {
                D3DFORMAT d3d9_target_fmt = dxgi_to_d3d9_format(dds.format);
                if (d3d9_target_fmt != D3DFMT_UNKNOWN)
                {
                    // Match the original's mip topology: single level stays single,
                    // a mipmapped original gets a full chain (auto-generated as needed).
                    uint32_t target_levels = choose_replacement_levels(dds, (original_levels == 0) ? 1u : original_levels);
                    std::vector<MipLevel> mips = build_replacement_mips(dds, target_levels);

                    if (mips.empty())
                    {
                        Logger::get().error("[TextureManager] Injected DDS 0x" + format_hash_hex(hash) + " produced no usable mip levels.");
                    }
                    else
                    {
                        // Only the single-level case is worth a warning. A shorter chain that the
                        // author actually authored is a choice, and is applied without comment.
                        if (original_levels > 1 && mips.size() == 1)
                        {
                            Logger::get().warn("[TextureManager] Injected DDS 0x" + format_hash_hex(hash) + " has a single mip level, replacing a texture that had " + std::to_string(original_levels) + ". Mips cannot be generated from block-compressed data, so it samples level 0 at every distance and will shimmer in motion. Re-export with mipmaps if that was not intended.");
                        }

                        IDirect3DTexture9 *highres_tex = nullptr;
                        ScopedFlag no_reentry(D3D9Hook::s_inside_injection);
                        HRESULT hr = device->CreateTexture(
                            dds.width, dds.height, static_cast<UINT>(mips.size()), 0,
                            d3d9_target_fmt, D3DPOOL_MANAGED, &highres_tex, nullptr);

                        bool needs_swizzle = (dds.format == reshade::api::format::r8g8b8a8_unorm || dds.format == reshade::api::format::r8g8b8a8_unorm_srgb);
                        UINT block_height = is_block_compressed(dds.format) ? 4 : 1;

                        if (SUCCEEDED(hr) && highres_tex != nullptr)
                        {
                            bool upload_ok = true;
                            for (size_t lvl = 0; lvl < mips.size(); ++lvl)
                            {
                                D3DLOCKED_RECT rect = {};
                                if (FAILED(highres_tex->LockRect(static_cast<UINT>(lvl), &rect, nullptr, 0)))
                                {
                                    upload_ok = false;
                                    break;
                                }

                                const MipLevel &m = mips[lvl];
                                UINT num_rows = (m.height + (block_height - 1)) / block_height;
                                UINT copy_row_pitch = (std::min)(static_cast<UINT>(rect.Pitch), m.row_pitch);
                                uint8_t *dest_ptr = static_cast<uint8_t *>(rect.pBits);

                                if (needs_swizzle)
                                {
                                    for (UINT y = 0; y < num_rows; ++y)
                                    {
                                        const uint8_t *src_row = m.ptr + y * m.row_pitch;
                                        uint8_t *dest_row = dest_ptr + y * rect.Pitch;
                                        for (UINT x = 0; x < m.width; ++x)
                                        {
                                            uint8_t r = src_row[x * 4 + 0];
                                            uint8_t g = src_row[x * 4 + 1];
                                            uint8_t b = src_row[x * 4 + 2];
                                            uint8_t a = src_row[x * 4 + 3];
                                            dest_row[x * 4 + 0] = b;
                                            dest_row[x * 4 + 1] = g;
                                            dest_row[x * 4 + 2] = r;
                                            dest_row[x * 4 + 3] = a;
                                        }
                                    }
                                }
                                else
                                {
                                    for (UINT y = 0; y < num_rows; ++y)
                                        std::memcpy(dest_ptr + y * rect.Pitch, m.ptr + y * m.row_pitch, copy_row_pitch);
                                }

                                highres_tex->UnlockRect(static_cast<UINT>(lvl));
                            }

                            if (upload_ok)
                            {
                                // Keep our reference (released in release_replacements).
                                m_d3d9_replacements[hash] = highres_tex;

                                details.status = TextureStatus::INJECTED;
                                details.filepath_injected = inject_path.string();
                                details.replacement_handle = reinterpret_cast<uint64_t>(highres_tex);
                                details.repl_width = dds.width;
                                details.repl_height = dds.height;
                                created = true;

                                Logger::get().info("[TextureManager] Loaded high-res DX9 replacement for 0x" + format_hash_hex(hash) + " (" + std::to_string(dds.width) + "x" + std::to_string(dds.height) + ", " + std::to_string(mips.size()) + " mips, original had " + std::to_string(original_levels) + ")");
                            }
                            else
                            {
                                highres_tex->Release();
                                Logger::get().error("[TextureManager] Failed to upload mip data for DX9 replacement 0x" + format_hash_hex(hash));
                            }
                        }
                        else
                        {
                            Logger::get().error("[TextureManager] Failed to create high-res replacement D3D9 texture for 0x" + format_hash_hex(hash));
                        }
                    }
                }
                else
                {
                    Logger::get().error("[TextureManager] Unsupported DX9 format mapping for injected texture " + inject_path.string() + ", format ID: " + std::to_string(static_cast<uint32_t>(dds.format)));
                }
            }
            else
            {
                Logger::get().error("[TextureManager] Failed to load injected DDS file " + inject_path.string() +
                                    (dds.load_error.empty() ? "" : " - " + dds.load_error));
            }
        }
        return created;
    }

    ID3D11ShaderResourceView *TextureManager::get_replacement_srv11(ID3D11ShaderResourceView *orig)
    {
        if (orig == nullptr)
            return orig;

        const uint32_t generation = m_bind_generation.load(std::memory_order_relaxed);
        const uint64_t frame = m_frame_count_atomic.load(std::memory_order_relaxed);

        // Fast path: one COM call, no lock. This runs for every texture the game binds.
        SrvBindCache cache = {};
        UINT cache_size = sizeof(cache);
        if (SUCCEEDED(orig->GetPrivateData(TT_SRV_CACHE_GUID, &cache_size, &cache)) &&
            cache_size == sizeof(cache) && cache.generation == generation)
        {
            if (cache.tracked == 0)
                return orig; // not one of ours; nothing to track or replace

            // Record visibility at most once per view per frame, into a per-thread buffer.
            if (cache.last_seen_frame != frame)
            {
                cache.last_seen_frame = frame;
                orig->SetPrivateData(TT_SRV_CACHE_GUID, sizeof(cache), &cache);

                s_seen_this_frame.push_back(cache.hash);
                if (s_seen_this_frame.size() >= 64)
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    flush_seen_locked();
                }
            }

            // enable_injection is read live rather than cached, so the panel's checkbox takes
            // effect immediately instead of waiting for a generation bump.
            return (cache.replacement != nullptr && enable_injection) ? cache.replacement : orig;
        }

        return resolve_srv11_slow(orig, generation, frame);
    }

    // Everything the fast path above must not do: COM queries, the manager lock, and the one-time
    // bookkeeping (view format, preview pin, bulk-dump capture). Runs on the first bind of a view
    // and again whenever m_bind_generation moves.
    ID3D11ShaderResourceView *TextureManager::resolve_srv11_slow(ID3D11ShaderResourceView *orig, uint32_t generation, uint64_t frame)
    {
        SrvBindCache cache = {};
        cache.generation = generation;
        cache.tracked = 0;
        cache.last_seen_frame = frame;
        cache.replacement = nullptr;

        ID3D11Resource *orig_res = nullptr;
        orig->GetResource(&orig_res);
        if (orig_res == nullptr)
            return orig;

        // Resolve the content hash from the resource's private-data tag.
        uint64_t hash = 0;
        UINT size = sizeof(hash);
        HRESULT hr = orig_res->GetPrivateData(TT_HASH_GUID, &size, &hash);
        orig_res->Release();
        if (FAILED(hr) || size != sizeof(hash))
        {
            // Remember the miss too: an untracked view is the common case in a busy frame, and
            // this is what stops it paying for three COM calls on every bind.
            orig->SetPrivateData(TT_SRV_CACHE_GUID, sizeof(cache), &cache);
            return orig;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
        orig->GetDesc(&vd);

        // Our replacements are plain 2D views. If the game is sampling this content as an array,
        // cube, 3D or multisampled view, handing its shader a TEXTURE2D SRV would feed the wrong
        // resource type to the sampler: corrupt output or a dropped draw. Track it, but never
        // substitute.
        const bool replaceable_view = (vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D);

        cache.tracked = 1;
        cache.hash = hash;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_current_frame_hashes.insert(hash);

        // Record how the game samples this texture (the SRV's concrete view format). Done
        // once per texture; it reveals the sRGB intent that a TYPELESS resource hides.
        {
            auto tit = m_tracked_textures.find(hash);
            if (tit != m_tracked_textures.end() && tit->second.view_format_id == 0 &&
                vd.Format != DXGI_FORMAT_UNKNOWN)
            {
                tit->second.view_format_id = static_cast<uint32_t>(vd.Format);
                tit->second.view_format_str = dxgi_format_name(vd.Format);
            }
        }

        if (!replaceable_view)
        {
            static bool s_warned = false;
            if (!s_warned)
            {
                s_warned = true;
                Logger::get().warn("[TextureManager] Texture 0x" + format_hash_hex(hash) +
                                   " is sampled through a non-2D view (dimension " +
                                   std::to_string(static_cast<int>(vd.ViewDimension)) +
                                   "); such textures are listed but not replaced.");
            }
        }

        // Pin the live original SRV for preview if this is the selected texture.
        if (hash == m_preview_target_hash && m_preview_srv11 == nullptr)
        {
            orig->AddRef();
            m_preview_srv11 = orig;
        }

        // Bulk dump: take a reference the first time a queued texture is drawn.
        if (!m_pending_dumps.empty())
        {
            auto pit = m_pending_dumps.find(hash);
            if (pit != m_pending_dumps.end())
            {
                m_pending_dumps.erase(pit);
                orig->AddRef();
                m_readback_queue.push_back({hash, nullptr, orig});
            }
        }

        ID3D11ShaderResourceView *result = orig;
        if (enable_injection && replaceable_view)
        {
            auto it = m_d3d11_replacements.find(hash);
            if (it == m_d3d11_replacements.end())
            {
                // Hot reload: see the D3D9 path in get_replacement_texture9.
                note_pending_injection(hash, true);
            }
            else if (it->second != nullptr)
            {
                cache.replacement = it->second;
                result = it->second;
            }
        }

        orig->SetPrivateData(TT_SRV_CACHE_GUID, sizeof(cache), &cache);
        return result;
    }

    void TextureManager::register_unmap_texture11(ID3D11Device *device, ID3D11Resource *resource, const void *pixel_data, UINT width, UINT height, DXGI_FORMAT format, UINT pitch,
                                                  const D3D11_SUBRESOURCE_DATA *initial_levels, UINT initial_level_count)
    {
        if (device == nullptr || resource == nullptr || pixel_data == nullptr || width == 0 || height == 0)
            return;

        if (filter_small_textures && (width < 16 || height < 16))
            return;

        reshade::api::format reshade_fmt = static_cast<reshade::api::format>(format);

        // Hash the tight rows, never the source pitch. On the Map/Unmap path that pitch is chosen
        // by the driver (commonly padded to an alignment, and the padding itself can be
        // uninitialised), so hashing it would make the same texture hash differently on another
        // GPU and break every shared texture mod. See TextureHash.h.
        const UINT tight_row = reshade::api::format_row_pitch(reshade_fmt, width);
        if (tight_row == 0)
            return;
        const UINT rows = dxgi_format_is_compressed(format) ? ((height + 3) / 4) : height;

        uint64_t hash = compute_hash64_rows(static_cast<const uint8_t *>(pixel_data), pitch, tight_row, rows);
        if (hash == 0)
            return;

        const uint32_t sk_hash = accept_sk_names
            ? compute_crc32c_rows(static_cast<const uint8_t *>(pixel_data), pitch, tight_row, rows)
            : 0u;

        // Original description drives the replacement's mip topology and the info panel.
        D3D11_TEXTURE2D_DESC orig_desc = {};
        {
            ID3D11Texture2D *orig_tex = nullptr;
            if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&orig_tex))) && orig_tex != nullptr)
            {
                orig_tex->GetDesc(&orig_desc);
                orig_tex->Release();
            }
        }
        UINT original_levels = orig_desc.MipLevels; // 0 = full chain generated by the runtime

        std::lock_guard<std::mutex> lock(m_mutex);

        // Tag the original resource with its content hash (see get_replacement_srv11).
        resource->SetPrivateData(TT_HASH_GUID, sizeof(hash), &hash);

        TextureDetails details;
        details.hash = hash;
        details.hash_hex = format_hash_hex(hash);
        details.sk_hash = sk_hash;
        details.width = width;
        details.height = height;
        details.mip_levels = (original_levels == 0) ? full_mip_count(width, height) : original_levels;
        details.format_id = static_cast<uint32_t>(format);
        details.format_str = "DXGI_FORMAT_" + dxgi_format_name(format);
        details.format_short = "DX11_" + dxgi_format_name(format);
        details.is_compressed = dxgi_format_is_compressed(format);
        details.is_srgb = dxgi_format_is_srgb(format);
        details.is_dx11 = true;
        details.array_size = (orig_desc.ArraySize > 0) ? orig_desc.ArraySize : 1;
        details.bind_flags = orig_desc.BindFlags;
        details.misc_flags = orig_desc.MiscFlags;
        details.cpu_access = orig_desc.CPUAccessFlags;
        details.usage = static_cast<uint32_t>(orig_desc.Usage);
        details.data_size = compute_texture_bytes(reshade_fmt, width, height, details.mip_levels);
        details.last_seen_frame = m_frame_count;
        details.last_seen_ticks = GetTickCount64();

        bool via_sk_name = false;
        std::filesystem::path inject_path = find_injection_path(hash, sk_hash, &via_sk_name);
        details.injected_via_sk_name = via_sk_name;
        if (enable_injection && !inject_path.empty() &&
            m_d3d11_replacements.find(hash) == m_d3d11_replacements.end())
        {
            build_replacement11(device, hash, inject_path, original_levels, details);
        }

        m_tracked_textures[hash] = details;

        if (auto_dump)
        {
            // CreateTexture2D hands over every level at once, so auto-dump can capture the whole
            // chain there. The Map/Unmap path only ever exposes one subresource, so it stays flat.
            std::vector<std::vector<uint8_t>> levels;
            if (initial_levels != nullptr && initial_level_count > 1)
            {
                for (UINT level = 0; level < initial_level_count; ++level)
                {
                    append_tight_level(levels, initial_levels[level].pSysMem, initial_levels[level].SysMemPitch,
                                       reshade_fmt, dxgi_format_is_compressed(format),
                                       (std::max)(1u, width >> level), (std::max)(1u, height >> level));
                }
            }
            if (levels.empty())
            {
                append_tight_level(levels, pixel_data, pitch, reshade_fmt,
                                   dxgi_format_is_compressed(format), width, height);
            }
            dump_texture(hash, width, height, format, std::move(levels));
            if (m_tracked_textures[hash].status != TextureStatus::INJECTED)
                m_tracked_textures[hash].status = TextureStatus::DUMPED;
        }
    }

    // Builds the DX11 replacement for one hash and stores it in m_d3d11_replacements.
    // original_levels is the original texture's MipLevels (0 = runtime-generated full chain).
    // Caller MUST hold m_mutex. Returns true when a replacement was created.
    bool TextureManager::build_replacement11(ID3D11Device *device, uint64_t hash, const std::filesystem::path &inject_path, UINT original_levels, TextureDetails &details)
    {
        if (device == nullptr)
            return false;

        bool created = false;
        {
            DDSImage dds;
            if (load_dds(inject_path.string(), dds) && !dds.subresources.empty())
            {
                // Single-level originals stay single-level; mipmapped originals
                // (or runtime-generated full chains, MipLevels == 0) get a full chain.
                uint32_t target_levels = choose_replacement_levels(dds, original_levels);
                std::vector<MipLevel> mips = build_replacement_mips(dds, target_levels);

                if (mips.empty())
                {
                    Logger::get().error("[TextureManager] Injected DDS 0x" + format_hash_hex(hash) + " produced no usable mip levels.");
                }
                else
                {
                    // See the D3D9 path: compare against the original's real level count.
                    // original_levels == 0 means the runtime generates a full chain.
                    const size_t orig_effective_levels = (original_levels == 0)
                        ? full_mip_count(details.width, details.height)
                        : original_levels;
                    // Only the single-level case is worth a warning. A shorter chain that the
                    // author actually authored is a choice, and is applied without comment.
                    if (orig_effective_levels > 1 && mips.size() == 1)
                    {
                        Logger::get().warn("[TextureManager] Injected DDS 0x" + format_hash_hex(hash) + " has a single mip level, replacing a texture that had " + std::to_string(orig_effective_levels) + ". Mips cannot be generated from block-compressed data, so it samples level 0 at every distance and will shimmer in motion. Re-export with mipmaps if that was not intended.");
                    }

                    // Use a concrete (non-TYPELESS) format so the SRV is valid.
                    DXGI_FORMAT view_fmt = dxgi_concrete_format(static_cast<DXGI_FORMAT>(dds.format));

                    D3D11_TEXTURE2D_DESC desc = {};
                    desc.Width = dds.width;
                    desc.Height = dds.height;
                    desc.MipLevels = static_cast<UINT>(mips.size());
                    desc.ArraySize = 1;
                    desc.Format = view_fmt;
                    desc.SampleDesc.Count = 1;
                    desc.SampleDesc.Quality = 0;
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    desc.CPUAccessFlags = 0;
                    desc.MiscFlags = 0;

                    std::vector<D3D11_SUBRESOURCE_DATA> subres_data(mips.size());
                    for (size_t i = 0; i < mips.size(); ++i)
                    {
                        subres_data[i].pSysMem = mips[i].ptr;
                        subres_data[i].SysMemPitch = mips[i].row_pitch;
                        subres_data[i].SysMemSlicePitch = mips[i].slice_pitch;
                    }

                    ID3D11Texture2D *highres_tex = nullptr;
                    ScopedFlag no_reentry(D3D11Hook::s_inside_injection);
                    HRESULT hr = device->CreateTexture2D(&desc, subres_data.data(), &highres_tex);
                    if (SUCCEEDED(hr) && highres_tex != nullptr)
                    {
                        ID3D11ShaderResourceView *highres_srv = nullptr;
                        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                        srv_desc.Format = desc.Format;
                        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        srv_desc.Texture2D.MostDetailedMip = 0;
                        srv_desc.Texture2D.MipLevels = desc.MipLevels;

                        hr = device->CreateShaderResourceView(highres_tex, &srv_desc, &highres_srv);
                        highres_tex->Release(); // SRV holds the texture reference

                        if (SUCCEEDED(hr) && highres_srv != nullptr)
                        {
                            // Keep our reference (released in release_replacements).
                            m_d3d11_replacements[hash] = highres_srv;
                            m_bind_generation.fetch_add(1, std::memory_order_relaxed);

                            details.status = TextureStatus::INJECTED;
                            details.filepath_injected = inject_path.string();
                            details.replacement_handle = reinterpret_cast<uint64_t>(highres_srv);
                            details.repl_width = dds.width;
                            details.repl_height = dds.height;
                            created = true;

                            Logger::get().info("[TextureManager] Loaded DX11 replacement for 0x" + format_hash_hex(hash) + " (" + std::to_string(dds.width) + "x" + std::to_string(dds.height) + ", " + std::to_string(mips.size()) + " mips, original had " + std::to_string(original_levels) + ")");
                        }
                    }
                    else
                    {
                        Logger::get().error("[TextureManager] Failed to create DX11 replacement texture for 0x" + format_hash_hex(hash) + ", HRESULT: " + std::to_string(hr));
                    }
                }
            }
            else
            {
                Logger::get().error("[TextureManager] Failed to load injected DDS file " + inject_path.string() +
                                    (dds.load_error.empty() ? "" : " - " + dds.load_error));
            }
        }
        return created;
    }

    std::vector<TextureDetails> TextureManager::get_active_textures()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<TextureDetails> result;
        result.reserve(m_tracked_textures.size());

        for (auto &pair : m_tracked_textures)
        {
            // Report what is actually happening on screen. Saying "Injected" merely because a
            // file exists hid a real bug: two mipmapped textures showed as injected for weeks
            // while their replacement was built and never bound. A file that is present but not
            // applied is PENDING, which is a question the user can act on.
            if (pair.second.replacement_handle != 0)
                pair.second.status = TextureStatus::INJECTED;
            else if (m_injected_files.find(pair.first) != m_injected_files.end())
                pair.second.status = TextureStatus::PENDING;

            if (show_current_frame_only)
            {
                if (pair.second.last_seen_frame == 0 || (m_frame_count > 0 && pair.second.last_seen_frame + 60 < m_frame_count))
                    continue;
            }
            result.push_back(pair.second);
        }
        return result;
    }

    TextureManager::InjectionStats TextureManager::get_injection_stats()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        InjectionStats st;
        st.files_found = m_injected_files.size();
        st.applied = m_d3d9_replacements.size() + m_d3d11_replacements.size();
        st.failed = m_failed_injections.size();
        return st;
    }

    bool TextureManager::dump_texture(uint64_t hash, UINT width, UINT height, DXGI_FORMAT format, std::vector<std::vector<uint8_t>> levels)
    {
        if (levels.empty())
            return false;

        DumpRequest req;
        req.hash = hash;
        req.width = width;
        req.height = height;
        req.format = format;
        req.levels = std::move(levels);

        {
            std::lock_guard<std::mutex> lock(m_dump_mutex);
            m_dump_queue.push_back(std::move(req));
        }
        m_dump_cv.notify_one();
        return true;
    }

    // Writes a full mip chain to TT/dump. `levels` holds tightly-packed pixel data for mip 0..n.
    // Dumping every level is what lets a dump be edited and injected straight back: a compressed
    // replacement without its mips cannot have them regenerated, and would alias in motion.
    std::string TextureManager::write_dump_dds_mips(uint64_t hash, UINT width, UINT height, DXGI_FORMAT format,
                                                    const std::vector<std::vector<uint8_t>> &levels,
                                                    UINT array_size)
    {
        if (array_size == 0)
            array_size = 1;
        if (levels.empty() || width == 0 || height == 0)
            return {};

        format = dxgi_concrete_format(format);
        const reshade::api::format fmt = static_cast<reshade::api::format>(format);

        std::error_code ec;
        std::filesystem::create_directories(m_dump_dir, ec);
        const std::filesystem::path dds_path = m_dump_dir / (format_hash_hex(hash) + ".dds");

        // levels is slice-major: mip_levels entries per array slice, so the mip index restarts
        // at the top of every slice.
        if (array_size == 0)
            array_size = 1;
        const size_t mip_levels = levels.size() / array_size;
        if (mip_levels == 0)
            return {};

        std::vector<reshade::api::subresource_data> subres(levels.size());
        for (size_t i = 0; i < levels.size(); ++i)
        {
            const size_t level = i % mip_levels;
            const UINT w = (std::max)(1u, width >> level);
            const UINT h = (std::max)(1u, height >> level);
            UINT row_pitch = reshade::api::format_row_pitch(fmt, w);
            UINT slice_pitch = reshade::api::format_slice_pitch(fmt, row_pitch, h);
            if (slice_pitch == 0)
                slice_pitch = row_pitch * h;

            subres[i].data = const_cast<uint8_t *>(levels[i].data()); // save_dds only reads it
            subres[i].row_pitch = row_pitch;
            subres[i].slice_pitch = slice_pitch;
        }

        reshade::api::resource_desc desc;
        desc.texture.width = width;
        desc.texture.height = height;
        desc.texture.levels = static_cast<uint16_t>(mip_levels);
        desc.texture.format = fmt;

        if (!save_dds_multi_mip(dds_path.string(), desc, subres, static_cast<uint32_t>(mip_levels), array_size))
            return {};
        return dds_path.string();
    }

    void TextureManager::dump_worker_loop()
    {
        while (true)
        {
            DumpRequest req;
            {
                std::unique_lock<std::mutex> lock(m_dump_mutex);
                m_dump_cv.wait(lock, [this]() { return !m_dump_thread_running || !m_dump_queue.empty(); });

                if (!m_dump_thread_running && m_dump_queue.empty())
                    break;

                req = std::move(m_dump_queue.front());
                m_dump_queue.erase(m_dump_queue.begin());
            }

            std::string path = write_dump_dds_mips(req.hash, req.width, req.height, req.format, req.levels);
            if (!path.empty())
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_tracked_textures.find(req.hash);
                if (it != m_tracked_textures.end())
                {
                    // Dumping an injected texture must not hide that it is injected.
                    if (it->second.status != TextureStatus::INJECTED)
                        it->second.status = TextureStatus::DUMPED;
                    it->second.filepath_dumped = path;
                }
            }
            else
            {
                Logger::get().error("[TextureManager] Failed to dump texture 0x" + format_hash_hex(req.hash));
            }
        }
    }

    std::string TextureManager::dump_resource11(uint64_t hash, ID3D11Resource *res)
    {
        ID3D11Device *device = D3D11Hook::get().get_device();
        ID3D11DeviceContext *ctx = D3D11Hook::get().get_context();
        if (device == nullptr || ctx == nullptr || res == nullptr)
            return {};

        std::string path;
        ID3D11Texture2D *tex2d = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex2d))) && tex2d != nullptr)
        {
            D3D11_TEXTURE2D_DESC desc = {};
            tex2d->GetDesc(&desc);

            D3D11_TEXTURE2D_DESC staging = desc;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.BindFlags = 0;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.MiscFlags = 0;

            // Keep the re-entrancy guard set across the whole readback. Our staging
            // CreateTexture2D/Map/Unmap all pass through our own hooks; without the guard
            // Hooked_Unmap would re-enter register_unmap_texture11, which locks m_mutex -
            // and request_dump already holds it on this thread, so that recursive lock is
            // undefined behaviour (observed as a crash right after the .dds was written).
            ID3D11Texture2D *staging_tex = nullptr;
            ScopedFlag no_reentry(D3D11Hook::s_inside_injection);
            HRESULT hr = device->CreateTexture2D(&staging, nullptr, &staging_tex);

            if (SUCCEEDED(hr) && staging_tex != nullptr)
            {
                ctx->CopyResource(staging_tex, tex2d);

                // Read back the whole mip chain, repacked to tight rows (the staging pitch is
                // padded), so the .dds can be edited and injected back without losing its mips.
                const reshade::api::format fmt = static_cast<reshade::api::format>(desc.Format);
                const bool compressed = dxgi_format_is_compressed(desc.Format);
                const UINT level_count = (desc.MipLevels > 0) ? desc.MipLevels : 1;
                // Cubemaps and texture arrays carry more than one slice; D3D indexes them
                // slice-major, which is also the order DDS stores them in.
                const UINT slice_count = (desc.ArraySize > 0) ? desc.ArraySize : 1;

                std::vector<std::vector<uint8_t>> levels;
                levels.reserve(static_cast<size_t>(level_count) * slice_count);

                bool readback_ok = true;
                for (UINT slice = 0; slice < slice_count && readback_ok; ++slice)
                for (UINT level = 0; level < level_count; ++level)
                {
                    const UINT w = (std::max)(1u, desc.Width >> level);
                    const UINT h = (std::max)(1u, desc.Height >> level);
                    const UINT tight_row = reshade::api::format_row_pitch(fmt, w);
                    const UINT rows = compressed ? ((h + 3) / 4) : h;
                    if (tight_row == 0 || rows == 0)
                    {
                        readback_ok = false;
                        break;
                    }

                    const UINT subresource = D3D11CalcSubresource(level, slice, level_count);
                    D3D11_MAPPED_SUBRESOURCE mapped = {};
                    if (FAILED(ctx->Map(staging_tex, subresource, D3D11_MAP_READ, 0, &mapped)) || mapped.pData == nullptr)
                    {
                        readback_ok = false;
                        break;
                    }

                    std::vector<uint8_t> buf(static_cast<size_t>(tight_row) * rows);
                    const uint8_t *src = static_cast<const uint8_t *>(mapped.pData);
                    for (UINT y = 0; y < rows; ++y)
                        std::memcpy(buf.data() + static_cast<size_t>(y) * tight_row,
                                    src + static_cast<size_t>(y) * mapped.RowPitch, tight_row);
                    ctx->Unmap(staging_tex, subresource);

                    levels.push_back(std::move(buf));
                }

                // A partial readback would misalign the slice-major layout, so only write a
                // complete set.
                if (readback_ok && levels.size() == static_cast<size_t>(level_count) * slice_count)
                    path = write_dump_dds_mips(hash, desc.Width, desc.Height, desc.Format, levels, slice_count);
                staging_tex->Release();
            }
            tex2d->Release();
        }
        return path;
    }

    std::string TextureManager::dump_base_texture9(uint64_t hash, IDirect3DBaseTexture9 *base)
    {
        if (base == nullptr)
            return {};

        IDirect3DTexture9 *tex = nullptr;
        if (FAILED(base->QueryInterface(__uuidof(IDirect3DTexture9), reinterpret_cast<void **>(&tex))) || tex == nullptr)
            return {};

        std::string path;
        D3DSURFACE_DESC sd = {};
        if (SUCCEEDED(tex->GetLevelDesc(0, &sd)))
        {
            DXGI_FORMAT dxgi = d3d9_format_to_dxgi(sd.Format);

            ScopedFlag no_reentry(D3D9Hook::s_inside_injection);

            D3DLOCKED_RECT lr = {};
            if (dxgi != DXGI_FORMAT_UNKNOWN && SUCCEEDED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
            {
                // Lockable pool (managed / system memory / dynamic). Read back the whole mip
                // chain, tightly packed, so the dump can be injected back with its mips intact.
                tex->UnlockRect(0);

                const reshade::api::format fmt = static_cast<reshade::api::format>(dxgi);
                const bool compressed = dxgi_format_is_compressed(dxgi);
                const UINT level_count = tex->GetLevelCount();

                std::vector<std::vector<uint8_t>> levels;
                levels.reserve(level_count);

                for (UINT level = 0; level < level_count; ++level)
                {
                    const UINT w = (std::max)(1u, sd.Width >> level);
                    const UINT h = (std::max)(1u, sd.Height >> level);
                    const UINT tight_row = reshade::api::format_row_pitch(fmt, w);
                    const UINT rows = compressed ? ((h + 3) / 4) : h;
                    if (tight_row == 0 || rows == 0)
                        break;

                    D3DLOCKED_RECT lrl = {};
                    if (FAILED(tex->LockRect(level, &lrl, nullptr, D3DLOCK_READONLY)) || lrl.pBits == nullptr)
                        break;

                    std::vector<uint8_t> buf(static_cast<size_t>(tight_row) * rows);
                    const uint8_t *src = static_cast<const uint8_t *>(lrl.pBits);
                    for (UINT y = 0; y < rows; ++y)
                        std::memcpy(buf.data() + static_cast<size_t>(y) * tight_row,
                                    src + static_cast<size_t>(y) * lrl.Pitch, tight_row);
                    tex->UnlockRect(level);

                    levels.push_back(std::move(buf));
                }

                path = write_dump_dds_mips(hash, sd.Width, sd.Height, dxgi, levels);
            }
            else if (dxgi != DXGI_FORMAT_UNKNOWN && (sd.Usage & D3DUSAGE_RENDERTARGET))
            {
                // Render target: copy to a system-memory surface, then read that back.
                IDirect3DDevice9 *dev = D3D9Hook::get().get_device();
                IDirect3DSurface9 *src = nullptr;
                IDirect3DSurface9 *dst = nullptr;
                if (dev != nullptr && SUCCEEDED(tex->GetSurfaceLevel(0, &src)) && src != nullptr)
                {
                    if (SUCCEEDED(dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &dst, nullptr)) && dst != nullptr)
                    {
                        D3DLOCKED_RECT lr2 = {};
                        if (SUCCEEDED(dev->GetRenderTargetData(src, dst)) &&
                            SUCCEEDED(dst->LockRect(&lr2, nullptr, D3DLOCK_READONLY)))
                        {
                            // A render target has no mip chain to read back; dump its one level.
                            std::vector<std::vector<uint8_t>> rt_level;
                            append_tight_level(rt_level, lr2.pBits, lr2.Pitch,
                                               static_cast<reshade::api::format>(dxgi),
                                               dxgi_format_is_compressed(dxgi), sd.Width, sd.Height);
                            dst->UnlockRect();
                            path = write_dump_dds_mips(hash, sd.Width, sd.Height, dxgi, rt_level);
                        }
                        dst->Release();
                    }
                    src->Release();
                }
            }

        }

        tex->Release();
        return path; // request_dump reports the failure with context
    }

    bool TextureManager::request_dump(uint64_t hash)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_tracked_textures.find(hash);
        if (it == m_tracked_textures.end())
            return false;
        TextureDetails &d = it->second;

        // We read back only the live handle pinned while the texture is on screen. Reading
        // an arbitrary tracked pointer is unsafe (it may have been freed and its address
        // reused), so dumping requires the texture to be visible when the button is clicked.
        std::string path;
        if (hash != m_preview_target_hash)
        {
            Logger::get().warn("[TextureManager] Cannot dump 0x" + format_hash_hex(hash) + ": select the texture first.");
            return false;
        }

        bool attempted = false;
        if (d.is_dx11 && m_preview_srv11 != nullptr)
        {
            attempted = true;
            ID3D11Resource *res = nullptr;
            m_preview_srv11->GetResource(&res);
            if (res != nullptr)
            {
                path = dump_resource11(hash, res);
                res->Release();
            }
        }
        else if (!d.is_dx11 && m_preview_tex9 != nullptr)
        {
            attempted = true;
            path = dump_base_texture9(hash, m_preview_tex9);
        }

        if (!path.empty())
        {
            d.status = TextureStatus::DUMPED;
            d.filepath_dumped = path;
            Logger::get().info("[TextureManager] Dumped 0x" + format_hash_hex(hash) + " to " + path);
            return true;
        }

        if (!attempted)
            Logger::get().warn("[TextureManager] Cannot dump 0x" + format_hash_hex(hash) + ": it is not currently on screen. Select it while it is being drawn, then Dump.");
        else
            Logger::get().warn("[TextureManager] Cannot dump 0x" + format_hash_hex(hash) + ": this texture cannot be read back on demand (D3D9 default-pool). Turn on Auto-dump to capture it from the upload at load time.");
        return false;
    }

    size_t TextureManager::dump_all(bool scene_only)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t queued = 0;
        for (auto &pair : m_tracked_textures)
        {
            if (scene_only)
            {
                const TextureDetails &d = pair.second;
                bool active = d.last_seen_frame != 0 && !(m_frame_count > 0 && d.last_seen_frame + 60 < m_frame_count);
                if (!active)
                    continue;
            }
            m_pending_dumps.insert(pair.first);
            ++queued;
        }
        m_bind_generation.fetch_add(1, std::memory_order_relaxed);
        Logger::get().info("[TextureManager] Dump-all queued " + std::to_string(queued) + (scene_only ? " active" : " tracked") + " texture(s); each is written the next time it is drawn.");
        return queued;
    }

    // Drains a few queued bulk-dump readbacks per frame. Caller MUST hold m_mutex.
    void TextureManager::process_readback_queue()
    {
        int budget = 8;
        while (!m_readback_queue.empty() && budget-- > 0)
        {
            // FIFO: with LIFO a large Dump All served the newest first and starved the oldest.
            PendingReadback rb = m_readback_queue.front();
            m_readback_queue.erase(m_readback_queue.begin());

            std::string path;
            if (rb.srv11 != nullptr)
            {
                ID3D11Resource *res = nullptr;
                rb.srv11->GetResource(&res);
                if (res != nullptr)
                {
                    path = dump_resource11(rb.hash, res);
                    res->Release();
                }
                rb.srv11->Release();
            }
            else if (rb.tex9 != nullptr)
            {
                path = dump_base_texture9(rb.hash, rb.tex9);
                rb.tex9->Release();
            }

            if (!path.empty())
            {
                auto it = m_tracked_textures.find(rb.hash);
                if (it != m_tracked_textures.end())
                {
                    // Dumping an injected texture must not hide that it is injected.
                    if (it->second.status != TextureStatus::INJECTED)
                        it->second.status = TextureStatus::DUMPED;
                    it->second.filepath_dumped = path;
                }
            }
        }
    }
}
