// The Win32 surface retoque/misc/InstanceClaim.h uses, backed by maps a case can inspect and reset.
#pragma once
#include <cstdint>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

using DWORD = unsigned long;
using HANDLE = void*;
using BOOL = int;
using LPCWSTR = const wchar_t*;
#define FALSE 0
#define ERROR_ALREADY_EXISTS 183L

struct FakeWin
{
    DWORD pid = 100;
    DWORD lastError = 0;
    bool mutexFails = false;
    std::map<std::wstring, int> mutexes;           // name -> open handles
    std::map<std::wstring, std::wstring> env;      // the process environment
    std::vector<std::wstring> debugStrings;
    std::vector<std::wstring> handleNames;         // index + 1 is the handle value
};
inline FakeWin g_win;

inline DWORD GetCurrentProcessId() { return g_win.pid; }
inline DWORD GetLastError() { return g_win.lastError; }

inline HANDLE CreateMutexW(void*, BOOL, LPCWSTR name)
{
    if (g_win.mutexFails)
    {
        g_win.lastError = 5;
        return nullptr;
    }
    auto& count = g_win.mutexes[name];
    g_win.lastError = count > 0 ? ERROR_ALREADY_EXISTS : 0;
    ++count;
    g_win.handleNames.push_back(name);
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(g_win.handleNames.size()));
}

inline BOOL CloseHandle(HANDLE handle)
{
    const auto index = reinterpret_cast<uintptr_t>(handle) - 1;
    --g_win.mutexes[g_win.handleNames.at(index)];
    return 1;
}

inline DWORD GetEnvironmentVariableW(LPCWSTR name, wchar_t* buffer, DWORD size)
{
    auto it = g_win.env.find(name);
    if (it == g_win.env.end())
        return 0;
    if (it->second.size() + 1 > size)
        return static_cast<DWORD>(it->second.size() + 1);
    std::wcscpy(buffer, it->second.c_str());
    return static_cast<DWORD>(it->second.size());
}

inline BOOL SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value)
{
    g_win.env[name] = value;
    return 1;
}

inline void OutputDebugStringW(LPCWSTR text) { g_win.debugStrings.push_back(text); }
