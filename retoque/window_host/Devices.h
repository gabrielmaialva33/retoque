// The host's devices: one DXGI factory, and a D3D11 device (capture) and a D3D12 device and direct queue (transport,
// NR and the preview) on the one adapter that drives the source's monitor. All of it exists before OptiScaler.dll is
// loaded, so none of it is created through the DLL's hooks.
#pragma once

#include <string>

namespace RetoqueWindowHost
{
struct Devices
{
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device5> d3d11;
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3D12Device> d3d12;
    ComPtr<ID3D12CommandQueue> queue;

    bool CreateFactory(std::string& why);
    bool Create(IDXGIAdapter1* chosen, std::string& why);

    // DXGI_ERROR_DEVICE_REMOVED and friends, on either device.
    bool Lost() const;
};
} // namespace RetoqueWindowHost
