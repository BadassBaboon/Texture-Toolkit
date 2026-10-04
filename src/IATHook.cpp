#include "IATHook.h"
#include "Logger.h"
#include <cstring>

namespace TextureToolkit
{
    bool IATHook::hook_import(HMODULE module, const char *dll_name, const char *func_name, void *new_func, void **orig_func)
    {
        if (module == nullptr || dll_name == nullptr || func_name == nullptr || new_func == nullptr)
            return false;

        PIMAGE_DOS_HEADER dos_header = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
        if (dos_header->e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        PIMAGE_NT_HEADERS nt_headers = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<BYTE *>(module) + dos_header->e_lfanew);
        if (nt_headers->Signature != IMAGE_NT_SIGNATURE)
            return false;

        IMAGE_DATA_DIRECTORY import_dir = nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (import_dir.VirtualAddress == 0 || import_dir.Size == 0)
            return false;

        PIMAGE_IMPORT_DESCRIPTOR import_desc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(reinterpret_cast<BYTE *>(module) + import_dir.VirtualAddress);

        for (; import_desc->Name != 0; ++import_desc)
        {
            const char *mod_name = reinterpret_cast<const char *>(reinterpret_cast<BYTE *>(module) + import_desc->Name);
            if (_stricmp(mod_name, dll_name) != 0)
                continue;
            // Some old linkers leave out the name table and keep only the bound addresses, which
            // say nothing about which function is which. Nothing to match by: leave it alone.
            if (import_desc->OriginalFirstThunk == 0)
                continue;

            PIMAGE_THUNK_DATA orig_thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(reinterpret_cast<BYTE *>(module) + import_desc->OriginalFirstThunk);
            PIMAGE_THUNK_DATA thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(reinterpret_cast<BYTE *>(module) + import_desc->FirstThunk);

            for (; thunk->u1.Function != 0; ++thunk, ++orig_thunk)
            {
                // Imports by ordinal carry no name to match.
                bool match = false;
                if (!IMAGE_SNAP_BY_ORDINAL(orig_thunk->u1.Ordinal))
                {
                    PIMAGE_IMPORT_BY_NAME import_by_name = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(reinterpret_cast<BYTE *>(module) + orig_thunk->u1.AddressOfData);
                    match = std::strcmp(import_by_name->Name, func_name) == 0;
                }

                if (match)
                {
                    DWORD old_protect = 0;
                    if (VirtualProtect(&thunk->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &old_protect))
                    {
                        if (orig_func && *orig_func == nullptr)
                        {
                            *orig_func = reinterpret_cast<void *>(thunk->u1.Function);
                        }
                        thunk->u1.Function = reinterpret_cast<ULONG_PTR>(new_func);
                        VirtualProtect(&thunk->u1.Function, sizeof(ULONG_PTR), old_protect, &old_protect);

                        Logger::get().info("[IATHook] Successfully patched IAT import " + std::string(dll_name) + "!" + std::string(func_name));
                        return true;
                    }
                }
            }
        }

        return false;
    }

    void IATHook::hook_game_exe(const char *dll_name, const char *func_name, void *new_func, void **orig_func)
    {
        HMODULE main_mod = GetModuleHandleA(nullptr);
        hook_import(main_mod, dll_name, func_name, new_func, orig_func);
    }
}
