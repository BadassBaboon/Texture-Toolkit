#pragma once

#include <reshade.hpp>
#include <vector>
#include <string>
#include <cstdint>

namespace TextureToolkit
{
    struct DDSImage
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 1;
        uint32_t mip_levels = 1;
        uint32_t array_size = 1;
        reshade::api::format format = reshade::api::format::unknown;
        
        // Raw pixel/block payload for each mip level / subresource
        std::vector<std::vector<uint8_t>> subresources;
        std::vector<uint32_t> row_pitches;
        std::vector<uint32_t> slice_pitches;

        // Set when load_dds fails, describing why. Logged by the caller.
        std::string load_error;
    };

    bool load_dds(const std::string &filepath, DDSImage &out_image);
    bool save_dds(const std::string &filepath, const reshade::api::resource_desc &desc, const reshade::api::subresource_data &data);
    // `subresources` is ordered the way DDS stores it and D3D indexes it: slice-major, so all
    // `mip_levels` of slice 0, then all of slice 1, and so on. array_size 1 is an ordinary texture.
    bool save_dds_multi_mip(const std::string &filepath, const reshade::api::resource_desc &desc,
                            const std::vector<reshade::api::subresource_data> &subresources,
                            uint32_t mip_levels = 0, uint32_t array_size = 1);
}
