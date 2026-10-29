#pragma once

// One OptiScaler per process. Two proxy names in one game folder (dxgi.dll and winmm.dll, say) load two copies whose
// hooks fight over LoadLibraryExW, CreateDXGIFactory and the rest until the game dies before its engine starts
// (Divinity, 2026-09-19: CLAUDE.md, "One proxy per game directory"). The first copy to attach claims the process with a
// named mutex. A later one sees the claim and runs in pass-through, upstream's mode for excluded processes: it only
// forwards its exports to the real system DLL. It cannot log (opening the logger would rename the first copy's live
// OptiScaler.log to .previous.log), so it leaves its path in a per-process environment variable, and the first copy
// says so in its own log once the game has made its first DXGI factory (IdentifyGpu's GPU list), after every
// statically imported copy has attached. Idea from Autopilot 2.0.8 and DLSS5VKLayer 583832e; no code taken.

#include <windows.h>

#include <cwchar>
#include <string>

namespace InstanceClaim
{

// Per process, so neither the claim nor the note reaches a child process that inherits the environment
inline std::wstring ClaimName() { return L"Local\\OptiScaler.Instance." + std::to_wstring(GetCurrentProcessId()); }
inline std::wstring NoteName() { return L"RETOQUE_INERT_OPTISCALER_" + std::to_wstring(GetCurrentProcessId()); }

// True when another OptiScaler module of this process attached first. A successful claim's handle is never closed:
// it is the claim, for the life of the process. A claim that cannot be made at all answers false, as before this
// existed.
inline bool AnotherCopyOwnsProcess()
{
    HANDLE claim = CreateMutexW(nullptr, FALSE, ClaimName().c_str());

    if (claim == nullptr)
        return false;

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(claim);
        return true;
    }

    return false;
}

// The pass-through copy's note for the first one: its module path, appended to the per-process variable
inline void NoteInertCopy(const std::wstring& modulePath)
{
    std::wstring value;
    wchar_t current[4096] = {};
    const DWORD length = GetEnvironmentVariableW(NoteName().c_str(), current, 4096);

    if (length > 0 && length < 4096)
        value.assign(current, length).append(L";");

    value.append(modulePath);
    SetEnvironmentVariableW(NoteName().c_str(), value.c_str());
    OutputDebugStringW(
        (L"OptiScaler: another copy already runs in this process; " + modulePath + L" only forwards its exports\n")
            .c_str());
}

// The copies that stood down in this process, ';'-separated; empty when there were none
inline std::wstring InertCopies()
{
    wchar_t current[4096] = {};
    const DWORD length = GetEnvironmentVariableW(NoteName().c_str(), current, 4096);
    return length > 0 && length < 4096 ? std::wstring(current, length) : std::wstring();
}

} // namespace InstanceClaim
