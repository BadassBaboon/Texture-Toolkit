#include "Config.h"
#include "Logger.h"
#include <fstream>
#include <sstream>
#include <cstdio>

#include <imgui.h>

// Defined in imgui_impl_win32.cpp with external linkage, but not declared in its header, so it is
// declared here rather than by reaching into the backend's source.
ImGuiKey ImGui_ImplWin32_KeyEventToImGuiKey(WPARAM wParam, LPARAM lParam);

namespace TextureToolkit
{
    std::string hotkey_name(uint32_t vk)
    {
        // ImGui's own names rather than GetKeyNameTextW, which needs the extended-key bit set
        // correctly (Insert otherwise comes back as "Num 0"), returns nothing at all for Pause, and
        // localises into glyphs the Latin-only ImGui font cannot draw.
        //
        // The scancode in lParam is not optional here: punctuation keys are identified by scancode
        // because their virtual key differs per layout (tilde is VK_OEM_3 on US, VK_OEM_8 on UK,
        // VK_OEM_7 on French).
        const LPARAM lparam = static_cast<LPARAM>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16;
        const ImGuiKey key = ImGui_ImplWin32_KeyEventToImGuiKey(static_cast<WPARAM>(vk), lparam);
        if (key != ImGuiKey_None)
            return ImGui::GetKeyName(key);

        char buf[16] = {};
        std::snprintf(buf, sizeof(buf), "key 0x%02X", vk);
        return buf;
    }

    ConfigManager &ConfigManager::get()
    {
        static ConfigManager instance;
        return instance;
    }

    void ConfigManager::init(const std::filesystem::path &config_dir)
    {
        m_ini_path = config_dir / "TextureToolkit.ini";
        load();
    }

    void ConfigManager::load()
    {
        if (!std::filesystem::exists(m_ini_path))
        {
            save(); // Auto-generate default INI file
            return;
        }

        wchar_t ini_w[MAX_PATH];
        wcscpy_s(ini_w, m_ini_path.wstring().c_str());

        // Hotkey
        wchar_t hotkey_str[32] = L"";
        GetPrivateProfileStringW(L"TextureToolkit", L"HotKey", L"0x2D", hotkey_str, 32, ini_w);
        try
        {
            std::wstring hs(hotkey_str);
            m_config.hotkey = static_cast<uint32_t>(std::stoul(hs, nullptr, 16));
        }
        catch (...)
        {
            m_config.hotkey = VK_INSERT;
        }

        // A hotkey of 0 (or anything outside the virtual-key range) can never be pressed, which
        // would leave the panel unopenable with no clue why. Fall back and say so.
        if (m_config.hotkey == 0 || m_config.hotkey > 0xFE)
        {
            Logger::get().warn("[ConfigManager] HotKey=" + std::to_string(m_config.hotkey) +
                               " is not a usable virtual-key code; falling back to INSERT (0x2D).");
            m_config.hotkey = VK_INSERT;
        }

        // Resource root: holds dump/, inject/, and imgui.ini.
        wchar_t root_str[MAX_PATH] = L"";
        GetPrivateProfileStringW(L"TextureToolkit", L"ResourceRoot", L"TT", root_str, MAX_PATH, ini_w);
        m_config.resource_root = root_str;
        if (m_config.resource_root.empty())
            m_config.resource_root = "TT"; // an empty root would scatter dump/inject into the game folder

        // Toggles
        m_config.enable_injection = GetPrivateProfileIntW(L"TextureToolkit", L"EnableInjection", 1, ini_w) != 0;
        m_config.auto_dump = GetPrivateProfileIntW(L"TextureToolkit", L"AutoDump", 0, ini_w) != 0;
        m_config.filter_small_textures = GetPrivateProfileIntW(L"TextureToolkit", L"FilterSmallTextures", 1, ini_w) != 0;
        m_config.show_current_frame_only = GetPrivateProfileIntW(L"TextureToolkit", L"ShowCurrentFrameOnly", 1, ini_w) != 0;
        m_config.accept_sk_names = GetPrivateProfileIntW(L"TextureToolkit", L"AcceptSpecialKNames", 1, ini_w) != 0;

        // OSD
        m_config.show_osd_banner = GetPrivateProfileIntW(L"TextureToolkit", L"ShowOSDBanner", 1, ini_w) != 0;

        // Diagnostics
        m_config.verbose = GetPrivateProfileIntW(L"TextureToolkit", L"Verbose", 0, ini_w) != 0;

        Logger::get().info("[ConfigManager] Configuration loaded from " + m_ini_path.string());
    }

    void ConfigManager::save()
    {
        std::error_code ec;
        std::filesystem::create_directories(m_ini_path.parent_path(), ec);

        std::ofstream file(m_ini_path, std::ios::out | std::ios::trunc);
        if (!file.is_open())
            return;

        std::ostringstream ss;
        ss << "0x" << std::hex << std::uppercase << m_config.hotkey;

        file << "[TextureToolkit]\n"
             << "; Virtual Key Code for UI Toggle (0x2D = INSERT, 0x24 = HOME, 0x74 = F5)\n"
             << "HotKey=" << ss.str() << "\n\n"
             << "; Root folder for dump/, inject/, and imgui.ini.\n"
             << "; Relative to the game's executable folder, or an absolute path.\n"
             << "ResourceRoot=" << m_config.resource_root.string() << "\n\n"
             << "; Feature Toggles\n"
             << "EnableInjection=" << (m_config.enable_injection ? 1 : 0) << "\n"
             << "AutoDump=" << (m_config.auto_dump ? 1 : 0) << "\n"
             << "FilterSmallTextures=" << (m_config.filter_small_textures ? 1 : 0) << "\n"
             << "ShowCurrentFrameOnly=" << (m_config.show_current_frame_only ? 1 : 0) << "\n\n"
             << "; Also load texture packs named the way Special K names them (CRC-32C of the top mip)\n"
             << "AcceptSpecialKNames=" << (m_config.accept_sk_names ? 1 : 0) << "\n\n"
             << "; On-Screen Display (OSD)\n"
             << "ShowOSDBanner=" << (m_config.show_osd_banner ? 1 : 0) << "\n\n"
             << "; Diagnostics: 1 = verbose per-texture debug logging (slow)\n"
             << "Verbose=" << (m_config.verbose ? 1 : 0) << "\n";

        file.close();
        Logger::get().info("[ConfigManager] Configuration saved to " + m_ini_path.string());
    }
}
