#include "DDSLoader.h"
#include <fstream>
#include <cstring>
#include <algorithm>

namespace TextureToolkit
{
#pragma pack(push, 1)
    struct DDS_PIXELFORMAT
    {
        uint32_t dwSize;
        uint32_t dwFlags;
        uint32_t dwFourCC;
        uint32_t dwRGBBitCount;
        uint32_t dwRBitMask;
        uint32_t dwGBitMask;
        uint32_t dwBBitMask;
        uint32_t dwABitMask;
    };

    struct DDS_HEADER
    {
        uint32_t dwSize;
        uint32_t dwFlags;
        uint32_t dwHeight;
        uint32_t dwWidth;
        uint32_t dwPitchOrLinearSize;
        uint32_t dwDepth;
        uint32_t dwMipMapCount;
        uint32_t dwReserved1[11];
        DDS_PIXELFORMAT ddspf;
        uint32_t dwCaps;
        uint32_t dwCaps2;
        uint32_t dwCaps3;
        uint32_t dwCaps4;
        uint32_t dwReserved2;
    };

    struct DDS_HEADER_DXT10
    {
        uint32_t dxgiFormat;
        uint32_t resourceDimension;
        uint32_t miscFlag;
        uint32_t arraySize;
        uint32_t miscFlags2;
    };
#pragma pack(pop)

    static constexpr uint32_t DDS_MAGIC = 0x20534444; // "DDS "
    static constexpr uint32_t DDPF_FOURCC = 0x4;
    static constexpr uint32_t MAKE_FOURCC(char a, char b, char c, char d)
    {
        return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) | (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
    }

    static reshade::api::format dxgi_to_reshade_format(uint32_t dxgi_format)
    {
        return static_cast<reshade::api::format>(dxgi_format);
    }

    static uint32_t reshade_format_to_dxgi(reshade::api::format format)
    {
        return static_cast<uint32_t>(format);
    }

    bool load_dds(const std::string &filepath, DDSImage &out_image)
    {
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open())
            return false;

        uint32_t magic = 0;
        file.read(reinterpret_cast<char *>(&magic), sizeof(magic));
        if (magic != DDS_MAGIC)
            return false;

        DDS_HEADER header = {};
        file.read(reinterpret_cast<char *>(&header), sizeof(header));
        if (header.dwSize != sizeof(DDS_HEADER) || header.ddspf.dwSize != sizeof(DDS_PIXELFORMAT))
            return false;

        out_image.width = header.dwWidth;
        out_image.height = header.dwHeight;
        out_image.depth = (header.dwDepth > 0) ? header.dwDepth : 1;
        out_image.mip_levels = (header.dwMipMapCount > 0) ? header.dwMipMapCount : 1;
        out_image.array_size = 1;

        reshade::api::format fmt = reshade::api::format::unknown;

        if ((header.ddspf.dwFlags & DDPF_FOURCC) != 0)
        {
            if (header.ddspf.dwFourCC == MAKE_FOURCC('D', 'X', '1', '0'))
            {
                DDS_HEADER_DXT10 dxt10 = {};
                file.read(reinterpret_cast<char *>(&dxt10), sizeof(dxt10));
                fmt = dxgi_to_reshade_format(dxt10.dxgiFormat);
                if (dxt10.arraySize > 0)
                    out_image.array_size = dxt10.arraySize;
            }
            else if (header.ddspf.dwFourCC == MAKE_FOURCC('D', 'X', 'T', '1'))
                fmt = reshade::api::format::bc1_unorm;
            else if (header.ddspf.dwFourCC == MAKE_FOURCC('D', 'X', 'T', '3'))
                fmt = reshade::api::format::bc2_unorm;
            else if (header.ddspf.dwFourCC == MAKE_FOURCC('D', 'X', 'T', '5'))
                fmt = reshade::api::format::bc3_unorm;
            else if (header.ddspf.dwFourCC == MAKE_FOURCC('A', 'T', 'I', '1') || header.ddspf.dwFourCC == MAKE_FOURCC('B', 'C', '4', 'U'))
                fmt = reshade::api::format::bc4_unorm;
            else if (header.ddspf.dwFourCC == MAKE_FOURCC('A', 'T', 'I', '2') || header.ddspf.dwFourCC == MAKE_FOURCC('B', 'C', '5', 'U'))
                fmt = reshade::api::format::bc5_unorm;
        }
        // Non-FOURCC uncompressed. 24-bit has no GPU texture format, so it is read at
        // 3 bytes/pixel and expanded to 32-bit RGBA here (fixes both the byte size and the
        // channel layout). 32-bit is taken as-is.
        const bool expand_24 = ((header.ddspf.dwFlags & DDPF_FOURCC) == 0) && header.ddspf.dwRGBBitCount == 24;

        // A legacy 32-bit DDS says which byte is which through its channel masks, and most tools
        // write B8G8R8A8 (R mask 0x00FF0000). Assuming RGBA regardless loaded every one of those
        // with red and blue swapped. 24-bit is expanded below and picks its order from the masks
        // the same way.
        if ((header.ddspf.dwFlags & DDPF_FOURCC) == 0 && header.ddspf.dwRGBBitCount == 32)
        {
            const bool bgra = (header.ddspf.dwRBitMask == 0x00FF0000u &&
                               header.ddspf.dwBBitMask == 0x000000FFu);
            fmt = bgra ? reshade::api::format::b8g8r8a8_unorm
                       : reshade::api::format::r8g8b8a8_unorm;
        }
        else if ((header.ddspf.dwFlags & DDPF_FOURCC) == 0 && header.ddspf.dwRGBBitCount == 24)
        {
            fmt = reshade::api::format::r8g8b8a8_unorm; // expanded to 32-bit using the masks below
        }

        if (fmt == reshade::api::format::unknown)
            fmt = reshade::api::format::r8g8b8a8_unorm;

        out_image.format = fmt;

        // Byte offset of each channel within a source pixel, from the DDS masks. Defaults
        // to B,G,R order (D3DFMT_R8G8B8) when the masks are absent.
        auto mask_byte = [](uint32_t m) -> int {
            if (m & 0x000000FFu) return 0;
            if (m & 0x0000FF00u) return 1;
            if (m & 0x00FF0000u) return 2;
            if (m & 0xFF000000u) return 3;
            return -1;
        };
        int rb = mask_byte(header.ddspf.dwRBitMask);
        int gb = mask_byte(header.ddspf.dwGBitMask);
        int bb = mask_byte(header.ddspf.dwBBitMask);
        if (rb < 0 || gb < 0 || bb < 0) { rb = 2; gb = 1; bb = 0; }

        uint32_t current_w = out_image.width;
        uint32_t current_h = out_image.height;

        for (uint32_t mip = 0; mip < out_image.mip_levels; ++mip)
        {
            uint32_t row_pitch = reshade::api::format_row_pitch(fmt, current_w);
            uint32_t slice_pitch = reshade::api::format_slice_pitch(fmt, row_pitch, current_h);

            if (slice_pitch == 0)
                slice_pitch = row_pitch * current_h;

            std::vector<uint8_t> buffer(slice_pitch);

            if (expand_24)
            {
                const uint32_t src_row = current_w * 3;
                std::vector<uint8_t> src(static_cast<size_t>(src_row) * current_h);
                file.read(reinterpret_cast<char *>(src.data()), src.size());
                if (file.gcount() < static_cast<std::streamsize>(src.size()))
                    break;

                for (uint32_t y = 0; y < current_h; ++y)
                {
                    for (uint32_t x = 0; x < current_w; ++x)
                    {
                        const uint8_t *s = &src[static_cast<size_t>(y) * src_row + x * 3];
                        uint8_t *d = &buffer[static_cast<size_t>(y) * row_pitch + x * 4];
                        d[0] = s[rb];
                        d[1] = s[gb];
                        d[2] = s[bb];
                        d[3] = 255;
                    }
                }
            }
            else
            {
                file.read(reinterpret_cast<char *>(buffer.data()), slice_pitch);
                if (file.gcount() < static_cast<std::streamsize>(slice_pitch))
                {
                    // Short read. On mip 0 this means the file does not match its own header (a
                    // format we mapped wrongly, or truncated data) and the load fails outright, so
                    // record what we expected: the numbers identify which of the two it was.
                    if (mip == 0)
                        out_image.load_error = "mip 0 short read: expected " + std::to_string(slice_pitch) +
                                               " bytes, got " + std::to_string(static_cast<long long>(file.gcount())) +
                                               " (" + std::to_string(out_image.width) + "x" + std::to_string(out_image.height) +
                                               ", format " + std::to_string(static_cast<uint32_t>(fmt)) +
                                               ", fourcc 0x" + std::to_string(header.ddspf.dwFourCC) +
                                               ", bitcount " + std::to_string(header.ddspf.dwRGBBitCount) + ")";
                    break;
                }
            }

            out_image.subresources.push_back(std::move(buffer));
            out_image.row_pitches.push_back(row_pitch);
            out_image.slice_pitches.push_back(slice_pitch);

            current_w = (std::max)(1u, current_w / 2);
            current_h = (std::max)(1u, current_h / 2);
        }

        return !out_image.subresources.empty();
    }

    bool save_dds(const std::string &filepath, const reshade::api::resource_desc &desc, const reshade::api::subresource_data &data)
    {
        std::vector<reshade::api::subresource_data> subres;
        subres.push_back(data);
        return save_dds_multi_mip(filepath, desc, subres);
    }

    bool save_dds_multi_mip(const std::string &filepath, const reshade::api::resource_desc &desc, const std::vector<reshade::api::subresource_data> &subresources)
    {
        if (subresources.empty() || subresources[0].data == nullptr)
            return false;

        std::ofstream file(filepath, std::ios::binary);
        if (!file.is_open())
            return false;

        uint32_t magic = DDS_MAGIC;
        file.write(reinterpret_cast<const char *>(&magic), sizeof(magic));

        DDS_HEADER header = {};
        header.dwSize = sizeof(DDS_HEADER);
        header.dwFlags = 0x1 | 0x2 | 0x4 | 0x1000; // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT
        header.dwWidth = desc.texture.width;
        header.dwHeight = desc.texture.height;
        header.dwDepth = desc.texture.depth_or_layers;
        header.dwMipMapCount = static_cast<uint32_t>(subresources.size());

        if (header.dwMipMapCount > 1)
            header.dwFlags |= 0x20000; // DDSD_MIPMAPCOUNT

        header.ddspf.dwSize = sizeof(DDS_PIXELFORMAT);
        header.ddspf.dwFlags = DDPF_FOURCC;
        header.ddspf.dwFourCC = MAKE_FOURCC('D', 'X', '1', '0');
        header.dwCaps = 0x1000; // DDSCAPS_TEXTURE

        file.write(reinterpret_cast<const char *>(&header), sizeof(header));

        DDS_HEADER_DXT10 dxt10 = {};
        dxt10.dxgiFormat = reshade_format_to_dxgi(desc.texture.format);
        dxt10.resourceDimension = 3; // 2D Texture
        dxt10.arraySize = 1;

        file.write(reinterpret_cast<const char *>(&dxt10), sizeof(dxt10));

        uint32_t w = desc.texture.width;
        uint32_t h = desc.texture.height;

        for (const auto &subres : subresources)
        {
            // The reader (load_dds) assumes tightly-packed rows, so we must write at the tight
            // pitch. A source can hand us a larger, padded pitch (D3D11 staging textures are
            // commonly 256-byte aligned); when it does, copy row by row and drop the padding,
            // otherwise a texture whose natural pitch is not already aligned dumps skewed.
            const uint32_t tight_row = reshade::api::format_row_pitch(desc.texture.format, w);
            uint32_t tight_slice = reshade::api::format_slice_pitch(desc.texture.format, tight_row, h);
            if (tight_slice == 0)
                tight_slice = tight_row * h;

            const uint32_t src_row = (subres.row_pitch != 0) ? subres.row_pitch : tight_row;
            const auto *src = static_cast<const uint8_t *>(subres.data);

            if (src_row == tight_row || tight_row == 0)
            {
                file.write(reinterpret_cast<const char *>(src), tight_slice);
            }
            else
            {
                const uint32_t rows = tight_slice / tight_row; // block-rows for BC, pixel-rows otherwise
                for (uint32_t y = 0; y < rows; ++y)
                    file.write(reinterpret_cast<const char *>(src + static_cast<size_t>(y) * src_row), tight_row);
            }

            w = (std::max)(1u, w / 2);
            h = (std::max)(1u, h / 2);
        }

        return true;
    }
}
