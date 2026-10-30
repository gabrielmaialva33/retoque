// RealGetAsyncKeyStateSafe (OptiScaler/menu/input/input_system_detours.cpp, spliced in by run.py after this prelude)
// with the real retoque/menu/input/native_key_state.h, against a fake win32u and a fake user32 detour chain in which
// another overlay (REFramework's GetAsyncKeyState detour) reports every key up while the key is physically held.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#include <menu/input/native_key_state.h>

#define LOG_INFO(...) ((void) 0)

namespace fake
{
bool win32uExports = true;
bool held[256] = {};
int nativeCalls = 0, chainCalls = 0, bypassed = 0;
int bypassDepth = 0;
char module = 0;

// win32u!NtUserGetAsyncKeyState: the physical state
SHORT WINAPI NtUserGetAsyncKeyState(int vk) { return ++nativeCalls, held[vk] ? SHORT(0x8000) : SHORT(0); }

// The next detour in the user32 chain, as our own detour's trampoline (o_GetAsyncKeyState) calls it: an overlay that
// swallows input while its UI wants it
SHORT WINAPI OverlayDetour(int) { return ++chainCalls, bypassed += bypassDepth > 0, SHORT(0); }
} // namespace fake

HMODULE GetModuleHandleW(const wchar_t* name)
{
    return std::wcscmp(name, L"win32u.dll") == 0 ? reinterpret_cast<HMODULE>(&fake::module) : nullptr;
}

FARPROC GetProcAddress(HMODULE module, const char* name)
{
    if (module != reinterpret_cast<HMODULE>(&fake::module) || !fake::win32uExports ||
        std::strcmp(name, "NtUserGetAsyncKeyState") != 0)
        return nullptr;
    return reinterpret_cast<FARPROC>(&fake::NtUserGetAsyncKeyState);
}

SHORT WINAPI GetAsyncKeyState(int vk) { return fake::OverlayDetour(vk); }

namespace OptiInput
{
using GetAsyncKeyState_t = SHORT(WINAPI*)(int);
GetAsyncKeyState_t o_GetAsyncKeyState = fake::OverlayDetour;

class ScopedHookBypass
{
  public:
    ScopedHookBypass() { ++fake::bypassDepth; }
    ~ScopedHookBypass() { --fake::bypassDepth; }
};
} // namespace OptiInput
