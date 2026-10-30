// Fakes for the D3D12 FG command objects and the HUD-less format gate, sliced verbatim from
// OptiScaler/framegen/IFGFeature_Dx12.{h,cpp}, dlssg/DLSSG_Dx12.cpp and xefg/XeFG_Dx12.cpp. The device fails a chosen
// creation call and returns null for it, as D3D12 does; every object counts its references so a leak, a double release
// or a call on a missing object shows.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <unordered_map>

template <typename... T> void testLog(T&&...) {}
#define LOG_TRACE(...) testLog(__VA_ARGS__)
#define LOG_DEBUG(...) testLog(__VA_ARGS__)
#define LOG_INFO(...) testLog(__VA_ARGS__)
#define LOG_WARN(...) testLog(__VA_ARGS__)
#define LOG_ERROR(...) testLog(__VA_ARGS__)
#define BUFFER_COUNT 4
#define SAFE_RELEASE(p)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        if (p && p != nullptr)                                                                                         \
        {                                                                                                              \
            (p)->Release();                                                                                            \
            (p) = nullptr;                                                                                             \
        }                                                                                                              \
    } while ((void) 0, 0);

using HRESULT = int32_t;
using UINT = unsigned int;
using UINT64 = unsigned long long;
using DWORD = unsigned long;
using BOOL = int;
using HANDLE = void*;
constexpr BOOL FALSE = 0;
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_OUTOFMEMORY = (HRESULT) 0x8007000Eu;
constexpr DWORD WAIT_OBJECT_0 = 0;
#define FAILED(hr) (((HRESULT) (hr)) < 0)
#define SUCCEEDED(hr) (((HRESULT) (hr)) >= 0)
#define IID_PPV_ARGS(pp) 0, reinterpret_cast<void**>(pp)

enum D3D12_COMMAND_LIST_TYPE
{
    D3D12_COMMAND_LIST_TYPE_DIRECT = 0
};
enum D3D12_FENCE_FLAGS
{
    D3D12_FENCE_FLAG_NONE = 0
};
enum DXGI_FORMAT
{
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_R10G10B10A2_UNORM = 24
};
enum FG_ResourceType : uint32_t
{
    Depth = 0,
    Velocity,
    HudlessColor,
    UIColor,
    Distortion,
    Color,
    ResourceTypeCOUNT
};

static int objectsAlive = 0;
static int calls = 0;      // device creations so far
static int failAt = -1;    // the creation call that fails
static int fenceCalls = 0; // fence creations so far

struct IUnknown
{
    int refs = 1;
    virtual ~IUnknown() = default;
    IUnknown() { ++objectsAlive; }
    unsigned long Release()
    {
        if (refs <= 0)
        {
            std::fprintf(stderr, "FAIL: released an object with no references left\n");
            std::_Exit(1);
        }
        if (--refs == 0)
        {
            --objectsAlive;
            delete this;
            return 0;
        }
        return (unsigned long) refs;
    }
    void SetName(const wchar_t*) {}
};
struct ID3D12CommandAllocator : IUnknown
{
    HRESULT Reset() { return S_OK; }
};
struct ID3D12GraphicsCommandList : IUnknown
{
    bool open = true;
    HRESULT Close()
    {
        open = false;
        return S_OK;
    }
    HRESULT Reset(ID3D12CommandAllocator*, void*)
    {
        open = true;
        return S_OK;
    }
};
using ID3D12CommandList = ID3D12GraphicsCommandList;
struct ID3D12Fence : IUnknown
{
    UINT64 completed = ~0ull; // the GPU is idle in these cases
    UINT64 GetCompletedValue() { return completed; }
    HRESULT SetEventOnCompletion(UINT64, HANDLE) { return S_OK; }
};
struct ID3D12CommandQueue : IUnknown
{
    void ExecuteCommandLists(UINT, ID3D12CommandList* const*) {}
    HRESULT Signal(ID3D12Fence*, UINT64) { return S_OK; }
};

static bool Fails() { return ++calls == failAt; }

struct ID3D12Device
{
    HRESULT CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE, int, void** out)
    {
        *out = Fails() ? nullptr : static_cast<void*>(new ID3D12CommandAllocator());
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, void*, int, void** out)
    {
        *out = Fails() ? nullptr : static_cast<void*>(new ID3D12GraphicsCommandList());
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT CreateFence(UINT64, D3D12_FENCE_FLAGS, int, void** out)
    {
        ++fenceCalls;
        *out = Fails() ? nullptr : static_cast<void*>(new ID3D12Fence());
        return *out ? S_OK : E_OUTOFMEMORY;
    }
};

static int eventsAlive = 0;
inline HANDLE CreateEvent(void*, BOOL, BOOL, void*)
{
    ++eventsAlive;
    return reinterpret_cast<HANDLE>(0x100 + eventsAlive);
}
inline DWORD WaitForSingleObject(HANDLE, DWORD) { return WAIT_OBJECT_0; }
inline bool CheckForRealObject(const char*, IUnknown*, IUnknown**) { return false; }

struct State
{
    ID3D12Device* currentD3D12Device = nullptr;
    bool fgChanged = false;
    static State& Instance()
    {
        static State s;
        return s;
    }
};

class IFGFeature_Dx12
{
  public:
    virtual ~IFGFeature_Dx12() = default;
    int index = 0;
    int GetIndex() { return index; }

    bool WaitForUIAllocator(UINT index);
    bool SubmitUICommandList(UINT index);
    ID3D12GraphicsCommandList* GetUICommandList(int index = -1);
    ID3D12GraphicsCommandList* GetSCCommandList(int index = -1);
    bool DropIncompleteCommandObjects();
    virtual void CreateObjects(ID3D12Device* InDevice) = 0;

    ID3D12Device* _device = nullptr;
    ID3D12CommandQueue* _gameCommandQueue = nullptr;
    // @command-object-members@
};

struct DLSSG_Dx12 : IFGFeature_Dx12
{
    void CreateObjects(ID3D12Device* InDevice) override;
};
struct XeFG_Dx12 : IFGFeature_Dx12
{
    void CreateObjects(ID3D12Device* InDevice) override;
};

// The HUD-less format gate's inputs
struct FakeDesc
{
    DXGI_FORMAT Format;
};
struct FakeResource
{
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    FakeDesc GetDesc() { return { format }; }
};
struct FrameResource
{
    FakeResource* resource = nullptr;
    FakeResource* GetResource() { return resource; }
};
