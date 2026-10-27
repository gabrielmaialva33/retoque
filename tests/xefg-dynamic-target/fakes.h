// Fakes for the two production slices run.py splices in after this file: XeFG_DynamicMFG::TargetFps
// (OptiScaler/framegen/xefg/XeFG_DynamicMFG.cpp) and FGPresent's sync interval block (OptiScaler/hooks/FG_Hooks.cpp).
// XeFG_DynamicCount.h and XeFG_DynamicCap.h are the real headers.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <optional>

#include "XeFG_DynamicCap.h"
#include "XeFG_DynamicCount.h"

using UINT = unsigned int;
using HWND = void*;
constexpr UINT DXGI_PRESENT_ALLOW_TEARING = 0x200;

template <typename T> struct Option
{
    T value {};
    T value_or_default() const { return value; }
};

struct Config
{
    Option<float> FramerateLimit;            // [Framerate] FramerateLimit, 0 = none
    Option<float> FGXeFGFramerateTargetDMFG; // [XeFG] FramerateTargetDMFG, 0 = auto
    std::optional<bool> ForceVsync;          // [Framerate] ForceVsync, unset = the game's own interval
    Option<UINT> VsyncInterval;

    static Config* Instance()
    {
        static Config c;
        return &c;
    }
};

struct State
{
    std::atomic<bool> fgPresentVsync { false };
    bool SCAllowTearing = false;
    bool realExclusiveFullscreen = false;

    static State& Instance()
    {
        static State s;
        return s;
    }
};

#define LOG_DEBUG(...) ((void) 0)

// Only what TargetFps reaches: the refresh rate it is handed, counted
class XeFG_DynamicMFG
{
  public:
    float refresh = 0.0f;
    int refreshQueries = 0;

    float DisplayRefreshFps(double, HWND)
    {
        ++refreshQueries;
        return refresh;
    }

    float TargetFps(double nowMs, HWND window);
};
