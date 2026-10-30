#!/usr/bin/env python3
"""Compile the actual NGX core shutdown adapter and getter with strict host fakes."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
source = (root / 'OptiScaler/proxies/NVNGX_Proxy.h').read_text()


def between(start, end):
    assert source.count(start) == 1, f'production boundary changed: {start}'
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


dx11_types = between('typedef NVSDK_NGX_Result (*PFN_D3D11_Shutdown)',
                     'typedef NVSDK_NGX_Result (*PFN_D3D11_GetParameters)')
dx11_slots = between('    PFN_D3D11_Shutdown D3D11_Shutdown = nullptr;',
                     '    PFN_D3D11_GetParameters D3D11_GetParameters = nullptr;')
dx11_load = between('            _module.D3D11_Shutdown1 =',
                    '            _module.D3D11_GetParameters =')
dx11_getter = between('    static PFN_D3D11_Shutdown1 D3D11_Shutdown1()', '    // DirectX12')
types = between('typedef NVSDK_NGX_Result (*PFN_D3D12_Shutdown)',
                'typedef NVSDK_NGX_Result (*PFN_D3D12_GetParameters)')
adapter = between('    static NVSDK_NGX_Result ShutdownDx12Device(', '    inline static void LogCallback(')
custom = between('struct CustomNgxModule', 'class NVNGXProxy')
custom_shutdown = between('    static void ShutdownCustomDx12(', '    static void SetDx12Inited(')
getter = between('    static PFN_D3D12_Shutdown1 D3D12_Shutdown1()', '    // Vulkan')
unit = """
#include <cassert>
#include <type_traits>
#include <iostream>
using NVSDK_NGX_Result = unsigned;
constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Success = 1;
constexpr NVSDK_NGX_Result NVSDK_NGX_Result_FAIL_NotImplemented = 0xBAD00012;
struct ID3D12Device {};
struct ID3D11Device {};
""" + dx11_types + types + """
struct Module
{
    void* dll = nullptr;
""" + dx11_slots + """
    PFN_D3D12_Shutdown D3D12_Shutdown = nullptr;
    PFN_D3D12_CoreShutdown1 D3D12_Shutdown1 = nullptr;
};
using NvngxModule = Module;
struct KernelBaseProxy
{
    inline static void* address = nullptr;
    static void* Resolve(void*, const char*) { return address; }
    static auto GetProcAddress_() { return &Resolve; }
};
""" + custom + """
class NVNGXProxy
{
  public:
    inline static Module _module;
    inline static bool _dx12Inited = false;
    inline static bool _dx11Inited = false;
    static void LoadDx11() {
""" + dx11_load + """    }
""" + adapter + dx11_getter + getter + custom_shutdown + "};\n" + (here / 'cases.h').read_text()
with tempfile.TemporaryDirectory(prefix='optiscaler-ngx-shutdown-') as directory:
    cpp = Path(directory) / 'shutdown.cpp'
    binary = Path(directory) / 'shutdown'
    cpp.write_text(unit)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
