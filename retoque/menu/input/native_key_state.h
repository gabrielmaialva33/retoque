#pragma once

#include <windows.h>

namespace OptiInput
{
// Physical key state for the menu's own polling, beneath every GetAsyncKeyState detour in the process. user32's
// GetAsyncKeyState is the game-facing chain: another overlay hooked into it (REFramework's, which reports keys up while
// its UI wants input) also hides the menu key from us, whether its detour sits above ours or our trampoline leads into
// it. win32u's NtUserGetAsyncKeyState is the system call user32 makes underneath. Null where win32u lacks the export
// (Windows before 10 1607); the caller keeps the user32 path there. From tig3rmast3r/OptiScaler_DLSSNR_VR 3054caf2
// ("Acquire physical menu input independently of game API hooks"), GPL-3.0; that fork returns 0 without the export.
using NativeAsyncKeyState = SHORT(WINAPI*)(int);

inline NativeAsyncKeyState NativeAsyncKeyStateReader()
{
    static const auto reader = []() -> NativeAsyncKeyState
    {
        const auto module = GetModuleHandleW(L"win32u.dll");
        return module == nullptr
                   ? nullptr
                   : reinterpret_cast<NativeAsyncKeyState>(GetProcAddress(module, "NtUserGetAsyncKeyState"));
    }();

    return reader;
}
} // namespace OptiInput
