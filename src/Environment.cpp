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
        // A DLL's own name and version, from its version resource: what Explorer shows under
        // Properties > Details. Proxy DLLs are named after the system file they stand in for, so
        // this is the only way to tell Ultimate ASI Loader from ReShade from DXVK by file alone.
        struct FileVersion
        {
            std::string product;
            std::string version;
        };

        FileVersion file_version(const std::filesystem::path &path)
        {
            FileVersion out;
            DWORD handle = 0;
            const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
            if (size == 0)
                return out;
            std::vector<unsigned char> data(size);
            if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
                return out;

            // The numeric version, as "6.8.0" (a trailing .0 build dropped).
            VS_FIXEDFILEINFO *fixed = nullptr;
            UINT len = 0;
            if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&fixed), &len) && fixed != nullptr &&
                len >= sizeof(VS_FIXEDFILEINFO))
            {
                const unsigned v[4] = { HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
                                        HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS) };
                out.version = std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]);
                if (v[3] != 0)
                    out.version += "." + std::to_string(v[3]);
            }

            // ProductName in the first language the resource carries.
            struct Translation { WORD language, codepage; } *tr = nullptr;
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&tr), &len) &&
                tr != nullptr && len >= sizeof(Translation))
            {
                wchar_t key[64];
                swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\ProductName", tr->language, tr->codepage);
                wchar_t *name = nullptr;
                if (VerQueryValueW(data.data(), key, reinterpret_cast<void **>(&name), &len) && name != nullptr && len > 1)
                    out.product = path_utf8(std::filesystem::path(std::wstring(name, len - 1)));
            }
            return out;
        }

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
            {
                const FileVersion fv = file_version(path);
                add("ReShade" + (fv.version.empty() ? std::string() : " " + fv.version) + " (" + path_utf8(path.filename()) + ")");
            }
            else if (name.find(L".addon") != std::wstring::npos || name.find(L"reshade") != std::wstring::npos)
                add("ReShade add-on (" + path_utf8(path.filename()) + ")"); // .addon, .addon32, .addon64
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
                                     name == L"dxgi.dll" || name == L"dinput8.dll" || name == L"ddraw.dll" ||
                                     name == L"winmm.dll" || name == L"version.dll" || name == L"dsound.dll"))
            {
                // Say what the proxy actually is, when it says so itself: an ASI loader, DXVK, a
                // d3d8-to-d3d9 converter. Without a product name, at least where it sits.
                const FileVersion fv = file_version(path);
                std::string what = path_utf8(path.filename()) + " in the game folder";
                if (!fv.product.empty())
                    what += ": " + fv.product + (fv.version.empty() ? std::string() : " " + fv.version);
                add(what);
            }
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
