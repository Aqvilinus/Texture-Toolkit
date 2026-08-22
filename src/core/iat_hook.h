#pragma once

#include <windows.h>
#include <string>

namespace TextureToolkit
{
    class IATHook
    {
    public:
        static bool hook_import(HMODULE module, const char *dll_name, const char *func_name, void *new_func, void **orig_func);
        static void hook_all_modules(const char *dll_name, const char *func_name, void *new_func, void **orig_func);
    };
}
