#pragma once

// The only pieces of ReShade's addon API this project ever used were the pixel-format enum and its
// two pitch helpers, plus a couple of plain data structs. Those are vendored under deps/reshade
// (the header is dual-licensed BSD-3-Clause OR MIT, and taken here under MIT) so a clean clone
// builds without a ReShade checkout sitting next to it.
//
// The format values are the DXGI_FORMAT values, and the pitch helpers are copied verbatim rather
// than retyped: the row pitch feeds the tight-row texture hash, so a transcription slip here would
// silently rename every file in every published mod.

#include "reshade_api_format.hpp"
#include <cstdint>

namespace reshade::api
{
    // Pixel data for one subresource. row_pitch of 0 means the rows are tightly packed.
    struct subresource_data
    {
        const void *data = nullptr;
        uint32_t row_pitch = 0;
        uint32_t slice_pitch = 0;
    };

    // Just the texture fields the DDS writer reads. Kept in ReShade's shape so the call sites did
    // not have to change when the dependency went away.
    struct resource_desc
    {
        struct
        {
            uint32_t width = 0;
            uint32_t height = 0;
            uint16_t depth_or_layers = 1;
            uint16_t levels = 1;
            format format = format::unknown;
        } texture;
    };
}
