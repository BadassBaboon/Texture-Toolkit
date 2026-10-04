#pragma once

#include <string>
#include <vector>
#include <map>
#include <initializer_list>
#include <filesystem>
#include <windows.h>

namespace TextureToolkit
{
    // Printable name of a virtual-key code, for the panel title, the startup banner and the log.
    // All three used to say "INSERT" whatever HotKey was actually set to.
    std::string hotkey_name(uint32_t vk);

    // Folder names compare without case, as Windows does.
    struct CaseInsensitiveLess
    {
        bool operator()(const std::wstring &a, const std::wstring &b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
    };

    // 1/0, true/false, yes/no, on/off, any case; anything else is `fallback`.
    inline bool parse_ini_bool(const std::wstring &v, bool fallback)
    {
        for (const wchar_t *t : { L"1", L"true", L"yes", L"on" })
            if (_wcsicmp(v.c_str(), t) == 0)
                return true;
        for (const wchar_t *f : { L"0", L"false", L"no", L"off" })
            if (_wcsicmp(v.c_str(), f) == 0)
                return false;
        return fallback;
    }

    // The inject folder's entry in the mod load order.
    inline const wchar_t *const kBaseModId = L"inject";

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

        // Texture mods: every folder under the resource root other than dump/ and inject/ is a
        // mod (see TextureManager::rescan_injected). mod_load_order is [Mods] LoadOrder, folder
        // names highest priority first, with kBaseModId standing for the inject folder itself;
        // mods it does not name go after everything it does. mod_enabled is [ModEnabled], one
        // <folder>=0/1 per mod the user has switched, overriding the Enabled the mod ships with.
        std::vector<std::wstring> mod_load_order;
        std::map<std::wstring, bool, CaseInsensitiveLess> mod_enabled;
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

        void save_keys();
        void write_template();

        std::filesystem::path m_ini_path;
        Configuration m_config;
    };
}
