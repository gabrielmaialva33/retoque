#include "pch.h"
#include "DlssNr_PresentDepthDx12.h"
#include "DlssNr_PresentDepth.h"
#include "DlssNr_PresentDepthTables.h"
#include <Config.h>
#include <State.h>
#include <Logger.h>
#include <hooks/Hook_Utils.h>
#include <resource_tracking/ResTrack_Dx12.h>
#include <detours/detours.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <memory>
#include <utility>
#include <wrl/client.h>

namespace DlssNr::PresentDepthDx12
{
using namespace PresentDepth;
using Microsoft::WRL::ComPtr;
namespace
{
constexpr size_t kDescriptors = 8192, kLists = 512, kQueues = 32;
constexpr uint64_t kInternal = UINT64_MAX;
// Private CPU recording-generation stamp; never a GPU command or a resource reference.
constexpr GUID kRecording = { 0xf75df2a4, 0x92e1, 0x49b7, { 0x89, 0x71, 0x6a, 0x50, 0xdb, 0x9c, 0x37, 0x21 } };
constexpr GUID kReleaseNotice = { 0x18a9a576, 0xe24e, 0x4f19, { 0xa9, 0xb3, 0xa2, 0x86, 0xdd, 0xa7, 0x63, 0xfc } };
int64_t Now()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
thread_local unsigned internal = 0;
thread_local Frame* currentFrame = nullptr;
struct ListSlot
{
    std::atomic<uintptr_t> key { 0 };
    Recording recording;
};
struct QueueSlot
{
    uintptr_t key = 0;
    unsigned inflight = 0;
    bool unknown = false;
};
struct Capture
{
    Id id = 0;
    bool inverted = false, full = false;
};
struct Owner
{
    std::mutex mutex; // leaf: never held across any runtime call, log or GPU wait
    Model model;
    Window window;
    AddressTable<kDescriptors> descriptors;
    AddressTable<kDescriptors> resources;
    std::array<ListSlot, kLists> lists {};
    std::array<QueueSlot, kQueues> queues {};
    std::array<Capture, 4> captures {};
    std::atomic<uint64_t> stamp { 0 }, calls { 0 }, samples { 0 }, sampledNs { 0 };
    std::atomic<bool> overflow { false };
    std::atomic<bool> running { false };
    ID3D12Device* device = nullptr; // identity only, no observer resource ownership
    uint64_t presenter = 0;
    uint64_t commits = 0;
    bool intervalUnknown = false, installed = false;
    UINT stride = 0;
    QueueSlot* Queue(uintptr_t key)
    {
        for (auto& q : queues)
            if (q.key == key)
                return &q;
        for (auto& q : queues)
            if (!q.key)
            {
                q.key = key;
                return &q;
            }
        overflow.store(true);
        return nullptr;
    }
    Recording* Record(ID3D12GraphicsCommandList* list, bool insert = false)
    {
        const uintptr_t key = reinterpret_cast<uintptr_t>(list);
        const auto first = (key >> 4) % kLists;
        for (size_t i = 0; i < kLists; ++i)
        {
            auto& s = lists[(first + i) % kLists];
            auto found = s.key.load(std::memory_order_acquire);
            if (!found && insert)
            {
                if (s.key.compare_exchange_strong(found, key, std::memory_order_acq_rel))
                    return &s.recording;
            }
            if (found == key)
                return &s.recording;
            if (!found && !insert)
                return nullptr;
        }
        overflow.store(true);
        return nullptr;
    }
};
// No allocation, lock, TLS access or clock on the default path. Owner is allocated only by successful gated setup.
std::atomic<Owner*> owner { nullptr };
std::mutex installMutex;
Owner* Running()
{
    auto* s = owner.load(std::memory_order_acquire);
    return s && s->running.load(std::memory_order_acquire) ? s : nullptr;
}
struct Sample
{
    Owner* s;
    int64_t start = 0;
    explicit Sample(Owner* o) : s(o)
    {
        if (s && !internal && (s->calls.fetch_add(1, std::memory_order_relaxed) & 63) == 0)
            start = Now();
    }
    void Finish()
    {
        if (start)
        {
            s->sampledNs.fetch_add(uint64_t(Now() - start), std::memory_order_relaxed);
            s->samples.fetch_add(1, std::memory_order_relaxed);
            start = 0;
        }
    }
    ~Sample() { Finish(); }
};
template <class T> uint64_t ObjectIdentity(T* object, Owner& s)
{
    uint64_t token = 0;
    UINT bytes = sizeof(token);
    if (SUCCEEDED(object->GetPrivateData(kRecording, &bytes, &token)) && bytes == sizeof(token) && token &&
        token != kInternal)
        return token;
    token = ++s.stamp;
    if (!token || token == kInternal || FAILED(object->SetPrivateData(kRecording, sizeof(token), &token)))
    {
        s.overflow.store(true);
        return 0;
    }
    // Concurrent first observations can conservatively disagree, but cannot reuse a destroyed object's identity.
    return token;
}
bool OurDevice(ID3D12Device* device, Owner* s) { return ResTrack_Dx12::NrRealObject(device) == s->device; }
void Bind(ID3D12GraphicsCommandList* list, const D3D12_CPU_DESCRIPTOR_HANDLE* handle)
{
    auto* s = Running();
    if (!s || internal)
        return;
    auto* r = s->Record(list);
    if (!r)
        return;
    r->bound = 0;
    if (!handle)
        return;
    const View v = s->descriptors.Get(handle->ptr);
    if (!v.id)
    {
        r->overflow = true;
        return;
    }
    if (v.flags & (1ull << 32))
        return; // read-only depth
    r->bound = v.id;
    Event e;
    e.kind = Kind::Bind;
    e.id = v.id;
    e.viewFormat = uint32_t(v.flags);
    e.timeNs = Now();
    r->Add(e);
    if (v.flags & (1ull << 33))
    {
        e.kind = Kind::Unsupported;
        r->Add(e);
    }
}
void Draw(ID3D12GraphicsCommandList* list, bool nonempty)
{
    auto* s = Running();
    if (!s || internal)
        return;
    if (auto* r = s->Record(list); r && r->bound && nonempty)
    {
        Event e;
        e.kind = Kind::Draw;
        e.id = r->bound;
        e.count = 1;
        r->Add(e);
    }
}
void UnsupportedBound(ID3D12GraphicsCommandList* list)
{
    auto* s = Running();
    if (!s || internal)
        return;
    if (auto* r = s->Record(list); r && r->bound)
    {
        Event e;
        e.kind = Kind::Unsupported;
        e.id = r->bound;
        r->Add(e);
    }
}
void Clear(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE handle, float depth, bool full)
{
    auto* s = Running();
    if (!s || internal)
        return;
    if (auto* r = s->Record(list))
    {
        const auto v = s->descriptors.Get(handle.ptr);
        if (!v.id)
        {
            r->overflow = true;
            return;
        }
        Event e;
        e.kind = Kind::Clear;
        e.id = v.id;
        e.depth = depth;
        e.complete = full;
        r->Add(e);
    }
}
#define ORIGINAL(name, iface, method)                                                                                  \
    using name##Fn = rewrite_signature<decltype(&iface::method)>::type;                                                \
    name##Fn original##name = nullptr
ORIGINAL(CreateList, ID3D12Device, CreateCommandList);
ORIGINAL(CreateList1, ID3D12Device4, CreateCommandList1);
ORIGINAL(View, ID3D12Device, CreateDepthStencilView);
ORIGINAL(Copy, ID3D12Device, CopyDescriptors);
ORIGINAL(CopySimple, ID3D12Device, CopyDescriptorsSimple);
ORIGINAL(Bind, ID3D12GraphicsCommandList, OMSetRenderTargets);
ORIGINAL(Draw, ID3D12GraphicsCommandList, DrawInstanced);
ORIGINAL(Indexed, ID3D12GraphicsCommandList, DrawIndexedInstanced);
ORIGINAL(Clear, ID3D12GraphicsCommandList, ClearDepthStencilView);
ORIGINAL(Barriers, ID3D12GraphicsCommandList, ResourceBarrier);
ORIGINAL(Reset, ID3D12GraphicsCommandList, Reset);
ORIGINAL(Close, ID3D12GraphicsCommandList, Close);
ORIGINAL(Bundle, ID3D12GraphicsCommandList, ExecuteBundle);
ORIGINAL(Indirect, ID3D12GraphicsCommandList, ExecuteIndirect);
ORIGINAL(BeginPass, ID3D12GraphicsCommandList4, BeginRenderPass);
ORIGINAL(EndPass, ID3D12GraphicsCommandList4, EndRenderPass);
ORIGINAL(Mesh, ID3D12GraphicsCommandList6, DispatchMesh);
ORIGINAL(Enhanced, ID3D12GraphicsCommandList7, Barrier);
#undef ORIGINAL

void __stdcall Released(void* data)
{
    auto* s = Running();
    if (!s || !data || State::Instance().isShuttingDown)
        return;
    std::lock_guard lock(s->mutex);
    const auto key = reinterpret_cast<uintptr_t>(data);
    s->model.Forget(s->resources.Get(key).id);
    s->resources.Put(key, {});
}
bool NoticeRelease(ID3D12Resource* resource)
{
    if (State::Instance().isShuttingDown)
        return false;
    uint8_t noticed = 0;
    UINT bytes = sizeof(noticed);
    if (SUCCEEDED(resource->GetPrivateData(kReleaseNotice, &bytes, &noticed)) && bytes == sizeof(noticed) && noticed)
        return true;
    ComPtr<ID3DDestructionNotifier> notifier;
    UINT callback = 0;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&notifier))) ||
        FAILED(notifier->RegisterDestructionCallback(&Released, resource, &callback)))
        return false;
    noticed = 1;
    resource->SetPrivateData(kReleaseNotice, sizeof(noticed), &noticed);
    return true;
}
// Resources created through unhooked creation variants can still be identified at DSV creation, but their state
// and placement stay unknown. Never infer a creation state from the DSV or the optimized clear value.
Id ResourceId(ID3D12Device* device, ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc, bool known,
              uint32_t state, ID3D12Heap* heap, Placement placement)
{
    auto* s = Running();
    if (!s || !resource || !OurDevice(device, s))
        return 0;
    const auto key = reinterpret_cast<uintptr_t>(resource);
    if (const auto current = s->resources.Get(key).id)
        return current;
    const bool lifetime = NoticeRelease(resource); // outside observer lock
    Shape shape;
    shape.width = uint32_t(std::min<uint64_t>(desc.Width, UINT32_MAX));
    shape.height = desc.Height;
    shape.format = uint32_t(desc.Format);
    shape.samples = desc.SampleDesc.Count;
    shape.texture2D = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    shape.singleSlice = desc.DepthOrArraySize == 1;
    shape.placement = placement;
    std::lock_guard lock(s->mutex);
    if (const auto current = s->resources.Get(key).id)
        return current;
    const Id id = s->model.Create(shape, lifetime, known, state, reinterpret_cast<uintptr_t>(heap));
    if (!id || !s->resources.Put(key, { id, 0 }))
        s->overflow.store(true);
    return id;
}
void NewList(ID3D12Device* device, HRESULT result, void** returned, bool closed)
{
    auto* s = Running();
    if (!s || FAILED(result) || !returned || !*returned || !OurDevice(device, s))
        return;
    Sample sample(s);
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(static_cast<IUnknown*>(*returned)->QueryInterface(IID_PPV_ARGS(&list))))
        return;
    auto* native = static_cast<ID3D12GraphicsCommandList*>(ResTrack_Dx12::NrRealObject(list.Get()));
    const uint64_t token = internal ? kInternal : ++s->stamp;
    const bool known = SUCCEEDED(native->SetPrivateData(kRecording, sizeof(token), &token));
    if (!internal)
        if (auto* r = s->Record(native, true))
        {
            r->Reset(token, known);
            r->closed = closed;
        }
}
HRESULT STDMETHODCALLTYPE HookCreateList(ID3D12Device* device, UINT node, D3D12_COMMAND_LIST_TYPE type,
                                         ID3D12CommandAllocator* allocator, ID3D12PipelineState* pipeline, REFIID iid,
                                         void** result)
{
    const HRESULT hr = originalCreateList(device, node, type, allocator, pipeline, iid, result);
    NewList(device, hr, result, false);
    return hr;
}
VALIDATE_MEMBER_HOOK(HookCreateList, CreateListFn)
HRESULT STDMETHODCALLTYPE HookCreateList1(ID3D12Device4* device, UINT node, D3D12_COMMAND_LIST_TYPE type,
                                          D3D12_COMMAND_LIST_FLAGS flags, REFIID iid, void** result)
{
    const HRESULT hr = originalCreateList1(device, node, type, flags, iid, result);
    NewList(device, hr, result, true);
    return hr;
}
VALIDATE_MEMBER_HOOK(HookCreateList1, CreateList1Fn)
void STDMETHODCALLTYPE HookView(ID3D12Device* device, ID3D12Resource* resource,
                                const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    originalView(device, resource, desc, handle);
    auto* s = Running();
    if (!s || internal || !OurDevice(device, s))
        return;
    Sample sample(s);
    View v;
    if (resource)
    {
        const auto shape = resource->GetDesc();
        v.id = ResourceId(device, resource, shape, false, 0, nullptr, Placement::Unknown);
        v.flags = uint32_t(desc ? desc->Format : shape.Format);
        if (desc && (desc->Flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH))
            v.flags |= 1ull << 32;
        if (desc && !((desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D && desc->Texture2D.MipSlice == 0) ||
                      desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DMS))
            v.flags |= 1ull << 33;
    }
    std::lock_guard lock(s->mutex);
    if (!s->descriptors.Put(handle.ptr, v))
        s->overflow.store(true);
}
VALIDATE_MEMBER_HOOK(HookView, ViewFn)
void CopyViews(ID3D12Device* device, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* dest, const UINT* sizes, UINT ns,
               const D3D12_CPU_DESCRIPTOR_HANDLE* src, const UINT* sourceSizes, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    auto* s = Running();
    if (!s || internal || type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV || !OurDevice(device, s))
        return;
    Sample sample(s);
    if (nd > kDescriptors || ns > kDescriptors || (nd && !dest) || (ns && !src))
    {
        s->overflow.store(true);
        return;
    }
    // Capture sources before any destination writes, including overlapping ranges.
    std::array<View, kDescriptors> values {};
    size_t count = 0;
    std::lock_guard lock(s->mutex);
    for (UINT i = 0; i < ns; ++i)
    {
        const UINT n = sourceSizes ? sourceSizes[i] : 1;
        if (n > values.size() - count)
        {
            s->overflow.store(true);
            return;
        }
        for (UINT j = 0; j < n; ++j)
            values[count++] = s->descriptors.Get(src[i].ptr + SIZE_T(j) * s->stride);
    }
    size_t at = 0;
    for (UINT i = 0; i < nd; ++i)
    {
        const UINT n = sizes ? sizes[i] : 1;
        if (n > count - at)
        {
            s->overflow.store(true);
            return;
        }
        for (UINT j = 0; j < n; ++j)
            if (!s->descriptors.Put(dest[i].ptr + SIZE_T(j) * s->stride, values[at++]))
                s->overflow.store(true);
    }
    if (at != count)
        s->overflow.store(true);
}
void STDMETHODCALLTYPE HookCopy(ID3D12Device* d, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* dest, const UINT* sizes,
                                UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* src, const UINT* ss,
                                D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    CopyViews(d, nd, dest, sizes, ns, src, ss, type);
    originalCopy(d, nd, dest, sizes, ns, src, ss, type);
}
VALIDATE_MEMBER_HOOK(HookCopy, CopyFn)
void STDMETHODCALLTYPE HookCopySimple(ID3D12Device* d, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dest,
                                      D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    CopyViews(d, 1, &dest, &n, 1, &src, &n, type);
    originalCopySimple(d, n, dest, src, type);
}
VALIDATE_MEMBER_HOOK(HookCopySimple, CopySimpleFn)
void STDMETHODCALLTYPE HookBind(ID3D12GraphicsCommandList* l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rt, BOOL range,
                                const D3D12_CPU_DESCRIPTOR_HANDLE* depth)
{
    Sample sample(Running());
    Bind(l, depth);
    sample.Finish();
    originalBind(l, n, rt, range, depth);
}
VALIDATE_MEMBER_HOOK(HookBind, BindFn)
void STDMETHODCALLTYPE HookDraw(ID3D12GraphicsCommandList* l, UINT n, UINT instances, UINT start, UINT first)
{
    Sample sample(Running());
    Draw(l, n && instances);
    sample.Finish();
    originalDraw(l, n, instances, start, first);
}
VALIDATE_MEMBER_HOOK(HookDraw, DrawFn)
void STDMETHODCALLTYPE HookIndexed(ID3D12GraphicsCommandList* l, UINT n, UINT instances, UINT start, INT base,
                                   UINT first)
{
    Sample sample(Running());
    Draw(l, n && instances);
    sample.Finish();
    originalIndexed(l, n, instances, start, base, first);
}
VALIDATE_MEMBER_HOOK(HookIndexed, IndexedFn)
void STDMETHODCALLTYPE HookClear(ID3D12GraphicsCommandList* l, D3D12_CPU_DESCRIPTOR_HANDLE view,
                                 D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT n, const D3D12_RECT* rects)
{
    Sample sample(Running());
    if (flags & D3D12_CLEAR_FLAG_DEPTH)
        Clear(l, view, depth, n == 0);
    sample.Finish();
    originalClear(l, view, flags, depth, stencil, n, rects);
}
VALIDATE_MEMBER_HOOK(HookClear, ClearFn)
void STDMETHODCALLTYPE HookBarriers(ID3D12GraphicsCommandList* l, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
{
    if (auto* s = Running(); s && !internal)
    {
        Sample sample(s);
        if (auto* r = s->Record(l))
            for (UINT i = 0; i < count; ++i)
            {
                const auto& b = barriers[i];
                Event e;
                if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                {
                    e.id = s->resources.Get(reinterpret_cast<uintptr_t>(b.Transition.pResource)).id;
                    if (!e.id)
                        continue;
                    e.kind = Kind::Transition;
                    e.before = b.Transition.StateBefore;
                    e.after = b.Transition.StateAfter;
                    e.complete = b.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE &&
                                 b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    r->Add(e);
                }
                else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING)
                {
                    e.kind = Kind::Alias;
                    if (!b.Aliasing.pResourceBefore || !b.Aliasing.pResourceAfter)
                        r->Add(e);
                    else
                    {
                        const auto before =
                            s->resources.Get(reinterpret_cast<uintptr_t>(b.Aliasing.pResourceBefore)).id;
                        const auto after = s->resources.Get(reinterpret_cast<uintptr_t>(b.Aliasing.pResourceAfter)).id;
                        // A non-depth resource could alias a known depth heap; without its placement, refuse
                        // conservatively.
                        if (!before || !after)
                            r->Add(e);
                        else
                        {
                            e.id = before;
                            r->Add(e);
                            e.id = after;
                            r->Add(e);
                        }
                    }
                }
            }
    }
    originalBarriers(l, count, barriers);
}
VALIDATE_MEMBER_HOOK(HookBarriers, BarriersFn)
HRESULT STDMETHODCALLTYPE HookReset(ID3D12GraphicsCommandList* l, ID3D12CommandAllocator* a, ID3D12PipelineState* p)
{
    const HRESULT hr = originalReset(l, a, p);
    if (auto* s = Running())
    {
        Sample sample(s);
        if (internal)
        {
            const auto token = kInternal;
            if (SUCCEEDED(hr))
                l->SetPrivateData(kRecording, sizeof(token), &token);
        }
        else
        {
            ComPtr<ID3D12Device> device;
            if (SUCCEEDED(l->GetDevice(IID_PPV_ARGS(&device))) && OurDevice(device.Get(), s))
            {
                const uint64_t token = ++s->stamp;
                const bool known = SUCCEEDED(hr) && SUCCEEDED(l->SetPrivateData(kRecording, sizeof(token), &token));
                if (auto* r = s->Record(l, true))
                    r->Reset(token, known);
            }
        }
    }
    return hr;
}
VALIDATE_MEMBER_HOOK(HookReset, ResetFn)
HRESULT STDMETHODCALLTYPE HookClose(ID3D12GraphicsCommandList* l)
{
    const HRESULT hr = originalClose(l);
    if (auto* s = Running())
    {
        Sample sample(s);
        if (internal)
        {
            // Never relabel a game list just because an internal operation visited it.
            if (!s->Record(l))
            {
                const auto token = kInternal;
                l->SetPrivateData(kRecording, sizeof(token), &token);
            }
        }
        else if (auto* r = s->Record(l))
            r->closed = SUCCEEDED(hr);
    }
    return hr;
}
VALIDATE_MEMBER_HOOK(HookClose, CloseFn)
void STDMETHODCALLTYPE HookBundle(ID3D12GraphicsCommandList* l, ID3D12GraphicsCommandList* b)
{
    Sample sample(Running());
    UnsupportedBound(l);
    sample.Finish();
    originalBundle(l, b);
}
VALIDATE_MEMBER_HOOK(HookBundle, BundleFn)
void STDMETHODCALLTYPE HookIndirect(ID3D12GraphicsCommandList* l, ID3D12CommandSignature* signature, UINT count,
                                    ID3D12Resource* args, UINT64 offset, ID3D12Resource* counter, UINT64 counterOffset)
{
    Sample sample(Running());
    UnsupportedBound(l);
    sample.Finish();
    originalIndirect(l, signature, count, args, offset, counter, counterOffset);
}
VALIDATE_MEMBER_HOOK(HookIndirect, IndirectFn)
void STDMETHODCALLTYPE HookMesh(ID3D12GraphicsCommandList6* l, UINT x, UINT y, UINT z)
{
    Sample sample(Running());
    UnsupportedBound(l);
    sample.Finish();
    originalMesh(l, x, y, z);
}
VALIDATE_MEMBER_HOOK(HookMesh, MeshFn)
void STDMETHODCALLTYPE HookBeginPass(ID3D12GraphicsCommandList4* l, UINT n,
                                     const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets,
                                     const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth, D3D12_RENDER_PASS_FLAGS flags)
{
    Sample sample(Running());
    Bind(l, depth ? &depth->cpuDescriptor : nullptr);
    if (depth)
    {
        if (depth->DepthBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
            Clear(l, depth->cpuDescriptor, depth->DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth, true);
        else if (depth->DepthBeginningAccess.Type != D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE)
            UnsupportedBound(l);
        if (depth->DepthEndingAccess.Type != D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE ||
            flags != D3D12_RENDER_PASS_FLAG_NONE)
            UnsupportedBound(l); // discard/resolve/suspend/resume require a separate proof
    }
    sample.Finish();
    originalBeginPass(l, n, targets, depth, flags);
}
VALIDATE_MEMBER_HOOK(HookBeginPass, BeginPassFn)
void STDMETHODCALLTYPE HookEndPass(ID3D12GraphicsCommandList4* l)
{
    Sample sample(Running());
    Bind(l, nullptr);
    sample.Finish();
    originalEndPass(l);
}
VALIDATE_MEMBER_HOOK(HookEndPass, EndPassFn)
void STDMETHODCALLTYPE HookEnhanced(ID3D12GraphicsCommandList7* l, UINT n, const D3D12_BARRIER_GROUP* groups)
{
    if (auto* s = Running(); s && !internal)
    {
        Sample sample(s);
        if (auto* r = s->Record(l))
            for (UINT i = 0; i < n; ++i)
            {
                if (groups[i].Type == D3D12_BARRIER_TYPE_TEXTURE)
                    for (UINT j = 0; j < groups[i].NumBarriers; ++j)
                    {
                        Event e;
                        e.kind = Kind::Enhanced;
                        e.id =
                            s->resources.Get(reinterpret_cast<uintptr_t>(groups[i].pTextureBarriers[j].pResource)).id;
                        if (e.id)
                            r->Add(e);
                    }
                else if (groups[i].Type == D3D12_BARRIER_TYPE_GLOBAL)
                    r->overflow = true;
            }
    }
    originalEnhanced(l, n, groups);
}
VALIDATE_MEMBER_HOOK(HookEnhanced, EnhancedFn)
} // namespace

bool Active() { return Running() != nullptr; }
InternalScope::InternalScope() : _active(Active())
{
    if (_active)
        ++internal;
}
InternalScope::~InternalScope()
{
    if (_active)
        --internal;
}
void Created(ID3D12Device* device, const D3D12_RESOURCE_DESC* desc, ID3D12Resource* resource,
             D3D12_RESOURCE_STATES state, ID3D12Heap* heap)
{
    if (!Running() || internal || ScopedInternalResourceCreation::Active() || !desc || !resource ||
        !(desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        return;
    ResourceId(device, resource, *desc, true, uint32_t(state), heap, heap ? Placement::Placed : Placement::Committed);
}
void Captured(size_t slot, ID3D12Resource* source, bool inverted, bool fullExtent)
{
    auto* s = Running();
    if (!s || slot >= s->captures.size())
        return;
    source = static_cast<ID3D12Resource*>(ResTrack_Dx12::NrRealObject(source));
    std::lock_guard lock(s->mutex);
    const Id id = s->resources.Get(reinterpret_cast<uintptr_t>(source)).id;
    const auto* r = s->model.Find(id);
    s->captures[slot] = { r && r->lifetime ? id : 0, inverted, fullExtent };
    ++s->commits;
}
void Eligible(size_t slot)
{
    auto* s = Running();
    if (!s || !currentFrame || slot >= s->captures.size())
        return;
    std::lock_guard lock(s->mutex);
    const auto& capture = s->captures[slot];
    const auto* r = s->model.Find(capture.id);
    if (!r || !r->lifetime || !capture.full || r->shape.width != currentFrame->width ||
        r->shape.height != currentFrame->height)
        return;
    currentFrame->eligible = true;
    currentFrame->truth = capture.id;
    currentFrame->inverted = capture.inverted;
}
Submission::Submission(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    auto* s = Running();
    if (!s)
        return;
    Sample sample(s);
    ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !OurDevice(device.Get(), s))
        return;
    // Query CPU markers before taking the leaf lock. A caller owns these command lists through the real call.
    constexpr size_t maximum = 1024;
    std::array<uint64_t, maximum> tokens {};
    if (count <= maximum)
        for (UINT i = 0; i < count; ++i)
        {
            UINT size = sizeof(uint64_t);
            if (FAILED(lists[i]->GetPrivateData(kRecording, &size, &tokens[i])) || size != sizeof(uint64_t))
                tokens[i] = 0;
        }
    const uint64_t queueId = ObjectIdentity(queue, *s);
    if (!queueId)
        return;
    std::lock_guard lock(s->mutex);
    auto* q = s->Queue(queueId);
    if (!q)
        return;
    _queue = q;
    if (++q->inflight != 1 || count > maximum)
        q->unknown = s->intervalUnknown = true;
    if (count > maximum)
    {
        s->model.UnknownSubmission();
        return;
    }
    if (q->inflight != 1)
        s->model.UnknownSubmission();
    for (UINT i = 0; i < count; ++i)
    {
        if (tokens[i] == kInternal)
            continue;
        auto* r = s->Record(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
        if (!r || !r->Usable(tokens[i]))
        {
            q->unknown = s->intervalUnknown = true;
            s->model.UnknownSubmission();
            continue;
        }
        for (size_t j = 0; j < r->size; ++j)
            s->model.Apply(r->events[j], q->key);
    }
}
Submission::~Submission()
{
    auto* s = Running();
    if (!s || !_queue)
        return;
    std::lock_guard lock(s->mutex);
    --static_cast<QueueSlot*>(_queue)->inflight;
}
Observation::Observation(bool ownsPresent, IDXGISwapChain3* chain, ID3D12CommandQueue* queue)
{
    auto* s = Running();
    if (!s || !ownsPresent || !chain || !queue || currentFrame)
        return;
    Sample sample(s);
    DXGI_SWAP_CHAIN_DESC1 desc {};
    ComPtr<ID3D12Device> device;
    if (FAILED(chain->GetDesc1(&desc)) || FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) ||
        !OurDevice(device.Get(), s))
        return;
    queue = static_cast<ID3D12CommandQueue*>(ResTrack_Dx12::NrRealObject(queue));
    auto* nativeChain = static_cast<IDXGISwapChain3*>(ResTrack_Dx12::NrRealObject(chain));
    const uint64_t presenter = ObjectIdentity(nativeChain, *s);
    const uint64_t queueId = ObjectIdentity(queue, *s);
    if (!presenter || !queueId)
        return;
    static thread_local Frame frame;
    std::lock_guard lock(s->mutex);
    auto* q = s->Queue(queueId);
    if (!q)
        return;
    const bool inflight =
        std::any_of(s->queues.begin(), s->queues.end(), [](const QueueSlot& item) { return item.inflight != 0; });
    const bool unknown = s->intervalUnknown || q->unknown || inflight || s->overflow.load() ||
                         (s->presenter && s->presenter != presenter);
    if (!s->presenter)
        s->presenter = presenter;
    frame = s->model.Present(desc.Width, desc.Height, q->key, unknown, Now());
    for (auto& item : s->queues)
        item.unknown = item.inflight != 0;
    s->intervalUnknown = inflight;
    sample.Finish();
    frame.commits = std::exchange(s->commits, 0);
    frame.calls = s->calls.exchange(0);
    frame.sampledNs = s->sampledNs.exchange(0);
    frame.samples = s->samples.exchange(0);
    currentFrame = &frame;
    _active = true;
}
Observation::~Observation()
{
    auto* s = Running();
    if (!s || !_active || !currentFrame)
        return;
    std::string line, changed;
    {
        std::lock_guard lock(s->mutex);
        s->window.Add(*currentFrame, &line, &changed);
    }
    currentFrame = nullptr;
    if (!changed.empty())
        LOG_INFO("{}", changed);
    if (!line.empty())
        LOG_INFO("{}", line);
}
void Install(ID3D12Device* input)
{
    // Only log is an implementation. Invalid/future values have exactly the constant/off hook path.
    const auto& cfg = *Config::Instance();
    static const bool requested = Enabled(cfg.DlssNrEnabled.value_or_default(), cfg.DlssNrHookMethod.value_or_default(),
                                          cfg.DlssNrPresentDepth.value_or_default());
    if (!requested || !input || ScopedInternalResourceCreation::Active() ||
        State::Instance().swapchainInteropApi != SwapchainInteropApi::None)
        return;
    std::lock_guard install(installMutex);
    if (owner.load())
        return; // one installation attempt/device implementation per process
    auto s = std::make_unique<Owner>();
    auto* device = static_cast<ID3D12Device*>(ResTrack_Dx12::NrRealObject(input));
    s->device = device;
    s->stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (!s->stride ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list))))
        return;
    list->Close();
    auto* nativeList = static_cast<ID3D12GraphicsCommandList*>(ResTrack_Dx12::NrRealObject(list.Get()));
    auto** dv = *reinterpret_cast<void***>(device);
    auto** lv = *reinterpret_cast<void***>(nativeList);
    originalCreateList = reinterpret_cast<CreateListFn>(dv[12]);
    ComPtr<ID3D12Device4> device4;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device4))))
        originalCreateList1 = reinterpret_cast<CreateList1Fn>((*reinterpret_cast<void***>(device4.Get()))[51]);
    originalView = reinterpret_cast<ViewFn>(dv[21]);
    originalCopy = reinterpret_cast<CopyFn>(dv[23]);
    originalCopySimple = reinterpret_cast<CopySimpleFn>(dv[24]);
    originalBind = reinterpret_cast<BindFn>(lv[46]);
    originalDraw = reinterpret_cast<DrawFn>(lv[12]);
    originalIndexed = reinterpret_cast<IndexedFn>(lv[13]);
    originalClear = reinterpret_cast<ClearFn>(lv[47]);
    originalBarriers = reinterpret_cast<BarriersFn>(lv[26]);
    originalReset = reinterpret_cast<ResetFn>(lv[10]);
    originalClose = reinterpret_cast<CloseFn>(lv[9]);
    originalBundle = reinterpret_cast<BundleFn>(lv[27]);
    originalIndirect = reinterpret_cast<IndirectFn>(lv[59]);
    ComPtr<ID3D12GraphicsCommandList4> four;
    ComPtr<ID3D12GraphicsCommandList6> six;
    ComPtr<ID3D12GraphicsCommandList7> seven;
    if (SUCCEEDED(nativeList->QueryInterface(IID_PPV_ARGS(&four))))
    {
        auto** v = *reinterpret_cast<void***>(four.Get());
        originalBeginPass = reinterpret_cast<BeginPassFn>(v[68]);
        originalEndPass = reinterpret_cast<EndPassFn>(v[69]);
    }
    if (SUCCEEDED(nativeList->QueryInterface(IID_PPV_ARGS(&six))))
        originalMesh = reinterpret_cast<MeshFn>((*reinterpret_cast<void***>(six.Get()))[79]);
    if (SUCCEEDED(nativeList->QueryInterface(IID_PPV_ARGS(&seven))))
        originalEnhanced = reinterpret_cast<EnhancedFn>((*reinterpret_cast<void***>(seven.Get()))[80]);
    if (!ResTrack_Dx12::EnableNrSubmissionTracking(device))
        return;
    if (DetourTransactionBegin() != NO_ERROR)
        return;
    bool ok = DetourUpdateThread(GetCurrentThread()) == NO_ERROR;
#define ATTACH(name)                                                                                                   \
    if (original##name &&                                                                                              \
        DetourAttach(reinterpret_cast<PVOID*>(&original##name), reinterpret_cast<PVOID>(Hook##name)) != NO_ERROR)      \
    ok = false
    ATTACH(CreateList);
    ATTACH(CreateList1);
    ATTACH(View);
    ATTACH(Copy);
    ATTACH(CopySimple);
    ATTACH(Bind);
    ATTACH(Draw);
    ATTACH(Indexed);
    ATTACH(Clear);
    ATTACH(Barriers);
    ATTACH(Reset);
    ATTACH(Close);
    ATTACH(Bundle);
    ATTACH(Indirect);
    ATTACH(BeginPass);
    ATTACH(EndPass);
    ATTACH(Mesh);
    ATTACH(Enhanced);
#undef ATTACH
    if (!ok)
    {
        DetourTransactionAbort();
        LOG_WARN("DLSS-NR present depth: observer hook transaction refused; nothing observed");
        return;
    }
    // Publish CPU storage before enabling callbacks. Never free this storage while hooks could be in flight.
    auto* published = s.release();
    owner.store(published, std::memory_order_release);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        LOG_WARN("DLSS-NR present depth: observer hook commit failed; nothing observed");
        return;
    }
    published->installed = true;
    published->running.store(true, std::memory_order_release);
    LOG_INFO("DLSS-NR present depth: log-only observer installed, no depth supplied to the model");
}
void DeviceDestroyed(IUnknown* device)
{
    auto* s = Running();
    if (s && device == s->device)
        s->running.store(false, std::memory_order_release);
}
void Stop()
{
    auto* s = owner.load();
    if (!s || !s->installed)
        return;
    s->running.store(false, std::memory_order_release);
    // Existing resource destruction callbacks remain owned by ResTrack. These detours are detached separately.
    if (DetourTransactionBegin() != NO_ERROR)
        return;
    DetourUpdateThread(GetCurrentThread());
#define DETACH(name)                                                                                                   \
    if (original##name)                                                                                                \
    DetourDetach(reinterpret_cast<PVOID*>(&original##name), reinterpret_cast<PVOID>(Hook##name))
    DETACH(CreateList);
    DETACH(CreateList1);
    DETACH(View);
    DETACH(Copy);
    DETACH(CopySimple);
    DETACH(Bind);
    DETACH(Draw);
    DETACH(Indexed);
    DETACH(Clear);
    DETACH(Barriers);
    DETACH(Reset);
    DETACH(Close);
    DETACH(Bundle);
    DETACH(Indirect);
    DETACH(BeginPass);
    DETACH(EndPass);
    DETACH(Mesh);
    DETACH(Enhanced);
#undef DETACH
    if (DetourTransactionCommit() == NO_ERROR)
        s->installed = false;
}
} // namespace DlssNr::PresentDepthDx12
