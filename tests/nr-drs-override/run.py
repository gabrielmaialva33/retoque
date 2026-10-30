#!/usr/bin/env python3
"""Compile the production NvAPI_DRS_GetSetting hook with a fake driver, under ASan/UBSan."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
source = (root / 'OptiScaler/nvapi/NvApiHooks.cpp').read_text()


def definition(text, signature):
    """The one definition that starts with `signature`, braces matched outside comments."""
    assert text.count(signature) == 1, f'production boundary changed: {signature}'
    begin = text.index(signature)
    depth, j = 0, text.index('{', begin)
    while True:
        if text.startswith('//', j):
            j = text.index('\n', j)
            continue
        depth += {'{': 1, '}': -1}.get(text[j], 0)
        if depth == 0:
            return text[begin:j + 1] + '\n'
        j += 1


hook = definition(source, 'NvAPI_Status __stdcall NvApiHooks::hkNvAPI_DRS_GetSetting(')
# The fork-owned headers the production file includes, exactly as it includes them (host-safe by contract).
fork_includes = ''.join(line + '\n' for line in source.splitlines() if re.match(r'#include <dlssnr/\w+\.h>$', line))

unit = """
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>
// MSVC's NvU32 (nvapi_lite_common.h, no clang, not unix): unsigned long, a type distinct from uint32_t, so the
// slice fails to compile where production would on Windows, e.g. binding u32CurrentValue to a uint32_t&.
using NvU32 = unsigned long;
#include <NvApiDriverSettings.h>
#define __stdcall
// LOG_* as Logger.h has them, recording the level, the format and the values instead of formatting them.
struct LogRecord
{
    std::string level, format;
    std::vector<std::string> values;
};
inline std::vector<LogRecord> logRecords;
inline std::string Text(const char* value) { return value; }
inline std::string Text(const std::string& value) { return value; }
template <typename T> std::enable_if_t<std::is_arithmetic_v<T> || std::is_enum_v<T>, std::string> Text(T value)
{
    return std::to_string(static_cast<unsigned long long>(value));
}
template <typename... Values> void Record(const char* level, const char* format, const Values&... values)
{
    logRecords.push_back({ level, format, { Text(values)... } });
}
#define LOG_TRACE(msg, ...) Record("trace", msg, ##__VA_ARGS__)
#define LOG_DEBUG(msg, ...) Record("debug", msg, ##__VA_ARGS__)
#define LOG_INFO(msg, ...) Record("info", msg, ##__VA_ARGS__)
#define LOG_WARN(msg, ...) Record("warn", msg, ##__VA_ARGS__)
""" + fork_includes + """
// nvapi.h, the parts the hook reads.
enum NvAPI_Status : int
{
    NVAPI_OK = 0,
    NVAPI_ERROR = -1,
    NVAPI_SETTING_NOT_FOUND = -160,
};
using NvDRSSessionHandle = struct NvDRSSession_*;
using NvDRSProfileHandle = struct NvDRSProfile_*;
enum NVDRS_SETTING_TYPE
{
    NVDRS_DWORD_TYPE,
    NVDRS_BINARY_TYPE,
    NVDRS_STRING_TYPE,
    NVDRS_WSTRING_TYPE,
};
struct NVDRS_SETTING
{
    NvU32 version;
    NvU32 settingId;
    NVDRS_SETTING_TYPE settingType;
    NvU32 isCurrentPredefined;
    NvU32 isPredefinedValid;
    union
    {
        NvU32 u32PredefinedValue;
        unsigned short wszPredefinedValue[8];
    };
    union
    {
        NvU32 u32CurrentValue;
        unsigned short wszCurrentValue[8];
    };
};

enum class FGOutput : std::uint32_t
{
    NoFG,
    FSRFG,
    DLSSG,
};
struct State
{
    FGOutput activeFgOutput = FGOutput::NoFG;
    bool dlssPresetsOverriddenExternally = false;
    std::uint32_t dlssRenderPresetExternal = 0;
    bool dlssdPresetsOverriddenExternally = false;
    std::uint32_t dlssdRenderPresetExternal = 0;
    static State& Instance()
    {
        static State state;
        return state;
    }
};
template <typename T> struct Option
{
    T value;
    T value_or_default() const { return value; }
};
struct Config
{
    Option<bool> UpscaleRatioOverrideEnabled { false };
    Option<float> UpscaleRatioOverrideValue { 1.5f };
    Option<bool> DlssNrEnabled { false };
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};

class NvApiHooks
{
  public:
    inline static NvAPI_Status (*o_NvAPI_DRS_GetSetting)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32,
                                                         NVDRS_SETTING*) = nullptr;
    static NvAPI_Status hkNvAPI_DRS_GetSetting(NvDRSSessionHandle hSession, NvDRSProfileHandle hProfile,
                                               NvU32 settingId, NVDRS_SETTING* pSetting);
};
""" + hook + (here / 'cases.h').read_text()

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-drs-override-') as directory:
    cpp = Path(directory) / 'drs.cpp'
    binary = Path(directory) / 'drs'
    cpp.write_text(unit)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                    f'-I{root / "retoque"}', f'-I{root / "OptiScaler"}', f'-I{root / "external/nvapi"}',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
