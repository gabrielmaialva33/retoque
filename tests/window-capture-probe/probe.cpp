// NR on any window, phase 0 (docs/dlssnr/design/nr-anything.md): what this Windows (or Wine/Proton prefix) offers a
// window-capture host, as a table of HRESULTs. Nothing here loads OptiScaler, NGX or a model, and nothing touches
// another program beyond reading one frame of the window it is pointed at.
//
//   probe.exe [--window 0xHANDLE]
//
// Each check prints one line:  PROBE <name> <ok|fail|skip> hr=0x........ <detail>
// and the last line is        PROBE-END checks=N ok=N fail=N skip=N
//
// Capture mechanics follow the host's (retoque/window_host/Capture.cpp), which re-derives NeuRotic beae4eea's, adapted
// from Magpie 69d6105d (GPL-3.0; Licenses/NRAnything_ATTRIBUTION.txt).

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;
using winrt::Windows::Foundation::Metadata::ApiInformation;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

namespace
{
int g_checks = 0, g_ok = 0, g_fail = 0, g_skip = 0;

void Report(const char* name, const char* result, HRESULT hr, const std::string& detail)
{
    ++g_checks;
    if (std::strcmp(result, "ok") == 0)
        ++g_ok;
    else if (std::strcmp(result, "fail") == 0)
        ++g_fail;
    else
        ++g_skip;
    std::printf("PROBE %s %s hr=0x%08X %s\n", name, result, static_cast<unsigned>(hr), detail.c_str());
    std::fflush(stdout);
}

void Check(const char* name, HRESULT hr, const std::string& detail = {})
{
    Report(name, SUCCEEDED(hr) ? "ok" : "fail", hr, detail);
}

std::string Narrow(const wchar_t* text)
{
    char out[1024] {};
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out, int(sizeof(out)) - 1, nullptr, nullptr);
    return out;
}

std::string ModulePath(const wchar_t* name)
{
    HMODULE module = GetModuleHandleW(name);
    if (module == nullptr)
        return "not loaded";
    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(module, path, MAX_PATH);
    return Narrow(path);
}

constexpr COLORREF kPaint = RGB(0x20, 0xC0, 0x60);

LRESULT CALLBACK TestWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        HBRUSH brush = CreateSolidBrush(kPaint);
        FillRect(dc, &ps.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(window, &ps);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void Pump(DWORD milliseconds)
{
    const ULONGLONG until = GetTickCount64() + milliseconds;
    do
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(10);
    } while (GetTickCount64() < until);
}

// One frame of the window through WGC: its size, and the centre pixel read back through a staging copy.
void CaptureOne(const char* name, HWND window, ID3D11Device* device, ID3D11DeviceContext* context, bool expectPaint)
{
    HRESULT hr = S_OK;
    try
    {
        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(&dxgi)));
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
        const IDirect3DDevice wrapped = inspectable.as<IDirect3DDevice>();
        auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        GraphicsCaptureItem item { nullptr };
        hr = interop->CreateForWindow(window, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item));
        Check((std::string(name) + "-item").c_str(), hr);
        if (FAILED(hr))
            return;
        const auto size = item.Size();
        auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(wrapped, DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                                   2, size);
        auto session = pool.CreateCaptureSession(item);
        session.StartCapture();
        Direct3D11CaptureFrame frame { nullptr };
        const ULONGLONG until = GetTickCount64() + 2000;
        while (frame == nullptr && GetTickCount64() < until)
        {
            Pump(20);
            frame = pool.TryGetNextFrame();
        }
        if (frame == nullptr)
        {
            Report((std::string(name) + "-frame").c_str(), "fail", E_PENDING, "no frame within 2 s");
            session.Close();
            pool.Close();
            return;
        }
        const auto content = frame.ContentSize();
        ComPtr<ID3D11Texture2D> texture;
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        desc.Width = desc.Height = 1;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MipLevels = desc.ArraySize = 1;
        ComPtr<ID3D11Texture2D> staging;
        winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, &staging));
        const UINT cx = UINT(content.Width / 2), cy = UINT(content.Height / 2);
        const D3D11_BOX box { cx, cy, 0, cx + 1, cy + 1, 1 };
        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture.Get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        winrt::check_hresult(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)); // waits for the copy
        const auto* bgra = static_cast<const unsigned char*>(mapped.pData);
        char detail[160];
        std::snprintf(detail, sizeof(detail), "content=%dx%d time=%lld centre=#%02X%02X%02X", content.Width,
                      content.Height, static_cast<long long>(frame.SystemRelativeTime().count()), bgra[2], bgra[1],
                      bgra[0]);
        const bool painted =
            bgra[2] == GetRValue(kPaint) && bgra[1] == GetGValue(kPaint) && bgra[0] == GetBValue(kPaint);
        context->Unmap(staging.Get(), 0);
        Report((std::string(name) + "-frame").c_str(), !expectPaint || painted ? "ok" : "fail", S_OK,
               std::string(detail) +
                   (expectPaint ? (painted ? " (the painted colour)" : " (not the painted colour)") : ""));
        texture.Reset();
        frame.Close();
        session.Close();
        pool.Close();
    }
    catch (const winrt::hresult_error& e)
    {
        Report((std::string(name) + "-frame").c_str(), "fail", e.code(), Narrow(e.message().c_str()));
    }
}

void Affinity(const char* name, HWND window)
{
    DWORD affinity = 0;
    SetLastError(ERROR_SUCCESS);
    const BOOL read = GetWindowDisplayAffinity(window, &affinity);
    Report(name, read ? "ok" : "fail", read ? S_OK : HRESULT_FROM_WIN32(GetLastError()),
           "affinity=0x" + std::to_string(affinity));
}

// The overlay's building blocks: an owned-style click-through popup, a DirectComposition target and a composition
// swapchain on a D3D12 queue, one cleared frame presented and then torn down.
void Overlay(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGIFactory2* factory)
{
    WNDCLASSEXW wc { sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RetoqueCaptureProbeOverlay";
    RegisterClassExW(&wc);
    HWND popup = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, L"probe overlay", WS_POPUP, 40, 40, 256, 256, nullptr, nullptr, wc.hInstance, nullptr);
    Report("overlay-window", popup != nullptr ? "ok" : "fail",
           popup != nullptr ? S_OK : HRESULT_FROM_WIN32(GetLastError()), "WS_POPUP, layered, transparent, no-activate");
    if (popup == nullptr)
        return;
    const BOOL excluded = SetWindowDisplayAffinity(popup, WDA_EXCLUDEFROMCAPTURE);
    Report("overlay-exclude-from-capture", excluded ? "ok" : "fail",
           excluded ? S_OK : HRESULT_FROM_WIN32(GetLastError()), "WDA_EXCLUDEFROMCAPTURE (Windows 10 2004+)");

    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = desc.Height = 256;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    ComPtr<IDXGISwapChain1> swapchain;
    HRESULT hr = factory->CreateSwapChainForComposition(queue, &desc, nullptr, &swapchain);
    Check("overlay-composition-swapchain", hr, "D3D12 queue, flip-sequential, premultiplied");
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    if (SUCCEEDED(hr))
    {
        hr = DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&dcomp));
        Check("overlay-dcomp-device", hr);
    }
    if (SUCCEEDED(hr))
    {
        hr = dcomp->CreateTargetForHwnd(popup, TRUE, &target);
        if (SUCCEEDED(hr))
            hr = dcomp->CreateVisual(&visual);
        if (SUCCEEDED(hr))
            hr = visual->SetContent(swapchain.Get());
        if (SUCCEEDED(hr))
            hr = target->SetRoot(visual.Get());
        if (SUCCEEDED(hr))
            hr = dcomp->Commit();
        Check("overlay-dcomp-target", hr, "target, visual, content, commit");
    }
    if (SUCCEEDED(hr))
    {
        // One frame through the queue: a cleared backbuffer, presented, the queue drained before anything is released.
        ComPtr<IDXGISwapChain3> sc3;
        ComPtr<ID3D12Resource> backbuffer;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12DescriptorHeap> heap;
        ComPtr<ID3D12Fence> fence;
        hr = swapchain.As(&sc3);
        if (SUCCEEDED(hr))
            hr = sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer));
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
        if (SUCCEEDED(hr))
            hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                           IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr))
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (SUCCEEDED(hr))
        {
            const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
            device->CreateRenderTargetView(backbuffer.Get(), nullptr, rtv);
            D3D12_RESOURCE_BARRIER barrier {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = backbuffer.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            list->ResourceBarrier(1, &barrier);
            const float colour[4] = { 0.0f, 0.25f, 0.5f, 0.5f };
            list->ClearRenderTargetView(rtv, colour, 0, nullptr);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(1, &barrier);
            hr = list->Close();
        }
        bool submitted = false;
        if (SUCCEEDED(hr))
        {
            ID3D12CommandList* lists[] = { list.Get() };
            queue->ExecuteCommandLists(1, lists);
            submitted = true;
            hr = sc3->Present(0, 0);
        }
        Check("overlay-present", hr, "one cleared frame");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        bool drained = !submitted;
        if (submitted && event != nullptr && SUCCEEDED(queue->Signal(fence.Get(), 1)) &&
            SUCCEEDED(fence->SetEventOnCompletion(1, event)))
            drained = WaitForSingleObject(event, 2000) == WAIT_OBJECT_0;
        if (submitted)
            Report("overlay-drain", drained ? "ok" : "fail", drained ? S_OK : E_FAIL, "queue idle before release");
        if (!drained)
        {
            // Never release what the GPU may still use: leak it all and end here.
            std::printf("PROBE-END checks=%d ok=%d fail=%d skip=%d abandoned=1\n", g_checks, g_ok, g_fail + 1, g_skip);
            std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 3);
        }
        if (event != nullptr)
            CloseHandle(event);
    }
    DestroyWindow(popup);
}

void Duplication(IDXGIAdapter1* adapter, ID3D11Device* device)
{
    ComPtr<IDXGIOutput> output;
    HRESULT hr = adapter->EnumOutputs(0, &output);
    if (FAILED(hr))
    {
        Report("dxgi-duplicate-output", "skip", hr, "the adapter has no output 0");
        return;
    }
    ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    ComPtr<IDXGIOutputDuplication> duplication;
    if (SUCCEEDED(hr))
        hr = output1->DuplicateOutput(device, &duplication);
    Check("dxgi-duplicate-output", hr, "IDXGIOutput1::DuplicateOutput on output 0");
    if (SUCCEEDED(hr))
    {
        DXGI_OUTDUPL_FRAME_INFO info {};
        ComPtr<IDXGIResource> resource;
        hr = duplication->AcquireNextFrame(500, &info, &resource);
        Report("dxgi-acquire-frame", SUCCEEDED(hr) ? "ok" : (hr == DXGI_ERROR_WAIT_TIMEOUT ? "skip" : "fail"), hr,
               hr == DXGI_ERROR_WAIT_TIMEOUT ? "nothing changed on screen within 500 ms" : "");
        if (SUCCEEDED(hr))
            duplication->ReleaseFrame();
    }
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND target = nullptr;
    for (int i = 1; i < argc; ++i)
        if (std::wcscmp(argv[i], L"--window") == 0 && i + 1 < argc)
            target = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::wcstoull(argv[++i], nullptr, 16)));

    // Where this runs: Windows build, and Wine if it is Wine.
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    OSVERSIONINFOW os { sizeof(os) };
    if (auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")))
        get(&os);
    using WineVersionFn = const char*(CDECL*) ();
    auto wine = reinterpret_cast<WineVersionFn>(GetProcAddress(ntdll, "wine_get_version"));
    Report("environment", "ok", S_OK,
           "windows=" + std::to_string(os.dwMajorVersion) + "." + std::to_string(os.dwMinorVersion) + "." +
               std::to_string(os.dwBuildNumber) + " wine=" + (wine != nullptr ? wine() : "no"));

    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        Report("winrt", "ok", S_OK, "multi-threaded apartment");
    }
    catch (const winrt::hresult_error& e)
    {
        Report("winrt", "fail", e.code(), "the rest of the WGC checks cannot run");
    }

    bool wgc = false;
    try
    {
        wgc = GraphicsCaptureSession::IsSupported();
        Report("wgc-supported", wgc ? "ok" : "fail", S_OK, wgc ? "IsSupported() true" : "IsSupported() false");
        const auto session = winrt::name_of<GraphicsCaptureSession>();
        const bool cursor = ApiInformation::IsPropertyPresent(session, L"IsCursorCaptureEnabled");
        const bool border = ApiInformation::IsPropertyPresent(session, L"IsBorderRequired");
        Report("wgc-cursor-api", cursor ? "ok" : "fail", S_OK, "IsCursorCaptureEnabled (Windows 10 2004+)");
        Report("wgc-border-api", border ? "ok" : "skip", S_OK, "IsBorderRequired (Windows 11)");
    }
    catch (const winrt::hresult_error& e)
    {
        Report("wgc-supported", "fail", e.code(), Narrow(e.message().c_str()));
    }

    // One adapter for everything: the high-performance one.
    ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    Check("dxgi-factory", hr, "dxgi.dll=" + ModulePath(L"dxgi.dll"));
    if (FAILED(hr))
    {
        std::printf("PROBE-END checks=%d ok=%d fail=%d skip=%d\n", g_checks, g_ok, g_fail, g_skip);
        return 1;
    }
    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    DXGI_ADAPTER_DESC1 adapterDesc {};
    if (SUCCEEDED(hr))
        adapter->GetDesc1(&adapterDesc);
    char vendor[16];
    std::snprintf(vendor, sizeof(vendor), "0x%04X", adapterDesc.VendorId);
    Check("adapter", hr, Narrow(adapterDesc.Description) + " vendor=" + vendor);

    ComPtr<ID3D11Device> d3d11;
    ComPtr<ID3D11DeviceContext> context;
    if (SUCCEEDED(hr))
    {
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               nullptr, 0, D3D11_SDK_VERSION, &d3d11, nullptr, &context);
        Check("d3d11-device", hr, "d3d11.dll=" + ModulePath(L"d3d11.dll"));
        ComPtr<ID3D11Device5> fences;
        if (SUCCEEDED(hr))
            Check("d3d11-shared-fences", d3d11.As(&fences), "ID3D11Device5");
    }
    ComPtr<ID3D12Device> d3d12;
    ComPtr<ID3D12CommandQueue> queue;
    if (adapter != nullptr)
    {
        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12));
        Check("d3d12-device", hr, "d3d12.dll=" + ModulePath(L"d3d12.dll"));
        if (SUCCEEDED(hr))
        {
            D3D12_COMMAND_QUEUE_DESC desc {};
            desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            Check("d3d12-queue", d3d12->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
        }
    }

    // A window of our own, painted a known colour, then the target window.
    WNDCLASSEXW wc { sizeof(wc) };
    wc.lpfnWndProc = TestWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RetoqueCaptureProbeSource";
    RegisterClassExW(&wc);
    HWND own = CreateWindowExW(0, wc.lpszClassName, L"Retoque capture probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100,
                               100, 400, 300, nullptr, nullptr, wc.hInstance, nullptr);
    Report("own-window", own != nullptr ? "ok" : "fail", own != nullptr ? S_OK : HRESULT_FROM_WIN32(GetLastError()),
           "400x300, painted #20C060");
    Pump(300);
    if (own != nullptr)
        Affinity("own-affinity", own);
    if (wgc && d3d11 != nullptr && own != nullptr)
        CaptureOne("own-capture", own, d3d11.Get(), context.Get(), true);
    else
        Report("own-capture", "skip", S_OK, "no WGC, D3D11 device or window");

    if (target == nullptr)
    {
        // The foreground window, unless it is ours.
        HWND foreground = GetForegroundWindow();
        target = foreground != own ? foreground : nullptr;
    }
    if (target != nullptr && IsWindow(target))
    {
        wchar_t title[256] {};
        GetWindowTextW(target, title, 255);
        DWORD pid = 0;
        GetWindowThreadProcessId(target, &pid);
        Report("target", "ok", S_OK, "pid=" + std::to_string(pid) + " title=\"" + Narrow(title) + "\"");
        Affinity("target-affinity", target);
        if (wgc && d3d11 != nullptr)
            CaptureOne("target-capture", target, d3d11.Get(), context.Get(), false);
        else
            Report("target-capture", "skip", S_OK, "no WGC or D3D11 device");
    }
    else
        Report("target", "skip", S_OK, "no --window and no foreground window other than the probe's");

    if (d3d12 != nullptr && queue != nullptr)
        Overlay(d3d12.Get(), queue.Get(), factory.Get());
    else
        Report("overlay", "skip", S_OK, "no D3D12 device");
    if (d3d11 != nullptr)
        Duplication(adapter.Get(), d3d11.Get());

    if (own != nullptr)
        DestroyWindow(own);
    std::printf("PROBE-END checks=%d ok=%d fail=%d skip=%d\n", g_checks, g_ok, g_fail, g_skip);
    return g_fail == 0 ? 0 : 1;
}
