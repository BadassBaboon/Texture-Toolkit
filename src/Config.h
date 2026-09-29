#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <windows.h>

namespace TextureToolkit
{
    // Printable name of a virtual-key code, for the panel title, the startup banner and the log.
    // All three used to say "INSERT" whatever HotKey was actually set to.
    std::string hotkey_name(uint32_t vk);

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

        // Optional overlay folders, checked for a hash's .dds BEFORE the inject folder itself, in
        // this order, so an earlier one wins over both a later one and the base inject folder.
        // Each entry is relative to <ResourceRoot>/inject (or absolute), and does not need to
        // exist -- a folder that is not currently present is simply skipped, which is what makes
        // this useful for a conditional variant such as a controller-specific or dark-mode set:
        // only the hashes that folder actually ships override the base set, and it can be added,
        // removed or swapped for another without touching the base inject folder at all. Parsed
        // from the ini's semicolon-separated AdditionalSearchPath, e.g. "dualshock;darkmode".
        std::vector<std::filesystem::path> additional_search_paths;

        // Which content-hash algorithm identifies textures:
        //   0 = current 64-bit hash (default; no change from ordinary v1.1+ behaviour).
        //   1 = the 32-bit hash Texture Toolkit v1.0 used, with its 8-hex-digit naming, so a
        //       mod folder that still carries v1.0 file names loads without being renamed.
        //   2 = same as 1, and also writes "<ResourceRoot>/hash_migrate.txt" with one
        //       "<oldhash> <newhash>" line per texture that has a v1.0 replacement file, so that
        //       folder can be renamed to the current naming. See TextureHashLegacy.h.
        // Anything else is treated as 0.
        int hash_algorithm = 0;

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
