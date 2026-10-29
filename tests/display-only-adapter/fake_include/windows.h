#pragma once
#include <cstdint>
using UINT = unsigned int;
using LONG = long;
using DWORD = unsigned long;
using NTSTATUS = long;
struct LUID
{
    DWORD LowPart;
    LONG HighPart;
};
