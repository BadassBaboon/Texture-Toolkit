#include "Environment.h"
#include "PathUtil.h"

#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <vector>

namespace TextureToolkit
{
    namespace
    {
        std::string join(const std::vector<std::string> &items)
        {
            std::string out;
            for (size_t i = 0; i < items.size(); ++i)
                out += (i ? ", " : "") + items[i];
            return out;
        }
    }

    std::string describe_other_software()
    {
        HMODULE modules[1024] = {};
        DWORD needed = 0;
        if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
            return "could not list modules";
        const size_t count = (std::min)(static_cast<size_t>(needed / sizeof(HMODULE)), std::size(modules));

        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&describe_other_software), &self);
        const std::wstring game_dir = std::filesystem::path(module_file_name(nullptr)).parent_path().wstring();

        std::vector<std::string> found;
        const auto add = [&found](const std::string &what)
        {
            if (std::find(found.begin(), found.end(), what) == found.end())
                found.push_back(what);
        };
        for (size_t i = 0; i < count; ++i)
        {
            if (modules[i] == self)
                continue;
            const std::filesystem::path path(module_file_name(modules[i]));
            std::wstring name = path.filename().wstring();
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            const bool in_game_dir = _wcsicmp(path.parent_path().wstring().c_str(), game_dir.c_str()) == 0;

            if (GetProcAddress(modules[i], "ReShadeRegisterAddon") != nullptr)
                add("ReShade (" + path_utf8(path.filename()) + ")");
            else if (name.find(L"reshade") != std::wstring::npos)
                add("ReShade add-on");
            else if (name == L"specialk32.dll" || name == L"specialk64.dll")
                add("Special K");
            else if (name.rfind(L"rtsshooks", 0) == 0)
                add("RivaTuner Statistics Server");
            else if (name.rfind(L"gameoverlayrenderer", 0) == 0)
                add("Steam overlay");
            else if (name.rfind(L"discordhook", 0) == 0)
                add("Discord overlay");
            else if (name.rfind(L"graphics-hook", 0) == 0)
                add("OBS game capture");
            else if (in_game_dir && (name == L"d3d9.dll" || name == L"d3d8.dll" || name == L"d3d11.dll" ||
                                     name == L"dxgi.dll" || name == L"dinput8.dll" || name == L"ddraw.dll"))
                add(path_utf8(path.filename()) + " in the game folder");
        }
        return found.empty() ? std::string("none detected") : join(found);
    }

    std::string describe_unhooked_apis()
    {
        // Loaded, not necessarily used: OpenGL in particular is pulled in by plenty that never
        // draws with it. Worded as a lead, not a verdict, where it is reported.
        std::vector<std::string> found;
        if (GetModuleHandleW(L"d3d12.dll") != nullptr)
            found.push_back("Direct3D 12");
        if (GetModuleHandleW(L"vulkan-1.dll") != nullptr)
            found.push_back("Vulkan");
        if (GetModuleHandleW(L"d3d10.dll") != nullptr || GetModuleHandleW(L"d3d10_1.dll") != nullptr)
            found.push_back("Direct3D 10");
        if (GetModuleHandleW(L"d3d8.dll") != nullptr)
            found.push_back("Direct3D 8");
        if (GetModuleHandleW(L"opengl32.dll") != nullptr)
            found.push_back("OpenGL");
        return join(found);
    }
}
