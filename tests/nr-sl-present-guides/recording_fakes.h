#include <dlssnr/DlssNr_SlPresentPolicy.h>
#include <mutex>
#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdint>
#define STDMETHODCALLTYPE
using UINT = unsigned;
using HRESULT = long;
using PVOID = void*;
struct GUID
{
    uint32_t a;
    uint16_t b, c;
    uint8_t d[8];
};
#define FAILED(hr) ((hr) < 0)
#define SUCCEEDED(hr) ((hr) >= 0)
#define IID_PPV_ARGS(pp) 0, reinterpret_cast<void**>(pp)
constexpr int NO_ERROR = 0, D3D12_COMMAND_LIST_TYPE_DIRECT = 0;
using D3D12_RENDER_PASS_FLAGS = unsigned;
constexpr D3D12_RENDER_PASS_FLAGS D3D12_RENDER_PASS_FLAG_NONE = 0;
struct D3D12_RENDER_PASS_RENDER_TARGET_DESC
{
};
struct D3D12_RENDER_PASS_DEPTH_STENCIL_DESC
{
};
struct ID3D12CommandAllocator
{
};
struct ID3D12PipelineState
{
};
struct ID3D12Object
{
    void** table = nullptr;
    DlssNr::SlPresent::Recording data;
    bool stored = false, failStore = false;
    HRESULT GetPrivateData(const GUID&, UINT* size, void* out)
    {
        if (!stored || *size != sizeof(data))
            return -1;
        std::memcpy(out, &data, sizeof(data));
        return 0;
    }
    HRESULT SetPrivateData(const GUID&, UINT size, const void* in)
    {
        if (failStore)
            return -1;
        assert(size == sizeof(data));
        std::memcpy(&data, in, size);
        stored = true;
        return 0;
    }
};
struct ID3D12GraphicsCommandList : ID3D12Object
{
    int type = 0;
    int GetType() const { return type; }
    HRESULT QueryInterface(int, void** out)
    {
        *out = this;
        return 0;
    }
    unsigned Release() { return 0; }
};
using ID3D12GraphicsCommandList4 = ID3D12GraphicsCommandList;
namespace ResTrack_Dx12
{
ID3D12GraphicsCommandList* NrRealObject(ID3D12GraphicsCommandList* list) { return list; }
} // namespace ResTrack_Dx12
int detourFailure = 0, attached = 0, aborts = 0, commits = 0, forwards = 0;
bool resetSuccess = true;
int DetourTransactionBegin() { return detourFailure == 1 ? 1 : 0; }
int GetCurrentThread() { return 1; }
int DetourUpdateThread(int) { return detourFailure == 2 ? 1 : 0; }
template <class T> int DetourAttach(PVOID*, T)
{
    ++attached;
    return detourFailure == 3 ? 1 : 0;
}
int DetourTransactionAbort()
{
    ++aborts;
    return 0;
}
int DetourTransactionCommit()
{
    ++commits;
    return detourFailure == 4 ? 1 : 0;
}
HRESULT RealReset(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*)
{
    ++forwards;
    return resetSuccess ? 0 : -1;
}
HRESULT RealClose(ID3D12GraphicsCommandList*)
{
    ++forwards;
    return 0;
}
void RealBegin(ID3D12GraphicsCommandList4*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
               const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS)
{
    ++forwards;
}
void RealEnd(ID3D12GraphicsCommandList4*) { ++forwards; }
