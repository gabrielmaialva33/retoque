// Host stand-ins for the Windows pieces DlssNr_Replay.cpp touches.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#define __declspec(x)
using DWORD = unsigned long;
inline DWORD GetEnvironmentVariableA(const char* name, char* buffer, DWORD size)
{
    const char* value = std::getenv(name);
    if (value == nullptr)
        return 0;
    const DWORD length = static_cast<DWORD>(std::strlen(value));
    if (length + 1 > size)
        return length + 1; // too small: Windows reports the size needed and writes nothing
    std::memcpy(buffer, value, length + 1);
    return length;
}
constexpr size_t _TRUNCATE = static_cast<size_t>(-1);
template <size_t N> inline int strncpy_s(char (&dst)[N], const char* src, size_t)
{
    std::strncpy(dst, src, N - 1);
    dst[N - 1] = '\0';
    return 0;
}
