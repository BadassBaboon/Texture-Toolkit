#pragma once

#include <windows.h>
#include <string>
#include <filesystem>

namespace TextureToolkit
{
    // Full path of a loaded module (nullptr for the game's executable), however long it is.
    // A MAX_PATH buffer truncated an install path past 260 characters, and copying that path into
    // one with wcscpy_s ended the process at startup.
    inline std::wstring module_file_name(HMODULE module)
    {
        std::wstring buf(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
            if (n == 0)
                return {};
            if (n < buf.size())
            {
                buf.resize(n);
                return buf;
            }
            if (buf.size() >= 32768) // the longest path Windows has
                return {};
            buf.resize(buf.size() * 2);
        }
    }

    // Paths travel through this code as UTF-8 when they are narrow strings: for the log, for
    // ImGui, and between the texture manager and the DDS reader and writer. path::string()
    // converts through the ANSI code page instead and throws on any character that page lacks,
    // such as a Cyrillic or Japanese folder name on an English Windows.
    inline std::string path_utf8(const std::filesystem::path &p)
    {
        const std::wstring &w = p.native();
        if (w.empty())
            return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
        if (n > 0)
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    inline std::filesystem::path path_from_utf8(const std::string &s)
    {
        if (s.empty())
            return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
        if (n > 0)
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
        return std::filesystem::path(out);
    }
}
