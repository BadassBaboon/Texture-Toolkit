#pragma once

#include <string>
#include <filesystem>
#include <windows.h>

namespace TextureToolkit
{
    struct Configuration
    {
        uint32_t hotkey = VK_INSERT; // Default: INSERT key (0x2D)

        // Root folder for all of Texture Toolkit's runtime files (dump/, inject/, imgui.ini).
        // Relative to the game's executable folder, or an absolute path. Rename or relocate
        // this one value and everything moves together.
        std::filesystem::path resource_root = "TT";

        bool enable_injection = true;
        bool auto_dump = false;
        bool filter_small_textures = true;
        bool show_current_frame_only = true;

        // Also accept texture packs named the way Special K names them (CRC-32C of the top mip).
        bool accept_sk_names = true;

        bool show_osd_banner = true;
        float osd_duration_seconds = 6.0f;

        // When true, per-texture/per-hook Debug logging is written (very chatty).
        bool verbose = false;
    };

    class ConfigManager
    {
    public:
        static ConfigManager &get();

        void init(const std::filesystem::path &config_dir);
        void load();
        void save();

        Configuration &get_config() { return m_config; }

    private:
        ConfigManager() = default;

        std::filesystem::path m_ini_path;
        Configuration m_config;
    };
}
