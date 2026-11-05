#pragma once
void* exported(void*, const char*);
namespace KernelBaseProxy
{
inline auto GetProcAddress_() { return exported; }
} // namespace KernelBaseProxy
