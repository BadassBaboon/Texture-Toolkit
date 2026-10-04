#pragma once

#include <windows.h>
#include <string>

namespace TextureToolkit
{
    class IATHook
    {
    public:
        static bool hook_import(HMODULE module, const char *dll_name, const char *func_name, void *new_func, void **orig_func);

        // hook_import on the game's executable. Other modules keep their own imports: the inline
        // hook on the export itself is what reaches them.
        static void hook_game_exe(const char *dll_name, const char *func_name, void *new_func, void **orig_func);
    };
}
