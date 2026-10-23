#pragma once
// Host prelude for the production menu_overlay_vk.cpp: Win32 types, logging and State as fakes; the
// Vulkan types and prototypes are the real Khronos headers, defined by fakes.cpp.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

typedef struct HWND__* HWND;
typedef unsigned int UINT;
typedef unsigned long long UINT64;
typedef unsigned long long ULONG64;
typedef int BOOL;

BOOL IsWindow(HWND hwnd);

// The event log every fake writes to. Cases assert on its order.
void Ev(const std::string& event);
void FakeLog(const char* level, const char* format);

#define LOG_FUNC() ((void) 0)
#define LOG_FUNC_RESULT(...) ((void) 0)
#define LOG_TRACE(...) ((void) 0)
#define LOG_DEBUG(...) ((void) 0)
#define LOG_INFO(...) ((void) 0)
#define LOG_WARN(fmt, ...) FakeLog("WARN", fmt)
#define LOG_ERROR(fmt, ...) FakeLog("ERROR", fmt)

enum class API
{
    NotSelected,
    DX11,
    DX12,
    Vulkan,
};

struct State
{
    bool menuOverlayIsVulkan = false;
    bool isShuttingDown = false;
    API swapchainApi = API::NotSelected;

    static State& Instance()
    {
        static State state;
        return state;
    }
};
