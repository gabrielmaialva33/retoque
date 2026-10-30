// The Win32 surface retoque/menu/input/native_key_state.h and RealGetAsyncKeyStateSafe use. cases.cpp defines them.
#pragma once
#include <cstdint>
#define WINAPI
using SHORT = short;
using HMODULE = struct HINSTANCE__*;
using FARPROC = void (*)(void); // matches every function type, as the real one is cast
HMODULE GetModuleHandleW(const wchar_t* name);
FARPROC GetProcAddress(HMODULE module, const char* name);
SHORT WINAPI GetAsyncKeyState(int vk);
