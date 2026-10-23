// The production exposure scan (retoque/dlssnr/DlssNr_ExposureScan.cpp) against fake D3D12 objects and the
// production submission model. docs/dlssnr/design/exposure-scan-capacity.md, section 8.
//
// The fakes keep every object they made until the case ends, destroyed or not, so a use after destruction is a
// recorded violation instead of undefined behaviour. A placed resource does NOT keep its heap alive here: the
// worst case the design has to survive. A fake list records copies; executing it writes each source's bytes into
// the readback memory at once and notifies the submission model, and the case decides when, and whether, the
// fence completes. One case per process: the scan's state is global.

#include "pch.h"

#include "retoque/dlssnr/DlssNr_ExposureScan.h"
#include "retoque/dlssnr/DlssNr_Submission.h"

#include <Config.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <new>
#include <string>
#include <vector>

namespace
{
using Model = DlssNr::Submission::Detail::Model;
namespace Scan = DlssNr::ExposureScan;

Model g_model;
std::array<uint64_t, DlssNr::Submission::Detail::MaxQueues> g_completed {};
std::array<uint64_t, DlssNr::Submission::Detail::MaxQueues> g_signalled {};
uint64_t g_clock = 0;
bool g_failTrack = false;
bool g_removed = false; // the device reports itself removed
auto completedValue = [](size_t q) { return g_completed[q]; };

std::vector<std::string> g_violations;
std::vector<std::string> g_failures;
unsigned g_copiesRecorded = 0;

void Violation(const std::string& what) { g_violations.push_back(what); }

void Expect(bool condition, const std::string& what)
{
    if (!condition)
        g_failures.push_back(what);
}

struct Heap final : ID3D12Heap
{
    std::string name;
    ULONG refs = 1; // the game's
    bool destroyed = false;

    ULONG AddRef() override
    {
        if (destroyed)
            Violation(name + ": AddRef on a destroyed heap");
        return ++refs;
    }
    ULONG Release() override
    {
        if (destroyed || refs == 0)
        {
            Violation(name + ": Release on a destroyed heap");
            return 0;
        }
        if (--refs == 0)
            destroyed = true;
        return refs;
    }
};

struct Resource final : ID3D12Resource
{
    std::string name;
    ULONG refs = 1;     // every reference, the game's included
    ULONG gameRefs = 1; // the game's
    bool destroyed = false;
    D3D12_RESOURCE_DESC desc {};
    Heap* heap = nullptr; // placed in it; the fake runtime holds no reference on it
    unsigned char contents[16] {};
    std::vector<unsigned char> memory; // a readback buffer's

    ULONG AddRef() override
    {
        if (destroyed)
            Violation(name + ": AddRef on a destroyed resource");
        return ++refs;
    }
    ULONG Release() override
    {
        if (destroyed || refs == 0)
        {
            Violation(name + ": Release on a destroyed resource");
            return 0;
        }
        if (--refs == 0)
        {
            destroyed = true;
            if (heap != nullptr && heap->destroyed)
                Violation(name + ": destroyed after its heap");
        }
        return refs;
    }
    HRESULT GetDevice(int, void** out) override;
    D3D12_RESOURCE_DESC GetDesc() override { return desc; }
    HRESULT Map(UINT, const D3D12_RANGE*, void** out) override
    {
        if (memory.empty() || destroyed)
            return E_FAIL;
        *out = memory.data();
        return S_OK;
    }
    void Unmap(UINT, const D3D12_RANGE*) override {}

    // References the scan holds: everything that is not the game's.
    unsigned ScanRefs() const { return destroyed ? 0 : (unsigned) (refs - gameRefs); }
    void SetFloat(float v) { std::memcpy(contents, &v, sizeof(v)); }
    void SetU32(uint32_t v) { std::memcpy(contents, &v, sizeof(v)); }
};

std::deque<Resource>& Pool()
{
    static std::deque<Resource> pool;
    return pool;
}

std::deque<Heap>& Heaps()
{
    static std::deque<Heap> heaps;
    return heaps;
}

struct Device final : ID3D12Device
{
    ULONG AddRef() override { return 1; }
    ULONG Release() override { return 1; }
    HRESULT GetDeviceRemovedReason() override { return g_removed ? E_FAIL : S_OK; }
    HRESULT CreateCommittedResource(const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC* d,
                                    D3D12_RESOURCE_STATES, const void*, int, void** out) override
    {
        Resource& r = Pool().emplace_back();
        r.name = "readback";
        r.desc = *d;
        r.gameRefs = 0; // the scan's own
        r.memory.assign((size_t) d->Width, 0);
        *out = static_cast<ID3D12Resource*>(&r);
        return S_OK;
    }
};

struct Copy
{
    Resource* src;
    Resource* dst;
    uint64_t offset;
    unsigned bytes;
};

struct List final : ID3D12GraphicsCommandList
{
    std::vector<Copy> copies;

    ULONG AddRef() override { return 1; }
    ULONG Release() override { return 1; }
    D3D12_COMMAND_LIST_TYPE GetType() override { return D3D12_COMMAND_LIST_TYPE_DIRECT; }

    static void Check(ID3D12Resource* resource, const char* what)
    {
        auto* r = static_cast<Resource*>(resource);
        if (r->destroyed)
            Violation(r->name + ": " + what + " recorded on a destroyed resource");
        else if (r->heap != nullptr && r->heap->destroyed)
            Violation(r->name + ": " + what + " recorded over a freed heap");
        else if (r->memory.empty() && r->gameRefs == 0)
            Violation(r->name + ": " + what + " recorded after the game let it go");
    }
    void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers) override
    {
        for (UINT i = 0; i < count; ++i)
            Check(barriers[i].Transition.pResource, "a barrier");
    }
    void CopyBufferRegion(ID3D12Resource* dst, UINT64 offset, ID3D12Resource* src, UINT64, UINT64 bytes) override
    {
        Check(src, "a copy");
        ++g_copiesRecorded;
        copies.push_back({ static_cast<Resource*>(src), static_cast<Resource*>(dst), offset, (unsigned) bytes });
    }
    void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst, UINT, UINT, UINT,
                           const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX*) override
    {
        Check(src->pResource, "a copy");
        ++g_copiesRecorded;
        copies.push_back({ static_cast<Resource*>(src->pResource), static_cast<Resource*>(dst->pResource),
                           dst->PlacedFootprint.Offset, 4 });
    }
};

Device g_device;

HRESULT Resource::GetDevice(int, void** out)
{
    *out = static_cast<ID3D12Device*>(&g_device);
    return S_OK;
}

uintptr_t Key(List& list) { return reinterpret_cast<uintptr_t>(static_cast<ID3D12GraphicsCommandList*>(&list)); }

// The GPU runs the list at once; whether anyone is told depends on the fence.
void Execute(List& list, size_t queue = 0, bool signalled = true)
{
    for (const Copy& c : list.copies)
    {
        if (c.src->destroyed)
            Violation(c.src->name + ": the GPU read it after it was destroyed");
        else if (c.src->heap != nullptr && c.src->heap->destroyed)
            Violation(c.src->name + ": the GPU read it over a freed heap");
        else if (!c.dst->destroyed && c.offset + c.bytes <= c.dst->memory.size())
            std::memcpy(c.dst->memory.data() + c.offset, c.src->contents, std::min<size_t>(c.bytes, 16));
    }
    list.copies.clear();

    auto epoch = g_model.BeforeExecute(Key(list));
    Model::AfterExecute(epoch, queue, ++g_signalled[queue], signalled);
}

void Complete(size_t queue = 0) { g_completed[queue] = g_signalled[queue]; }

// Reset without execution: those commands can never run.
void Discard(List& list)
{
    list.copies.clear();
    Model::AfterReset(g_model.Current(Key(list)), true);
}

void Reset(List& list) { Model::AfterReset(g_model.Current(Key(list)), true); }

void Advance() { g_model.Advance(++g_clock); }

List g_list;
int g_frame = 0;

// One frame of the pass, its list run, its fence answered, the list reset.
void Frame()
{
    Scan::Tick(&g_device, &g_list);
    Execute(g_list);
    Complete();
    Reset(g_list);
    Advance();
    ++g_frame;
}

D3D12_RESOURCE_DESC BufferDesc(unsigned bytes = 4)
{
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return d;
}

Resource* Make(const std::string& name, float value, D3D12_RESOURCE_DESC desc = BufferDesc())
{
    Resource& r = Pool().emplace_back();
    r.name = name;
    r.desc = desc;
    r.SetFloat(value);
    return &r;
}

// The creation hook's call. Placed resources pass their heap where the scan takes one (the 2026-10-23 change);
// before it, the scan took two arguments for both hooks.
template <class... A> void NoteCreated(const D3D12_RESOURCE_DESC* desc, Resource* r, A... heap)
{
    if constexpr (requires { Scan::NoteResource(desc, static_cast<ID3D12Resource*>(r), heap...); })
        Scan::NoteResource(desc, static_cast<ID3D12Resource*>(r), heap...);
    else
        Scan::NoteResource(desc, static_cast<ID3D12Resource*>(r));
}

Resource* Create(const std::string& name, float value, D3D12_RESOURCE_DESC desc = BufferDesc())
{
    Resource* r = Make(name, value, desc);
    NoteCreated(&r->desc, r);
    return r;
}

Resource* CreatePlaced(const std::string& name, float value, Heap* heap)
{
    Resource* r = Make(name, value);
    r->heap = heap;
    NoteCreated(&r->desc, r, static_cast<ID3D12Heap*>(heap));
    return r;
}

void GameRelease(Resource* r)
{
    r->gameRefs--;
    r->Release();
}

// An exposure: swings by an order of magnitude between bright and dark, around `centre`.
float Exposure(float centre, int frame) { return centre * std::pow(10.0f, 0.5f * std::sin(frame / 9.0f)); }

struct Found
{
    float value = 0, low = 0, high = 0;
    int which = 0;
};

Found Best()
{
    Found f;
    f.value = Scan::BestValue(&f.which, &f.low, &f.high);
    return f;
}

// The best value's range lies within what `centre`'s exposure can take, and spans a real swing.
bool FoundExposure(const Found& f, float centre)
{
    return f.value > 0 && f.low >= centre * 0.3f && f.high <= centre * 3.3f && f.high > f.low * 2.0f;
}

const Scan::Candidate* ReportFor(const std::vector<Scan::Candidate>& report, float low, float high)
{
    for (const auto& c : report)
        if (c.reads > 0 && std::fabs(c.lowest - low) < 1e-6f && std::fabs(c.highest - high) < 1e-6f)
            return &c;
    return nullptr;
}

void Begin()
{
    Config::Instance()->DlssNrEnabled = true;
    Config::Instance()->DlssNrWhitePointSource = 2;
    Scan::SetResourceHooksAttached(true);
}

// ---- cases ----------------------------------------------------------------------------------------------------

// Section 4: 64 zero buffers fill the list at load; the exposure is created after them.
void Late()
{
    Begin();
    std::vector<Resource*> zeros;
    for (int i = 0; i < 64; ++i)
        zeros.push_back(Create("zero" + std::to_string(i), 0.0f));
    Resource* exposure = Create("exposure", Exposure(0.5f, 0));
    Expect(exposure->ScanRefs() == 1, "the exposure created after a full list is held (waiting), not refused");

    for (int f = 0; f < 400; ++f)
    {
        exposure->SetFloat(Exposure(0.5f, f));
        Frame();
    }

    const Found f = Best();
    Expect(Scan::Where() == Scan::Verdict::Found, "the late exposure is found");
    Expect(FoundExposure(f, 0.5f), "the value found is the late exposure's, not a zero buffer's");
    unsigned released = 0;
    for (Resource* z : zeros)
        released += z->ScanRefs() == 0;
    Expect(released > 0, "zero buffers gave way to it (retired as false while something waited)");
}

// Section 5: Cyberpunk's order. A new set is created while the list is full, then the feature is released, then
// the game lets the old set go.
void Recreate()
{
    Begin();
    std::vector<Resource*> old;
    for (int i = 0; i < 59; ++i)
        old.push_back(Create("old" + std::to_string(i), 1.0f + 0.01f * i));
    Resource* oldExposure = Create("old exposure", Exposure(0.5f, 0));
    old.push_back(oldExposure);
    for (int f = 0; f < 120; ++f)
    {
        oldExposure->SetFloat(Exposure(0.5f, f));
        Frame();
    }
    Expect(FoundExposure(Best(), 0.5f), "the old exposure is found before the recreation");

    std::vector<Resource*> fresh;
    for (int i = 0; i < 59; ++i)
        fresh.push_back(Create("new" + std::to_string(i), 2.0f + 0.01f * i));
    Resource* exposure = Create("new exposure", Exposure(20.0f, 0));
    fresh.push_back(exposure);

    Scan::ReleaseTrackedResources(); // the top of NVSDK_NGX_D3D12_ReleaseFeature
    for (Resource* r : old)
        GameRelease(r);

    for (int f = 0; f < 300; ++f)
    {
        exposure->SetFloat(Exposure(20.0f, f));
        Frame();
    }

    Expect(FoundExposure(Best(), 20.0f), "the exposure re-created before the release is found after it");
    unsigned alive = 0;
    for (Resource* r : old)
        alive += !r->destroyed;
    Expect(alive == 0,
           "every buffer the game let go of was released by the scan (" + std::to_string(alive) + " still alive)");
    Expect(exposure->ScanRefs() == 1, "the new exposure is still held");
}

// Section 5: a game that keeps its exposure across a feature release keeps it in the scan.
void LongLived()
{
    Begin();
    Resource* exposure = Create("exposure", Exposure(0.5f, 0));
    for (int i = 0; i < 10; ++i)
        Create("quiet" + std::to_string(i), 1.0f + 0.01f * i);
    for (int f = 0; f < 120; ++f)
    {
        exposure->SetFloat(Exposure(0.5f, f));
        Frame();
    }
    const Found before = Best();
    Expect(FoundExposure(before, 0.5f), "found before the release");
    const auto reportBefore = Scan::Report();
    const Scan::Candidate* c = ReportFor(reportBefore, before.low, before.high);
    const unsigned readsBefore = c != nullptr ? c->reads : 0;

    Scan::ReleaseTrackedResources();
    Expect(exposure->ScanRefs() == 1, "a committed exposure the game still holds is kept across the release");

    for (int f = 120; f < 240; ++f)
    {
        exposure->SetFloat(Exposure(0.5f, f));
        Frame();
    }
    const Found after = Best();
    Expect(FoundExposure(after, 0.5f), "still found after the release");
    const auto reportAfter = Scan::Report();
    c = ReportFor(reportAfter, after.low, after.high);
    Expect(c != nullptr && c->reads > readsBefore + 20, "its readings carried on across the release");
}

// Section 3.4: a newcomer is never credited with a value its own buffer did not hold.
void Attribution()
{
    Begin();
    Resource* a = Create("a", 8.0f);
    for (int f = 0; f < 20; ++f)
        Frame();
    Scan::ReleaseTrackedResources();
    Resource* b = Create("b", 1.0f);
    for (int f = 0; f < 20; ++f)
        Frame();
    Expect(Scan::Where() != Scan::Verdict::Found, "two constant buffers are not a find");
    Expect(Scan::BestValue() == 0.0f, "nothing usable");
    bool bClean = false;
    for (const auto& c : Scan::Report())
        if (c.reads > 0 && c.latest == 1.0f)
            bClean = c.lowest == 1.0f && c.highest == 1.0f;
    Expect(bClean, "b's range holds only b's value");

    // A late completion: a copy of `late` is recorded, its fence held on a second queue, the game lets `late` go
    // and a newcomer takes its position while the copy is unconfirmed.
    Resource* late = Create("late", 6.0f);
    for (int f = 0; f < 5; ++f)
        Frame();
    Scan::Tick(&g_device, &g_list);
    Execute(g_list, 1); // queue 1, not completed
    Reset(g_list);
    Advance();
    GameRelease(late);
    Resource* newcomer = Create("newcomer", 1.5f);
    for (int f = 0; f < 6; ++f)
        Frame();
    Expect(!late->destroyed && late->ScanRefs() == 1, "a candidate with an unconfirmed copy is not released");
    Complete(1);
    for (int f = 0; f < 6; ++f)
        Frame();
    Expect(late->destroyed, "released once its copy is confirmed");
    bool newcomerClean = false;
    for (const auto& c : Scan::Report())
        if (c.reads > 0 && c.latest == 1.5f)
            newcomerClean = c.lowest == 1.5f && c.highest == 1.5f;
    Expect(newcomerClean, "the newcomer is not credited with the late copy's value");
    (void) a;
    (void) b;
    (void) newcomer;
}

// Section 4: an integer buffer (65536 reads as 9.18e-41) gives way before plausible quiet ones.
void Integer()
{
    Begin();
    std::vector<Resource*> quiet;
    for (int i = 0; i < 63; ++i)
        quiet.push_back(Create("quiet" + std::to_string(i), 1.0f + 0.01f * i));
    Resource* integer = Make("integer", 0.0f);
    integer->SetU32(65536);
    NoteCreated(&integer->desc, integer);
    Resource* exposure = Create("exposure", Exposure(0.5f, 0));
    for (int f = 0; f < 200; ++f)
    {
        exposure->SetFloat(Exposure(0.5f, f));
        Frame();
    }
    Expect(FoundExposure(Best(), 0.5f), "the exposure waiting behind a full list is found");
    Expect(integer->ScanRefs() == 0, "the integer buffer gave way to it");
    unsigned held = 0;
    for (Resource* r : quiet)
        held += r->ScanRefs() == 1;
    Expect(held == quiet.size(), "plausible quiet buffers are kept (" + std::to_string(held) + " of 63)");
}

// Section 4: 64 plausible quiet buffers never give way as false; once watched past patience, one is moved aside for
// the untried exposure waiting behind them, and comes back to the waiting list with its reference.
void Quiet()
{
    Begin();
    std::vector<Resource*> quiet;
    for (int i = 0; i < 64; ++i)
        quiet.push_back(Create("quiet" + std::to_string(i), 1.0f + 0.01f * i));
    Resource* exposure = Create("exposure", Exposure(0.5f, 0));
    for (int f = 0; f < 1700; ++f)
    {
        exposure->SetFloat(Exposure(0.5f, f));
        Frame();
    }
    Expect(Best().value == 0.0f, "nothing found while the exposure waits behind plausible quiet buffers");
    Expect(Scan::Where() == Scan::Verdict::Watching, "watching, not barren, while something untried waits");
    for (int f = 1700; f < 2000; ++f)
    {
        exposure->SetFloat(Exposure(0.5f, f));
        Frame();
    }
    Expect(FoundExposure(Best(), 0.5f), "found once a quiet buffer past patience made way for it");
    unsigned held = 0;
    for (Resource* r : quiet)
        held += r->ScanRefs() == 1;
    Expect(held == quiet.size(),
           "the quiet buffer moved aside is still held, waiting (" + std::to_string(held) + " of 64)");
}

// Section 4: a full waiting list gives up its oldest never-copied candidate for a newcomer.
void Overflow()
{
    Begin();
    std::vector<Resource*> made;
    for (int i = 0; i < 64 + 192; ++i)
        made.push_back(Create("r" + std::to_string(i), 1.0f));
    unsigned held = 0;
    for (Resource* r : made)
        held += r->ScanRefs();
    Expect(held == 64 + 192, "64 read and 192 waiting are held (" + std::to_string(held) + ")");
    Resource* newcomer = Create("newcomer", 1.0f);
    Expect(newcomer->ScanRefs() == 1, "the newcomer is held");
    Expect(made[64]->ScanRefs() == 0, "the oldest waiting one gave way");
    Expect(made[65]->ScanRefs() == 1 && made[63]->ScanRefs() == 1, "and only that one");
    Expect(!made[64]->destroyed, "a release of a resource the game still holds destroys nothing");
}

// Section 3.4: readings need a completed recording.
void FenceCase(const char* how)
{
    Begin();
    Resource* c = Create("c", Exposure(0.5f, 0));
    for (int f = 0; f < 30; ++f)
    {
        c->SetFloat(Exposure(0.5f, f));
        Scan::Tick(&g_device, &g_list);
        if (std::strcmp(how, "never") == 0)
            Execute(g_list); // never completed
        else if (std::strcmp(how, "signal") == 0)
        {
            Execute(g_list, 0, false); // the Signal failed
            Complete();
        }
        else if (std::strcmp(how, "removed") == 0)
        {
            Execute(g_list);
            g_completed[0] = std::numeric_limits<uint64_t>::max(); // GetCompletedValue on a removed device
            g_removed = true;
        }
        Reset(g_list);
        Advance();
    }
    unsigned reads = 0;
    for (const auto& r : Scan::Report())
        reads += r.reads;
    Expect(reads == 0, std::string("no reading without a completed recording (") + std::to_string(reads) + ")");
    Expect(Scan::Where() != Scan::Verdict::Found, "nothing found from unconfirmed readbacks");
    Expect(g_copiesRecorded <= 4, "no readback slot is rewritten while its recording is unconfirmed (" +
                                      std::to_string(g_copiesRecorded) + " copies)");

    GameRelease(c);
    for (int f = 0; f < 10; ++f)
    {
        Scan::Tick(&g_device, &g_list);
        Reset(g_list);
        Advance();
    }
    if (g_removed)
        Expect(c->destroyed, "on a removed device nothing can run again: let go and released, not held for good");
    else
        Expect(!c->destroyed && c->ScanRefs() == 1, "a candidate whose copies are unconfirmed is retained");
}

// A list the game resets without executing: its copies never run; the slot is reused without being read.
void Discarded()
{
    Begin();
    Resource* c = Create("c", 2.0f);
    for (int f = 0; f < 12; ++f)
    {
        Scan::Tick(&g_device, &g_list);
        Discard(g_list);
        Advance();
    }
    unsigned reads = 0;
    for (const auto& r : Scan::Report())
        reads += r.reads;
    Expect(reads == 0, "nothing read from discarded recordings");
    Expect(g_copiesRecorded > 4, "the sweep goes on: a discarded recording frees its slot");
    for (int f = 0; f < 10; ++f)
        Frame();
    for (const auto& r : Scan::Report())
        reads += r.reads;
    Expect(reads > 0, "readings resume once lists run");
    (void) c;
}

// Sections 3.3 and 3.1: freed, released after settlement, and a new resource at the same address.
void Reuse()
{
    Begin();
    Resource* a = Create("a", 3.0f);
    for (int f = 0; f < 5; ++f)
        Frame();
    Scan::Tick(&g_device, &g_list);
    Execute(g_list, 1); // held on queue 1
    Reset(g_list);
    Advance();
    GameRelease(a);
    for (int f = 0; f < 5; ++f)
        Frame();
    Expect(!a->destroyed, "not destroyed while the scan's copy of it is unconfirmed");
    Complete(1);
    for (int f = 0; f < 3; ++f)
        Frame();
    Expect(a->destroyed, "destroyed once the game let go and the copy was confirmed");
    if (!a->destroyed)
        return;

    // The allocator hands the address back.
    Resource* b = a;
    b->~Resource();
    new (b) Resource();
    b->name = "b";
    b->desc = BufferDesc();
    b->SetFloat(Exposure(5.0f, 0));
    NoteCreated(&b->desc, b);
    Expect(b->ScanRefs() == 1, "a new resource at a released address is adopted");
    for (int f = 0; f < 100; ++f)
    {
        b->SetFloat(Exposure(5.0f, f));
        Frame();
    }
    Expect(FoundExposure(Best(), 5.0f), "found, with only its own values");
}

// Section 3.1: a placed candidate whose heap the game frees right after the resource.
void Placed()
{
    Begin();
    Heap& heap = Heaps().emplace_back();
    heap.name = "heap";
    Resource* p = CreatePlaced("placed", Exposure(0.5f, 0), &heap);
    for (int f = 0; f < 60; ++f)
    {
        p->SetFloat(Exposure(0.5f, f));
        Frame();
    }
    Expect(FoundExposure(Best(), 0.5f), "a placed exposure is found");
    GameRelease(p);
    heap.Release(); // the game's heap reference
    for (int f = 0; f < 30; ++f)
        Frame();
    Scan::ReleaseTrackedResources();
    for (int f = 0; f < 5; ++f)
        Frame();
    Expect(p->destroyed && heap.destroyed, "the scan let go of both");
}

// Section 5: view-only candidates keep the wipe; committed ones are kept.
void View()
{
    Begin();
    Resource* v = Make("view", 1.0f);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    Scan::NoteUav(v, &uav);
    Resource* c = Create("committed", 2.0f);
    for (int f = 0; f < 10; ++f)
        Frame();
    Expect(v->ScanRefs() == 1, "a view candidate is held while watched");
    Scan::ReleaseTrackedResources();
    Expect(v->ScanRefs() == 0, "a view candidate is released at the boundary, as before");
    Expect(c->ScanRefs() == 1, "a committed candidate the game holds is kept at the boundary");
}

// Section 5: a candidate on a removed device is released at the boundary whatever its origin; one on a live device
// is kept.
void RemovedAtBoundary()
{
    Begin();
    Resource* c = Create("committed", 2.0f);
    for (int f = 0; f < 10; ++f)
        Frame();
    Scan::ReleaseTrackedResources();
    Expect(c->ScanRefs() == 1, "kept at a boundary on a live device");
    g_removed = true;
    Scan::ReleaseTrackedResources();
    Expect(c->ScanRefs() == 0, "released at a boundary once its device was removed");
}

// A game that records each frame into a new list and never resets the old ones: completed recordings are never
// sealed, so a candidate's usage must still let go of the finished ones or it fills and the candidate stops being
// copied.
void FreshLists()
{
    Begin();
    Resource* exposure = Create("exposure", Exposure(0.5f, 0));
    std::deque<List> lists;
    for (int f = 0; f < 300; ++f)
    {
        exposure->SetFloat(Exposure(0.5f, f));
        List& list = lists.emplace_back();
        const unsigned before = g_copiesRecorded;
        Scan::Tick(&g_device, &list);
        if (f >= 200)
            Expect(g_copiesRecorded > before || f % 4 != 0, "still copied after " + std::to_string(f) + " frames");
        Execute(list);
        Complete();
        Advance();
    }
    Expect(FoundExposure(Best(), 0.5f), "found on lists that are never reset");
}

// Section 6: inert when NR is off or the scan is not wanted; nothing copied on an untracked list.
void Inert(const char* how)
{
    Begin();
    if (std::strcmp(how, "off") == 0)
        Config::Instance()->DlssNrEnabled = false;
    else if (std::strcmp(how, "unwanted") == 0)
        Config::Instance()->DlssNrWhitePointSource = 0;
    else if (std::strcmp(how, "untracked") == 0)
        g_failTrack = true;

    std::vector<Resource*> made;
    for (int i = 0; i < 5; ++i)
        made.push_back(Create("r" + std::to_string(i), Exposure(0.5f, 0)));
    Resource* v = Make("view", 1.0f);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    Scan::NoteUav(v, &uav);
    for (int f = 0; f < 10; ++f)
    {
        for (Resource* r : made)
            r->SetFloat(Exposure(0.5f, f));
        Frame();
    }

    if (std::strcmp(how, "untracked") == 0)
    {
        Expect(g_copiesRecorded == 0,
               "nothing copied on a list the tracker refuses (" + std::to_string(g_copiesRecorded) + ")");
        const char* status = Scan::Status();
        Expect(status != nullptr && std::strstr(status, "track") != nullptr, "the status says why");
        return;
    }

    unsigned held = v->ScanRefs();
    for (Resource* r : made)
        held += r->ScanRefs();
    Expect(held == 0, "no reference taken");
    Expect(g_copiesRecorded == 0, "nothing copied");
    if (std::strcmp(how, "off") == 0)
        Expect(Scan::Examined() == 0, "nothing examined with NR off");
}

} // namespace

namespace DlssNr::Submission
{
bool Track(ID3D12GraphicsCommandList* list, Usage& usage, bool)
{
    if (g_failTrack)
        return false;
    return bool(g_model.Track(reinterpret_cast<uintptr_t>(list), usage, completedValue));
}

bool Completed(const Usage& usage)
{
    bool any = false;
    for (const auto& e : usage.epochs)
    {
        if (!e)
            continue;
        any = true;
        if (!Detail::IsComplete(*e, completedValue))
            return false;
    }
    return any;
}

bool Settled(const Usage& usage) { return Detail::IsSettled(usage, completedValue, g_model.Clock(), g_model.Strict()); }

void PruneFinished(Usage& usage) { Detail::PruneLifetime(usage, completedValue, g_model.Clock(), g_model.Strict()); }

const char* LastRefusal() { return "injected-refusal"; }
} // namespace DlssNr::Submission

int main(int argc, char** argv)
{
    const std::map<std::string, std::function<void()>> cases {
        { "late", Late },
        { "recreate", Recreate },
        { "long-lived", LongLived },
        { "attribution", Attribution },
        { "integer", Integer },
        { "quiet", Quiet },
        { "overflow", Overflow },
        { "fence-never", [] { FenceCase("never"); } },
        { "fence-signal", [] { FenceCase("signal"); } },
        { "fence-removed", [] { FenceCase("removed"); } },
        { "discarded", Discarded },
        { "reuse", Reuse },
        { "placed", Placed },
        { "view", View },
        { "removed-boundary", RemovedAtBoundary },
        { "fresh-lists", FreshLists },
        { "inert-off", [] { Inert("off"); } },
        { "inert-unwanted", [] { Inert("unwanted"); } },
        { "untracked", [] { Inert("untracked"); } },
    };

    if (argc == 2 && std::strcmp(argv[1], "--list") == 0)
    {
        for (const auto& [name, run] : cases)
            std::printf("%s\n", name.c_str());
        return 0;
    }

    const auto it = argc == 2 ? cases.find(argv[1]) : cases.end();
    if (it == cases.end())
    {
        std::fprintf(stderr, "usage: cases <case> | --list\n");
        return 2;
    }

    it->second();

    for (const auto& v : g_violations)
        g_failures.push_back("violation: " + v);

    if (!g_failures.empty())
    {
        std::printf("FAIL %s\n", it->first.c_str());
        std::vector<std::string> seen;
        for (const auto& f : g_failures)
        {
            if (std::find(seen.begin(), seen.end(), f) != seen.end())
                continue;
            seen.push_back(f);
            std::printf("  %s\n", f.c_str());
            if (seen.size() >= 12)
            {
                std::printf("  ...\n");
                break;
            }
        }
        return 1;
    }

    std::printf("PASS %s\n", it->first.c_str());
    return 0;
}
