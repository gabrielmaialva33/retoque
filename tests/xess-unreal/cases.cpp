// Host fixture: production function bodies are inserted by run.py; no Windows or GPU calls.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_set>

// DECODER

using UINT = unsigned;
using HMODULE = void*;
using PVOID = void*;
constexpr int NO_ERROR = 0;
#define FAILED(value) ((value) < 0)
#define IID_PPV_ARGS(value) (value)
#define LOG_INFO(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)
#define LOG_ERROR(...) ((void) 0)
constexpr int D3D12_FENCE_FLAG_NONE = 0;
constexpr int D3D12_COMMAND_LIST_TYPE_DIRECT = 0;
constexpr int D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 1;
struct ID3D12Device;
struct Desc
{
    uint64_t Width = 100;
    UINT Height = 100;
};
struct ID3D12Resource
{
    Desc GetDesc() { return {}; }
};
struct ID3D12CommandAllocator
{
};
struct ID3D12Fence
{
};
struct ID3D12GraphicsCommandList
{
    void** vtable;
    ID3D12Device* device;
    bool failDevice = false;
    int gets = 0;
    int GetDevice(ID3D12Device** out);
    void SetName(const wchar_t*) {}
    int Close() { return 0; }
};
struct D3D12_COMMAND_QUEUE_DESC
{
    int Type;
};
void* vtable[27] {};
struct ID3D12CommandQueue
{
    void** vtable;
    void Release() {}
};
struct ID3D12Device
{
    int releases = 0;
    int Release() { return ++releases; }
    int CreateFence(int, int, ID3D12Fence** out)
    {
        static ID3D12Fence f;
        *out = &f;
        return 0;
    }
    int CreateCommandAllocator(int, ID3D12CommandAllocator** out)
    {
        static ID3D12CommandAllocator a;
        *out = &a;
        return 0;
    }
    int CreateCommandList(int, int, ID3D12CommandAllocator*, void*, ID3D12GraphicsCommandList** out)
    {
        static ID3D12GraphicsCommandList c { vtable, this };
        *out = &c;
        return 0;
    }
    int CreateCommandQueue(D3D12_COMMAND_QUEUE_DESC*, ID3D12CommandQueue** out)
    {
        static ID3D12CommandQueue q { vtable };
        *out = &q;
        return 0;
    }
};
int ID3D12GraphicsCommandList::GetDevice(ID3D12Device** out)
{
    ++gets;
    if (failDevice)
        return -1;
    *out = device;
    return 0;
}
struct DS_Dx12
{
    DS_Dx12(const char*, ID3D12Device*) {}
    bool IsInit() { return true; }
    ID3D12Resource* Buffer()
    {
        static ID3D12Resource resource;
        return &resource;
    }
    bool CreateBufferResource(ID3D12Device*, ID3D12Resource*, UINT, UINT, int) { return true; }
};
constexpr size_t CopyListCount = 2;
std::array<ID3D12CommandAllocator*, CopyListCount> _allocators {};
std::array<ID3D12GraphicsCommandList*, CopyListCount> _copyLists {};
ID3D12Fence* _fence = nullptr;
ID3D12Device* _device = nullptr;
DS_Dx12* _shader = nullptr;
std::recursive_mutex _mutex;
std::atomic<UINT> _candidateCount { 1 }, _minWidth { 100 }, _minHeight { 100 };
std::unordered_set<ID3D12GraphicsCommandList*> _requests;
unsigned _requestCount = 0, _requestsSinceCopy = 0;
constexpr unsigned StaleRequests = 60;
using PFN_CommandListReset = void*;
using PFN_ResourceBarrier = void*;
using PFN_ExecuteCommandLists = void*;
using PFN_FlattenVelocity = void*;
using PFN_AddVelocityFlatteningXeSSPass = void*;
using PFN_GetSecondaryViewRectSize = void*;
void* o_CommandListReset = nullptr;
void* o_ResourceBarrier = nullptr;
void* o_ExecuteCommandLists = nullptr;
void* o_FlattenVelocity = nullptr;
void* o_AddVelocityFlatteningXeSSPass = nullptr;
void* o_GetSecondaryViewRectSize = nullptr;
const wchar_t* _xessVelocityName = nullptr;
int32_t _viewRectOffset = 0;
void hkCommandListReset() {}
void hkResourceBarrier() {}
void hkExecuteCommandLists() {}
void hkFlattenVelocity() {}
void hkAddVelocityFlatteningXeSSPass() {}
void hkGetSecondaryViewRectSize() {}
int commitResult = 0;
int commitCalls = 0;
int DetourTransactionBegin() { return 0; }
int DetourUpdateThread(void*) { return 0; }
void* GetCurrentThread() { return nullptr; }
template <class T> int DetourAttach(void**, T) { return 0; }
int DetourTransactionCommit()
{
    ++commitCalls;
    return commitResult;
}
namespace scanner
{
uintptr_t FindWideString(HMODULE, const wchar_t*) { return 0x10000; }
uintptr_t FindRipReference(HMODULE, uintptr_t) { return 0x20000; }
uintptr_t GetFunctionStart(uintptr_t) { return 0x30000; }
} // namespace scanner
bool inlined = false, viewHelper = true;
bool StoresThroughRdx(uintptr_t) { return inlined; }
uintptr_t CallAfterName(uintptr_t, bool& argument)
{
    argument = true;
    return 0x40000;
}
void FindViewSizeAccess(uintptr_t, uintptr_t, uintptr_t& helper, int32_t& offset)
{
    helper = viewHelper ? 0x50000 : 0;
    offset = viewHelper ? 0x280 : 0;
}
struct Option
{
    bool enabled = true;
    bool value_or_default() { return enabled; }
};
struct Config
{
    Option XeSSUnrealDepth;
    static Config* Instance()
    {
        static Config c;
        return &c;
    }
};
int scanCount = 0;
namespace XeSSUnreal
{
void Hook();
bool Hooked();
} // namespace XeSSUnreal
// PRODUCTION

int failures = 0, checks = 0;
#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            ++failures;                                                                                                \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                          \
        }                                                                                                              \
    } while (false)
void clear()
{
    delete _shader;
    _shader = nullptr;
    _device = nullptr;
    o_CommandListReset = o_ResourceBarrier = o_ExecuteCommandLists = nullptr;
    o_FlattenVelocity = o_AddVelocityFlatteningXeSSPass = o_GetSecondaryViewRectSize = nullptr;
    _viewRectOffset = 0;
    _xessVelocityName = nullptr;
    _requests.clear();
    _requestCount = _requestsSinceCopy = 0;
}
int main()
{
    for (auto& entry : vtable)
        entry = reinterpret_cast<void*>(0x12340);
    // Both MOV encodings, all 16 source/destination registers: 32-bit writes kill, 64-bit copies propagate.
    for (unsigned opcode : { 0x89u, 0x8Bu })
        for (unsigned src = 0; src < 16; ++src)
            for (unsigned dst = 0; dst < 16; ++dst)
                for (bool wide : { false, true })
                {
                    const auto reg = opcode == 0x8B ? dst : src, rm = opcode == 0x8B ? src : dst;
                    uint8_t bytes[8] { uint8_t(0x40 | (wide ? 8 : 0) | ((reg >> 3) << 2) | (rm >> 3)), uint8_t(opcode),
                                       uint8_t(0xC0 | ((reg & 7) << 3) | (rm & 7)) };
                    uint32_t mask = (1u << src) | (1u << dst);
                    int32_t offset = 0;
                    XeSSUnrealDecode::TrackViewRegisters(bytes, mask, offset);
                    CHECK(mask == (wide ? ((1u << src) | (1u << dst)) : ((1u << src) & ~(1u << dst))));
                    uint8_t load[8] { uint8_t(0x48 | (dst >> 3)), 0x8B, uint8_t(0x80 | (dst & 7)), 0x80, 0x02 };
                    XeSSUnrealDecode::TrackViewRegisters(load, mask, offset);
                    CHECK(offset == (wide && (dst & 7) != 4 ? 0x280 : 0));
                }
    { // No REX, negative/unaligned displacement, first field wins; stores do not kill the base.
        uint32_t mask = 2;
        int32_t offset = 0;
        uint8_t narrow[8] { 0x89, 0xC9 };
        XeSSUnrealDecode::TrackViewRegisters(narrow, mask, offset);
        CHECK(mask == 0);
        uint8_t load[8] { 0x0F, 0x10, 0x81, 0xF0, 0xFF, 0xFF, 0xFF };
        mask = 2;
        XeSSUnrealDecode::TrackViewRegisters(load, mask, offset);
        CHECK(offset == -16 && mask == 2);
        load[3] = 32;
        XeSSUnrealDecode::TrackViewRegisters(load, mask, offset);
        CHECK(offset == -16);
        uint8_t store[8] { 0x48, 0x89, 0x81, 0x80, 0x02 };
        XeSSUnrealDecode::TrackViewRegisters(store, mask, offset);
        CHECK(mask == 2);
        uint8_t lea[8] { 0x48, 0x8D, 0x89, 0x80, 0x02 };
        XeSSUnrealDecode::TrackViewRegisters(lea, mask, offset);
        CHECK(mask == 0);
    }
    ID3D12Device first, second;
    ID3D12GraphicsCommandList list { vtable, &first }, other { vtable, &second };
    ID3D12Resource layout;
    for (int error : { 5, 87 })
    {
        commitResult = error;
        for (bool inlineCase : { false, true })
            for (bool helper : { false, true })
            {
                clear();
                inlined = inlineCase;
                viewHelper = helper;
                CHECK(!HookModule(nullptr));
                CHECK(!XeSSUnreal::Hooked());
                CHECK(o_GetSecondaryViewRectSize == nullptr && _viewRectOffset == 0 && _xessVelocityName == nullptr);
            }
        clear();
        CHECK(!InitCopy(&list, &first));
        CHECK(o_CommandListReset == nullptr && o_ResourceBarrier == nullptr && o_ExecuteCommandLists == nullptr);
        CHECK(RequestCopy(&list, &layout) == nullptr);
        CHECK(_requests.empty());
    }
    commitResult = 0;
    for (bool inlineCase : { false, true })
        for (bool helper : { false, true })
        {
            clear();
            inlined = inlineCase;
            viewHelper = helper;
            CHECK(HookModule(nullptr));
            CHECK(XeSSUnreal::Hooked());
            CHECK(inlineCase ? _xessVelocityName != nullptr : (o_GetSecondaryViewRectSize != nullptr) == helper);
        }
    clear();
    CHECK(RequestCopy(&list, &layout) != nullptr);
    CHECK(_requests.contains(&list));
    const auto count = _requestCount;
    const auto releases = second.releases;
    CHECK(RequestCopy(&other, &layout) == nullptr);
    CHECK(!_requests.contains(&other));
    CHECK(_requestCount == count);
    CHECK(second.releases == releases + 1);
    list.failDevice = true;
    _requests.clear();
    CHECK(RequestCopy(&list, &layout) == nullptr);
    CHECK(_requests.empty());
    list.failDevice = false;
    CHECK(RequestCopy(&list, &layout) != nullptr);
    clear();
    scanCount = 0;
    Config::Instance()->XeSSUnrealDepth.enabled = false;
    XeSSUnreal::Hook();
    CHECK(scanCount == 0 && !XeSSUnreal::Hooked());
    Config::Instance()->XeSSUnrealDepth.enabled = true;
    XeSSUnreal::Hook();
    CHECK(scanCount == 1);
    clear();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
