#pragma once

#include <cstdint>

// How long our own work inside each hot hook takes, written to the log every few seconds while
// verbose logging is on. Only Texture Toolkit's part of a hook is timed, never the game's or the
// driver's call it wraps, so the figures answer "is it us?" when a game stutters with it loaded.
// With verbose logging off, a timed scope costs one relaxed atomic load.
namespace TextureToolkit::HookTimings
{
    enum class Site : int
    {
        D3D9Upload,     // hashing and tracking a texture the game filled through LockRect
        D3D9Bind,       // choosing a replacement in SetTexture
        D3D11Create,    // hashing and tracking a texture created with its pixels
        D3D11Unmap,     // hashing and tracking a texture filled through Map/Unmap
        D3D11Bind,      // choosing replacements in *SetShaderResources
        Overlay,        // per-frame bookkeeping and the panel, inside Present
        Count
    };

    bool enabled();
    uint64_t now();
    void record(Site site, uint64_t start);

    // Call once per presented frame, from Present. Tracks frame times, and every few seconds
    // writes the figures gathered since the last report.
    void frame();

    class Scope
    {
    public:
        explicit Scope(Site site) : m_site(site), m_start(enabled() ? now() : 0) {}
        ~Scope()
        {
            if (m_start != 0)
                record(m_site, m_start);
        }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

    private:
        Site m_site;
        uint64_t m_start;
    };
}
