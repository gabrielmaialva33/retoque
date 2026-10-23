#pragma once
// The minimum DlssNr_ExposureScan.cpp takes from OptiScaler's precompiled header: D3D12, the log macros and the
// internal-creation scope. The log is captured so cases can read what the scan said.
#include <d3d12.h>
#include <format>
#include <string>
#include <vector>

namespace TestLog
{
inline std::vector<std::string>& Lines()
{
    static std::vector<std::string> lines;
    return lines;
}
} // namespace TestLog

#define LOG_DEBUG(...) TestLog::Lines().push_back(std::format(__VA_ARGS__))
#define LOG_INFO(...) TestLog::Lines().push_back(std::format(__VA_ARGS__))
#define LOG_WARN(...) TestLog::Lines().push_back(std::format(__VA_ARGS__))
#define LOG_ERROR(...) TestLog::Lines().push_back(std::format(__VA_ARGS__))

class ScopedInternalResourceCreation
{
  public:
    static bool Active() { return false; }
};
