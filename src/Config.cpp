#include "Config.h"
#include "Logger.h"
#include <fstream>
#include <sstream>
#include <cstdio>
#include <algorithm>

#include <imgui.h>

// Defined in imgui_impl_win32.cpp with external linkage, but not declared in its header, so it is
// declared here rather than by reaching into the backend's source.
ImGuiKey ImGui_ImplWin32_KeyEventToImGuiKey(WPARAM wParam, LPARAM lParam);

namespace
{
    // The ini is written as an ANSI file, which is how GetPrivateProfileStringW reads one back.
    std::string to_ini_text(const std::wstring &w)
    {
        if (w.empty())
            return {};
        const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>((std::max)(n, 0)), '\0');
        if (n > 0)
            WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    std::wstring trim(const std::wstring &s)
    {
        const size_t b = s.find_first_not_of(L" \t\r\n");
        if (b == std::wstring::npos)
            return {};
        const size_t e = s.find_last_not_of(L" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    // "DualShock; inject ;;DarkMode" -> {DualShock, inject, DarkMode}: pieces trimmed, empty and
    // repeated ones dropped, so a hand-edited list cannot produce a blank or doubled entry.
    std::vector<std::wstring> split_list(const std::wstring &value)
    {
        std::vector<std::wstring> out;
        size_t start = 0;
        while (start <= value.size())
        {
            size_t end = value.find(L';', start);
            if (end == std::wstring::npos)
                end = value.size();
            const std::wstring piece = trim(value.substr(start, end - start));
            bool dup = false;
            for (const std::wstring &o : out)
                dup |= (_wcsicmp(o.c_str(), piece.c_str()) == 0);
            if (!piece.empty() && !dup)
                out.push_back(piece);
            start = end + 1;
        }
        return out;
    }

    std::string join_list(const std::vector<std::wstring> &items)
    {
        std::string out;
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (i != 0)
                out += ';';
            out += to_ini_text(items[i]);
        }
        return out;
    }
}

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

        // Mods: load order, and the per-mod switches the user has set from the panel.
        {
            std::vector<wchar_t> buf(32768, L'\0');
            GetPrivateProfileStringW(L"Mods", L"LoadOrder", L"", buf.data(), static_cast<DWORD>(buf.size()), ini_w);
            m_config.mod_load_order = split_list(buf.data());

            // A section comes back as "key=value\0key=value\0\0".
            std::fill(buf.begin(), buf.end(), L'\0');
            GetPrivateProfileSectionW(L"ModEnabled", buf.data(), static_cast<DWORD>(buf.size()), ini_w);
            m_config.mod_enabled.clear();
            for (const wchar_t *entry = buf.data(); *entry != L'\0'; entry += wcslen(entry) + 1)
            {
                const std::wstring line(entry);
                const size_t eq = line.find(L'=');
                if (eq == std::wstring::npos)
                    continue;
                const std::wstring key = trim(line.substr(0, eq));
                const std::wstring value = trim(line.substr(eq + 1));
                // Comment lines can come back as entries too, and ours has an '=' in it.
                if (!key.empty() && key[0] != L';' && key[0] != L'#')
                    m_config.mod_enabled[key] = parse_ini_bool(value, true);
            }
        }

        Logger::get().info("[ConfigManager] Configuration loaded from " + m_ini_path.string());

        // The values, not just the path. Every diagnosis from a user's log has to start by knowing
        // what the settings were: a texture missing from the panel because ShowCurrentFrameOnly is
        // hiding it looks exactly like a texture that was never tracked at all.
        Logger::get().info(std::string("[ConfigManager] HotKey=") + hotkey_name(m_config.hotkey) +
                           " ResourceRoot=" + m_config.resource_root.string() +
                           " EnableInjection=" + (m_config.enable_injection ? "1" : "0") +
                           " AutoDump=" + (m_config.auto_dump ? "1" : "0") +
                           " FilterSmallTextures=" + (m_config.filter_small_textures ? "1" : "0") +
                           " ShowCurrentFrameOnly=" + (m_config.show_current_frame_only ? "1" : "0") +
                           " AcceptSpecialKNames=" + (m_config.accept_sk_names ? "1" : "0") +
                           " ShowOSDBanner=" + (m_config.show_osd_banner ? "1" : "0") +
                           " Verbose=" + (m_config.verbose ? "1" : "0"));
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
             << "Verbose=" << (m_config.verbose ? 1 : 0) << "\n\n"
             << "[Mods]\n"
             << "; Every folder in ResourceRoot other than dump and inject is a texture mod.\n"
             << "; Load order, highest priority first, separated by ';'. \"inject\" is the inject folder.\n"
             << "; Mods not listed load after the ones that are. Set from the panel's Mod files page.\n"
             << "LoadOrder=" << join_list(m_config.mod_load_order) << "\n\n"
             << "[ModEnabled]\n"
             << "; <mod folder>=1 or 0 switches a mod on or off, overriding its own mod.ini default.\n";
        for (const auto &kv : m_config.mod_enabled)
            file << to_ini_text(kv.first) << "=" << (kv.second ? 1 : 0) << "\n";

        file.close();
        // The values on save as well as on load. A user toggling a checkbox mid-session and a user
        // never touching it produced the same line, so a log could not say which setting was in
        // force when the thing they were reporting happened.
        Logger::get().info("[ConfigManager] Configuration saved to " + m_ini_path.string());
        Logger::get().info(std::string("[ConfigManager] Now: EnableInjection=") + (m_config.enable_injection ? "1" : "0") +
                           " AutoDump=" + (m_config.auto_dump ? "1" : "0") +
                           " FilterSmallTextures=" + (m_config.filter_small_textures ? "1" : "0") +
                           " ShowCurrentFrameOnly=" + (m_config.show_current_frame_only ? "1" : "0") +
                           " AcceptSpecialKNames=" + (m_config.accept_sk_names ? "1" : "0") +
                           " Verbose=" + (m_config.verbose ? "1" : "0"));
    }
}
