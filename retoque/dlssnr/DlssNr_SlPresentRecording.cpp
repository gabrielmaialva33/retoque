#include "pch.h"
#include "DlssNr_SlPresentGuides.h"
#include "DlssNr_SlPresentPolicy.h"
#include <resource_tracking/ResTrack_dx12.h>
#include <detours/detours.h>
#include <d3d12.h>
#include <mutex>
#include <atomic>

namespace DlssNr::SlPresent
{
namespace
{
// Recording state belongs to the command-list object. Address reuse never inherits an old recording.
constexpr GUID recordingKey { 0x43578a69, 0x8dc9, 0x498c, { 0xab, 0x19, 0x53, 0x2f, 0x11, 0x9b, 0xee, 0x48 } };
std::mutex installMutex;
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using CloseFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using BeginFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
                                         const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS);
using EndFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*);
ResetFn reset = nullptr;
CloseFn close = nullptr;
BeginFn begin = nullptr;
EndFn end = nullptr;
void* targets[4] {};
bool installed = false, failed = false;
std::atomic<bool> healthy { true };
Recording Read(ID3D12Object* list)
{
    Recording r;
    UINT size = sizeof(r);
    if (FAILED(list->GetPrivateData(recordingKey, &size, &r)) || size != sizeof(r))
        return {};
    return r;
}
void Write(ID3D12Object* list, const Recording& r)
{
    if (FAILED(list->SetPrivateData(recordingKey, sizeof(r), &r)))
        healthy = false;
}
HRESULT STDMETHODCALLTYPE Reset(ID3D12GraphicsCommandList* l, ID3D12CommandAllocator* a, ID3D12PipelineState* p)
{
    const auto result = reset(l, a, p);
    Recording r;
    r.Reset(SUCCEEDED(result));
    Write(l, r);
    return result;
}
HRESULT STDMETHODCALLTYPE Close(ID3D12GraphicsCommandList* l)
{
    auto r = Read(l);
    r.Close();
    Write(l, r);
    return close(l);
}
void STDMETHODCALLTYPE Begin(ID3D12GraphicsCommandList4* l, UINT n, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rt,
                             const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds, D3D12_RENDER_PASS_FLAGS flags)
{
    auto r = Read(l);
    r.Begin(flags == D3D12_RENDER_PASS_FLAG_NONE);
    Write(l, r);
    begin(l, n, rt, ds, flags);
}
void STDMETHODCALLTYPE End(ID3D12GraphicsCommandList4* l)
{
    end(l);
    auto r = Read(l);
    r.End();
    Write(l, r);
}
} // namespace
bool CopyRecording(ID3D12GraphicsCommandList* list)
{
    if (!list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return false;
    auto* object = ResTrack_Dx12::NrRealObject(list);
    ID3D12GraphicsCommandList4* real = nullptr;
    if (!object || FAILED(object->QueryInterface(IID_PPV_ARGS(&real))))
        return false;
    std::lock_guard lock(installMutex);
    auto** v = *reinterpret_cast<void***>(real);
    void* current[4] { v[10], v[9], v[68], v[69] };
    bool compatible = true;
    if (!installed && !failed)
    {
        for (size_t i = 0; i < 4; ++i)
            targets[i] = current[i];
        reset = reinterpret_cast<ResetFn>(targets[0]);
        close = reinterpret_cast<CloseFn>(targets[1]);
        begin = reinterpret_cast<BeginFn>(targets[2]);
        end = reinterpret_cast<EndFn>(targets[3]);
        if (DetourTransactionBegin() != NO_ERROR)
        {
            failed = true;
            real->Release();
            return false;
        }
        if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR)
        {
            DetourTransactionAbort();
            failed = true;
            real->Release();
            return false;
        }
        const auto a = DetourAttach(reinterpret_cast<PVOID*>(&reset), Reset);
        const auto b = DetourAttach(reinterpret_cast<PVOID*>(&close), Close);
        const auto c = DetourAttach(reinterpret_cast<PVOID*>(&begin), Begin);
        const auto d = DetourAttach(reinterpret_cast<PVOID*>(&end), End);
        if (a != NO_ERROR || b != NO_ERROR || c != NO_ERROR || d != NO_ERROR)
        {
            DetourTransactionAbort();
            failed = true;
        }
        else
        {
            installed = DetourTransactionCommit() == NO_ERROR;
            failed = !installed;
        }
    }
    for (size_t i = 0; i < 4; ++i)
        compatible &= current[i] == targets[i];
    const bool allowed = installed && healthy.load() && compatible && Read(real).CanCopy();
    real->Release();
    return allowed;
}
} // namespace DlssNr::SlPresent
