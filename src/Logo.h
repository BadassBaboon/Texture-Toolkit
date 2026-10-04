#pragma once

#include <imgui.h>

struct IDirect3DDevice9;
struct ID3D11Device;

// The Texture Toolkit logo as a GPU texture, for the panel and the startup banner. Built from the
// pixels compiled in from assets/logo.png (see tools/embed_logo.py) on whichever device the overlay
// draws with. Until it exists, or if creating it fails, draw_logo falls back to a drawn badge.
namespace TextureToolkit::Logo
{
    // Call once after the ImGui backend is initialised; a later call is a no-op.
    void create_d3d9(IDirect3DDevice9 *device);
    void create_d3d11(ID3D11Device *device);
    void release();

    // Draws the logo into `dl` with its top left at `pos`, `size` pixels square.
    void draw(ImDrawList *dl, ImVec2 pos, float size, float alpha = 1.0f);
}
