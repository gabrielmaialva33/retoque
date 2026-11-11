#include "pch.h"

#include "Devices.h"

namespace RetoqueWindowHost
{
namespace
{
std::string Failure(const char* what, HRESULT hr)
{
    char text[64];
    snprintf(text, sizeof(text), " (HRESULT 0x%08X)", static_cast<unsigned>(hr));
    return std::string(what) + text;
}
} // namespace

bool Devices::CreateFactory(std::string& why)
{
    const HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
        why = Failure("no DXGI factory", hr);
    return SUCCEEDED(hr);
}

bool Devices::Create(IDXGIAdapter1* chosen, std::string& why)
{
    adapter = chosen;
    // D3D11 for Windows Graphics Capture; BGRA support is what WGC's surfaces need.
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate;
    HRESULT hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   UINT(std::size(levels)), D3D11_SDK_VERSION, &device, nullptr, &immediate);
    if (FAILED(hr))
    {
        why = Failure("no D3D11 device on the source's adapter", hr);
        return false;
    }
    // ID3D11Device5 and ID3D11DeviceContext4 carry the shared fences (Windows 10 1703 or later).
    if (FAILED(hr = device.As(&d3d11)) || FAILED(hr = immediate.As(&context)))
    {
        why = Failure("the D3D11 device has no shared fences", hr);
        return false;
    }

    hr = D3D12CreateDevice(chosen, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12));
    if (FAILED(hr))
    {
        why = Failure("no D3D12 device on the source's adapter", hr);
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC desc {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = d3d12->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
    if (FAILED(hr))
    {
        why = Failure("no D3D12 direct queue", hr);
        return false;
    }
    return true;
}

bool Devices::Lost() const
{
    return (d3d12 != nullptr && d3d12->GetDeviceRemovedReason() != S_OK) ||
           (d3d11 != nullptr && d3d11->GetDeviceRemovedReason() != S_OK);
}
} // namespace RetoqueWindowHost
