// What dllmain's NvngxDlssPath block reads and writes, and the logging it uses.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#define LOG_DEBUG(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)

struct Config
{
    std::optional<std::wstring> NVNGX_DLSS_Library; // [Libraries] NvngxDlssPath, unset = auto

    static Config* Instance()
    {
        static Config c;
        return &c;
    }
};

// std::optional<std::wstring> in State.h. On Windows a path converts to std::wstring implicitly; here it does not, so
// the stand-in takes either, which lets the block compile as it is on both sides of the fix.
struct WidePath : std::optional<std::wstring>
{
    using std::optional<std::wstring>::operator=;
    WidePath& operator=(const std::filesystem::path& path)
    {
        std::optional<std::wstring>::operator=(path.wstring());
        return *this;
    }
};

struct State
{
    WidePath NVNGX_DLSS_Path;

    static State& Instance()
    {
        static State s;
        return s;
    }
};
