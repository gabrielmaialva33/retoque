// DLL-backed native D3D11 NGX + wrapped D3D11 swapchain, with no fallback backend.
// No game and no private Retoque API. A failure exits without a PASS marker.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_params.h"
#include <cstdio>
#include <cstdint>
#include <vector>

static FILE* logFile = nullptr;
static void Stage(const char* stage, const char* position)
{
    std::fprintf(logFile, "D3D11-PRESENT STAGE: %s %s\n", stage, position);
    std::fflush(logFile);
}
static void Fail(const char* stage, unsigned long code)
{
    if (logFile)
    {
        std::fprintf(logFile, "D3D11-PRESENT FAIL: stage=%s code=0x%08lX\n", stage, code);
        std::fflush(logFile);
    }
    // Unknown GPU/bridge state: process exit, no speculative teardown or retry.
    ExitProcess(1);
}
static void Check(HRESULT hr, const char* stage)
{
    if (FAILED(hr))
        Fail(stage, static_cast<unsigned long>(hr));
}
static void CheckNgx(NVSDK_NGX_Result result, const char* stage)
{
    if (result != NVSDK_NGX_Result_Success)
        Fail(stage, static_cast<unsigned long>(result));
}
template <typename T> static T Export(HMODULE module, const char* name)
{
    const FARPROC value = GetProcAddress(module, name);
    if (!value)
        Fail(name, GetLastError());
    return reinterpret_cast<T>(value);
}
template <typename T> static void Release(T*& p)
{
    if (p)
        p->Release();
    p = nullptr;
}
static LRESULT CALLBACK WindowProc(HWND window, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(window, msg, wp, lp);
}
static void Pump()
{
    MSG msg {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
static ID3D11Texture2D* Texture(ID3D11Device* device, DXGI_FORMAT format, UINT bind)
{
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = 640;
    desc.Height = 360;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    ID3D11Texture2D* texture = nullptr;
    Check(device->CreateTexture2D(&desc, nullptr, &texture), "CreateTexture2D");
    return texture;
}

int main(int argc, char** argv)
{
    logFile = std::fopen("dx11-present.log", "w");
    if (!logFile)
        return 1;
    std::setvbuf(logFile, nullptr, _IONBF, 0);
    Stage("Init", "begin");
    if (argc != 2)
        Fail("expected DLL path", 0);
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll)
        Fail("LoadLibrary OptiScaler", GetLastError());
    using FactoryFn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto factoryExport = Export<FactoryFn>(dll, "CreateDXGIFactory2");
    auto init = Export<decltype(&NVSDK_NGX_D3D11_Init)>(dll, "NVSDK_NGX_D3D11_Init");
    auto alloc = Export<decltype(&NVSDK_NGX_D3D11_AllocateParameters)>(dll, "NVSDK_NGX_D3D11_AllocateParameters");
    auto create = Export<decltype(&NVSDK_NGX_D3D11_CreateFeature)>(dll, "NVSDK_NGX_D3D11_CreateFeature");
    auto evaluate = Export<decltype(&NVSDK_NGX_D3D11_EvaluateFeature)>(dll, "NVSDK_NGX_D3D11_EvaluateFeature");
    auto release = Export<decltype(&NVSDK_NGX_D3D11_ReleaseFeature)>(dll, "NVSDK_NGX_D3D11_ReleaseFeature");
    auto destroy = Export<decltype(&NVSDK_NGX_D3D11_DestroyParameters)>(dll, "NVSDK_NGX_D3D11_DestroyParameters");
    auto shutdown = Export<decltype(&NVSDK_NGX_D3D11_Shutdown1)>(dll, "NVSDK_NGX_D3D11_Shutdown1");

    WNDCLASSW wc {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RetoqueDx11PresentProbe";
    if (!RegisterClassW(&wc))
        Fail("RegisterClass", GetLastError());
    HWND window = CreateWindowW(wc.lpszClassName, L"Retoque D3D11 probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 800, 500, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window)
        Fail("CreateWindow", GetLastError());
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL selected {};
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                            D3D11_SDK_VERSION, &device, &selected, &context),
          "D3D11CreateDevice");
    IDXGIFactory2* factory = nullptr;
    Check(factoryExport(0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory)), "exported CreateDXGIFactory2");
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = 640;
    desc.Height = 360;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* swapchain = nullptr;
    Check(factory->CreateSwapChainForHwnd(device, window, &desc, nullptr, nullptr, &swapchain),
          "CreateSwapChainForHwnd");

    Stage("D3D11_Init", "before");
    CheckNgx(init(0x1337, L".", device, nullptr, NVSDK_NGX_Version_API), "D3D11_Init");
    Stage("D3D11_Init", "after");
    NVSDK_NGX_Parameter* params = nullptr;
    CheckNgx(alloc(&params), "D3D11_AllocateParameters");
    if (!params)
        Fail("null parameters", 0);
    params->Set(NVSDK_NGX_Parameter_Width, 640u);
    params->Set(NVSDK_NGX_Parameter_Height, 360u);
    params->Set(NVSDK_NGX_Parameter_OutWidth, 640u);
    params->Set(NVSDK_NGX_Parameter_OutHeight, 360u);
    params->Set(NVSDK_NGX_Parameter_PerfQualityValue, NVSDK_NGX_PerfQuality_Value_DLAA);
    params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
    params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    NVSDK_NGX_Handle* feature = nullptr;
    Stage("CreateFeature", "before");
    CheckNgx(create(context, NVSDK_NGX_Feature_SuperSampling, params, &feature), "D3D11_CreateFeature");
    Stage("CreateFeature", "after");
    if (!feature)
        Fail("null feature", 0);
    auto* color = Texture(device, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    auto* output =
        Texture(device, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    auto* depth = Texture(device, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    auto* motion = Texture(device, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    std::vector<uint32_t> pixels(640 * 360, 0xff406080u);
    std::vector<float> depths(640 * 360, 0.5f);
    std::vector<uint32_t> vectors(640 * 360, 0u);
    context->UpdateSubresource(color, 0, nullptr, pixels.data(), 640 * 4, 0);
    context->UpdateSubresource(depth, 0, nullptr, depths.data(), 640 * 4, 0);
    context->UpdateSubresource(motion, 0, nullptr, vectors.data(), 640 * 4, 0);
    params->Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(color));
    params->Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource*>(output));
    params->Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(depth));
    params->Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(motion));
    params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 640u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 360u);
    params->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    Stage("Run", "begin");
    const ULONGLONG started = GetTickCount64();
    unsigned evaluations = 0, presents = 0;
    for (unsigned frame = 0; frame < 200; ++frame)
    {
        params->Set(NVSDK_NGX_Parameter_Reset, frame == 0 ? 1 : 0);
        CheckNgx(evaluate(context, feature, params, nullptr), "D3D11_EvaluateFeature");
        ++evaluations;
        ID3D11Texture2D* backbuffer = nullptr;
        Check(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer)), "GetBuffer");
        context->CopyResource(backbuffer, output);
        Release(backbuffer);
        Check(swapchain->Present(0, 0), "Present");
        ++presents;
        Pump();
        Sleep(30);
    }
    const ULONGLONG elapsed = GetTickCount64() - started;
    if (elapsed < 6000)
        Fail("window shorter than 6 seconds", static_cast<unsigned long>(elapsed));
    std::fprintf(logFile, "D3D11-PRESENT LOOP-COMPLETE: evaluations=%u presents=%u elapsed_ms=%llu\n", evaluations,
                 presents, static_cast<unsigned long long>(elapsed));
    std::fflush(logFile);
    Stage("Fence", "before");
    D3D11_QUERY_DESC queryDesc { D3D11_QUERY_EVENT, 0 };
    ID3D11Query* done = nullptr;
    Check(device->CreateQuery(&queryDesc, &done), "CreateQuery");
    context->End(done);
    context->Flush();
    const ULONGLONG waitStart = GetTickCount64();
    HRESULT completed = S_FALSE;
    while ((completed = context->GetData(done, nullptr, 0, 0)) == S_FALSE && GetTickCount64() - waitStart < 10000)
        Sleep(1);
    if (completed != S_OK)
        Fail("D3D11 completion", static_cast<unsigned long>(completed));
    Release(done);
    Stage("Fence", "after");
    Stage("ReleaseFeature", "before");
    CheckNgx(release(feature), "D3D11_ReleaseFeature");
    Stage("ReleaseFeature", "after");
    Stage("DestroyParameters", "before");
    CheckNgx(destroy(params), "D3D11_DestroyParameters");
    Stage("DestroyParameters", "after");
    Stage("Shutdown1", "before");
    CheckNgx(shutdown(device), "D3D11_Shutdown1");
    Stage("Shutdown1", "after");
    Release(color);
    Release(output);
    Release(depth);
    Release(motion);
    Stage("ReleaseSwapchain", "before");
    Release(swapchain);
    Stage("ReleaseSwapchain", "after");
    Release(factory);
    Stage("ReleaseContext", "before");
    Release(context);
    Stage("ReleaseContext", "after");
    Stage("ReleaseDevice", "before");
    Release(device);
    Stage("ReleaseDevice", "after");
    Stage("DestroyWindow", "before");
    DestroyWindow(window);
    Stage("DestroyWindow", "after");
    std::fprintf(logFile, "D3D11-PRESENT PASS: evaluations=%u presents=%u elapsed_ms=%llu\n", evaluations, presents,
                 static_cast<unsigned long long>(elapsed));
    std::fclose(logFile);
    return 0;
}
