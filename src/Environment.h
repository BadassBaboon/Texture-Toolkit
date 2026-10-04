#pragma once

#include <string>

// What else is running inside the game, for the startup watchdog and the panel's Build Info: the
// usual reasons a texture never reaches us.
namespace TextureToolkit
{
    // Overlays, wrappers and proxy DLLs loaded into the game ("ReShade (d3d9.dll), Steam overlay"),
    // or "none detected".
    std::string describe_other_software();

    // Graphics APIs loaded that Texture Toolkit does not hook ("Direct3D 12, Vulkan"), or empty.
    std::string describe_unhooked_apis();
}
