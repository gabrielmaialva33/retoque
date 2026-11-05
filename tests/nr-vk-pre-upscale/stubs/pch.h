#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
using LONG = long;
using PVOID = void*;
using HMODULE = void*;
constexpr LONG NO_ERROR = 0;
inline void* GetCurrentThread() { return nullptr; }
inline void* GetModuleHandleW(const wchar_t*) { return reinterpret_cast<void*>(1); }
#include "Config.h"
#include "hooks/Vulkan_Hooks.h"
