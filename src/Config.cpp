#include "Config.h"
#include "Logger.h"
#include <fstream>
#include <sstream>
#include <cstdio>

#include <imgui.h>

// Defined in imgui_impl_win32.cpp with external linkage, but not declared in its header, so it is
// declared here rather than by reaching into the backend's source.
ImGuiKey ImGui_ImplWin32_KeyEventToImGuiKey(WPARAM wParam, LPARAM lParam);

namespace
{
    // Splits "dualshock; darkmode ;;other" into {"dualshock", "darkmode", "other"}: trims each
    // piece and drops anything that comes out empty, so a stray leading/trailing/doubled ';' in a
    // hand-edited ini cannot produce a blank search path. Works on the wide string GetPrivateProfileStringW
    // hands back, rather than transcoding it to narrow first, so a non-ASCII folder name survives.
    std::vector<std::filesystem::path> split_search_paths(const std::wstring &value)
    {
        std::vector<std::filesystem::path> result;
        std::wstringstream ss(value);
        std::wstring piece;
        while (std::getline(ss, piece, L';'))
        {
            const size_t begin = piece.find_first_not_of(L" \t\r\n");
            if (begin == std::wstring::npos)
                continue;
            const size_t end = piece.find_last_not_of(L" \t\r\n");
            result.push_back(piece.substr(begin, end - begin + 1));
        }
        return result;
    }

    // For the ini template and the log; ini writing is narrow throughout (see ResourceRoot below),
    // so this is no more lossy for a non-ASCII folder name than that already is.
    std::string join_search_paths(const std::vector<std::filesystem::path> &paths)
    {
        std::string result;
        for (size_t i = 0; i < paths.size(); ++i)
        {
            if (i != 0)
                result += ';';
            result += paths[i].string();
        }
        return result;
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

        // Optional overlay folders, checked before inject/ itself, in the order listed. See Config.h.
        {
            wchar_t search_path_str[4096] = L"";
            GetPrivateProfileStringW(L"TextureToolkit", L"AdditionalSearchPath", L"", search_path_str, 4096, ini_w);
            m_config.additional_search_paths = split_search_paths(search_path_str);
        }

        // Hash algorithm: 0 (current), 1 (v1.0 legacy), 2 (legacy + migration file). See Config.h.
        m_config.hash_algorithm = GetPrivateProfileIntW(L"TextureToolkit", L"HashAlgorithm", 0, ini_w);
        if (m_config.hash_algorithm < 0 || m_config.hash_algorithm > 2)
        {
            Logger::get().warn("[ConfigManager] HashAlgorithm=" + std::to_string(m_config.hash_algorithm) +
                               " is not 0, 1, or 2; falling back to 0 (current algorithm).");
            m_config.hash_algorithm = 0;
        }

        // OSD
        m_config.show_osd_banner = GetPrivateProfileIntW(L"TextureToolkit", L"ShowOSDBanner", 1, ini_w) != 0;

        // Diagnostics
        m_config.verbose = GetPrivateProfileIntW(L"TextureToolkit", L"Verbose", 0, ini_w) != 0;

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
                           " AdditionalSearchPath=" + join_search_paths(m_config.additional_search_paths) +
                           " HashAlgorithm=" + std::to_string(m_config.hash_algorithm) +
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
             << "; Optional overlay folders\n"
             << "; A semi-colon separate list of folders (relative to `inject/` or absolute) that are\n"
             << "; also checked (in order) for textures.  If a texture is not found in any overlay folder\n"
             << "; then it falls back to the normal `inject/` folder.\n"
             << "AdditionalSearchPath=" << join_search_paths(m_config.additional_search_paths) << "\n\n"
             << "; Which hash algorithm to use\n"
             << "; 0 = 64-bit (v1.1)\n"
             << "; 1 = 32-bit (v1.0)\n"
             << "; 2 = 32-bit + write hash_migrate.txt to <ResourceRoot>\n"
             << "HashAlgorithm=" << m_config.hash_algorithm << "\n\n"
             << "; On-Screen Display (OSD)\n"
             << "ShowOSDBanner=" << (m_config.show_osd_banner ? 1 : 0) << "\n\n"
             << "; Diagnostics: 1 = verbose per-texture debug logging (slow)\n"
             << "Verbose=" << (m_config.verbose ? 1 : 0) << "\n";

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
                           " AdditionalSearchPath=" + join_search_paths(m_config.additional_search_paths) +
                           " HashAlgorithm=" + std::to_string(m_config.hash_algorithm) +
                           " Verbose=" + (m_config.verbose ? "1" : "0"));
    }
}
