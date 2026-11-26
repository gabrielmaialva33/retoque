#include "pch.h"
#include <menu/MenuWindowOwnerPolicy.h>
#include <menu/NrStatusRules.h>
#include <cassert>
#include <mutex>
#include <unordered_map>
#include <iostream>

using HWND = int;
using UINT = unsigned;
using WPARAM = unsigned;
using LPARAM = long;
using LRESULT = long;
using WNDPROC = LRESULT (*)(HWND, UINT, WPARAM, LPARAM);
#define CALLBACK
#define OPTIINPUT_LOG_VERBOSE(...) ((void) 0)
#define GET_RAWINPUT_CODE_WPARAM(w) (w)
constexpr UINT WM_NULL = 0, WM_NCDESTROY = 1, WM_UNICHAR = 2, WM_INPUT = 3;
constexpr WPARAM UNICODE_NOCHAR = 0xffff, RIM_INPUT = 0;
constexpr LRESULT TRUE = 1;
enum class InputMessageSource
{
    WndProc
};
struct
{
    std::mutex Mutex;
    std::unordered_map<HWND, WNDPROC> OriginalWndProcs;
} _state;
static int handledCalls = 0, originalCalls = 0, defaultCalls = 0;
static bool consume = false;
static bool IsTargetWindow(HWND) { return true; }
static bool HandleWindowMessage(HWND, UINT, WPARAM, LPARAM, InputMessageSource)
{
    ++handledCalls;
    return consume;
}
static LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM)
{
    ++defaultCalls;
    return 17;
}
static LRESULT CallWindowProcW(WNDPROC proc, HWND hwnd, UINT msg, WPARAM w, LPARAM l) { return proc(hwnd, msg, w, l); }
static LRESULT Original(HWND hwnd, UINT msg, WPARAM, LPARAM)
{
    ++originalCalls;
    // Real window callbacks can re-enter input. The forwarding call must not hold its mutex.
    assert(_state.Mutex.try_lock());
    assert(_state.OriginalWndProcs.contains(hwnd)); // still present while the last message is forwarded
    _state.Mutex.unlock();
    return msg == WM_NCDESTROY ? 42 : 43;
}
