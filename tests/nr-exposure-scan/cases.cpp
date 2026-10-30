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
unsigned long long g_listProbes = 0; // epochs visited by tracker calls that refresh orphan flags (cost, E-01 review)

size_t Epochs(const DlssNr::Submission::Usage& usage)
{
    size_t n = 0;
    for (const auto& e : usage.epochs)
        n += e != nullptr;
    return n;
}
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

// An execution the GPU has been handed: the list's commands as they were when it was executed, run when its fence
// completes. A list keeps its commands until Reset, so executing it again runs them again (a replay).
struct Pending
{
    std::vector<Copy> copies;
    size_t queue;
    uint64_t fence;
};

std::vector<Pending>& InFlight()
{
    static std::vector<Pending> pending;
    return pending;
}

void RunCopies(const std::vector<Copy>& copies)
{
    for (const Copy& c : copies)
    {
        if (c.src->destroyed)
            Violation(c.src->name + ": the GPU read it after it was destroyed");
        else if (c.src->heap != nullptr && c.src->heap->destroyed)
            Violation(c.src->name + ": the GPU read it over a freed heap");
        else if (!c.dst->destroyed && c.offset + c.bytes <= c.dst->memory.size())
            std::memcpy(c.dst->memory.data() + c.offset, c.src->contents, std::min<size_t>(c.bytes, 16));
    }
}

void Execute(List& list, size_t queue = 0, bool signalled = true)
{
    auto epoch = g_model.BeforeExecute(Key(list));
    Model::AfterExecute(epoch, queue, ++g_signalled[queue], signalled);
    InFlight().push_back({ list.copies, queue, g_signalled[queue] });
}

// The GPU reaches the queue's last signalled value: everything handed to it up to there runs, in order.
void Complete(size_t queue = 0)
{
    g_completed[queue] = g_signalled[queue];
    auto& pending = InFlight();
    for (auto it = pending.begin(); it != pending.end();)
    {
        if (it->queue != queue || it->fence > g_completed[queue])
        {
            ++it;
            continue;
        }
        RunCopies(it->copies);
        it = pending.erase(it);
    }
}

// Reset without execution: those commands can never run.
void Discard(List& list)
{
    list.copies.clear();
    Model::AfterReset(g_model.Current(Key(list)), true);
}

void Reset(List& list)
{
    list.copies.clear();
    Model::AfterReset(g_model.Current(Key(list)), true);
}

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
// sealed. A readback slot is reused only once its recording settles (complete and aged out of the tracker's replay
// window, review E-04), so the sweep slows to a few per window -- but it keeps going, a candidate's usage never fills,
// and the exposure is found.
void FreshLists()
{
    Begin();
    Resource* exposure = Create("exposure", Exposure(0.5f, 0));
    std::deque<List> lists;
    unsigned perWindow[6] = {};
    for (int f = 0; f < 6 * 128; ++f)
    {
        exposure->SetFloat(Exposure(0.5f, f));
        List& list = lists.emplace_back();
        const unsigned before = g_copiesRecorded;
        Scan::Tick(&g_device, &list);
        perWindow[f / 128] += g_copiesRecorded - before;
        Execute(list);
        Complete();
        Advance();
    }
    for (int w = 1; w < 6; ++w)
        Expect(perWindow[w] > 0, "still copied in window " + std::to_string(w) + " of 128 frames");
    std::printf("  fresh lists: copies per 128 frames %u %u %u %u %u %u\n", perWindow[0], perWindow[1], perWindow[2],
                perWindow[3], perWindow[4], perWindow[5]);
    Expect(FoundExposure(Best(), 0.5f), "found on lists that are never reset");
}

// Review E-01: a candidate's usage must not forget a recording that can still run. A quiet candidate (discovery: a
// sweep every frame) is copied into 58 lists the game keeps and never resets; the game then executes the first one
// again, resets the rest and lets the candidate go. Its release has to wait for that replay.
void ReplayAfterPrune()
{
    Begin();
    Resource* q = Create("quiet", 1.0f);
    std::deque<List> lists;
    for (int i = 0; i < 58; ++i)
    {
        List& l = lists.emplace_back();
        Scan::Tick(&g_device, &l);
        Execute(l);
        Complete();
        Advance();
    }
    Execute(lists[0]); // a replay, in flight
    for (size_t i = 1; i < lists.size(); ++i)
        Reset(lists[i]);
    GameRelease(q);
    for (int f = 0; f < 3; ++f)
    {
        List& l = lists.emplace_back();
        Scan::Tick(&g_device, &l);
        Discard(l);
        Advance();
    }
    Expect(!q->destroyed, "not released while a list that copied it is executing again");
    Complete(); // the replay runs
    Reset(lists[0]);
    for (int f = 0; f < 3; ++f)
        Frame();
    Expect(q->destroyed, "released once that list was reset");
}

// Review E-01, the model rule on its own: 57 completed recordings of lists that are kept and never reset, inside the
// replay window. PruneFinished (the production template) keeps every one; the admission pruning it replaced cut one.
void PruneModel()
{
    std::deque<List> lists;
    DlssNr::Submission::Usage usage;
    for (int i = 0; i < 57; ++i)
    {
        List& l = lists.emplace_back();
        Expect(DlssNr::Submission::Track(&l, usage), "tracked");
        Execute(l);
        Complete();
        Advance();
    }
    DlssNr::Submission::Usage cut = usage;
    DlssNr::Submission::Detail::PruneFinished(usage, completedValue, g_model.Clock(), g_model.Strict());
    Expect(Epochs(usage) == 57,
           "PruneFinished keeps completed recordings that may still run (" + std::to_string(Epochs(usage)) + ")");
    DlssNr::Submission::Detail::PruneLifetime(cut, completedValue, g_model.Clock(), g_model.Strict());
    Expect(Epochs(cut) < 57, "the admission pruning would have cut one (the review's E-01)");
    for (int i = 0; i < 130; ++i)
        Advance();
    DlssNr::Submission::Detail::PruneFinished(usage, completedValue, g_model.Clock(), g_model.Strict());
    Expect(Epochs(usage) == 0, "aged out of the replay window, they go");
}

// Review E-04: a readback slot is not rewritten while the recording that last wrote it can still run. `a` copies x
// into a slot and completes but is kept; four sweeps later the ring comes back to that slot. The game then runs `a`
// again after that frame's work, which writes x's value into the slot again.
void ReplaySlot()
{
    Begin();
    Resource* x = Create("x", 6.0f);
    for (int f = 0; f < 8; ++f)
        Frame();
    List a;
    Scan::Tick(&g_device, &a);
    Execute(a);
    Complete();
    Advance();
    GameRelease(x);
    Resource* y = Create("y", 1.5f);
    for (int f = 0; f < 3; ++f)
        Frame();
    Scan::Tick(&g_device, &g_list);
    Execute(g_list);
    Execute(a); // the replay, after this frame's work
    Complete();
    Reset(g_list);
    Advance();
    for (int f = 0; f < 6; ++f)
        Frame();
    bool clean = false;
    for (const auto& c : Scan::Report())
        if (c.reads > 0 && c.latest == 1.5f)
            clean = c.lowest == 1.5f && c.highest == 1.5f;
    Expect(clean, "y is never credited with what a replay of an older recording wrote");
    Expect(!x->destroyed, "x is held while a list that copied it can still run");
    Reset(a);
    for (int f = 0; f < 3; ++f)
        Frame();
    Expect(x->destroyed, "and released once that list is reset");
    (void) y;
}

// Review E-02: turning the scan off lets go of what it holds, whatever is released later and whether or not a
// frame of the pass runs. Off, on, off again.
void OffAfterOn()
{
    Begin();
    Resource* copied = Create("copied", 1.0f);
    std::vector<Resource*> fill;
    for (int i = 0; i < 63; ++i)
        fill.push_back(Create("fill" + std::to_string(i), 1.0f + 0.01f * i));
    Resource* waiting = Create("waiting", 2.0f); // the 65th: never copied
    for (int f = 0; f < 5; ++f)
        Frame();
    Expect(copied->ScanRefs() == 1 && waiting->ScanRefs() == 1, "held while wanted");

    Config::Instance()->DlssNrWhitePointSource = 0;
    for (int f = 0; f < 3; ++f)
        Frame();
    Expect(waiting->ScanRefs() == 0 && copied->ScanRefs() == 0,
           "frames of the pass with the scan off let go of what it held, before any feature release");
    Scan::ReleaseTrackedResources();
    GameRelease(copied);
    for (int f = 0; f < 3; ++f)
        Frame();
    Expect(copied->destroyed, "a copied candidate is released once the scan is off and its copies settled");
    Expect(waiting->ScanRefs() == 0, "a waiting candidate is released once the scan is off");
    unsigned held = 0;
    for (Resource* r : fill)
        held += r->ScanRefs();
    Expect(held == 0, "nothing is held while the scan is off (" + std::to_string(held) + ")");

    Config::Instance()->DlssNrWhitePointSource = 2;
    Resource* again = Create("again", 3.0f);
    for (int f = 0; f < 3; ++f)
        Frame();
    Expect(again->ScanRefs() == 1, "on again: a new candidate is held");

    // Off, with NR itself disabled: no frame of the pass runs, only a feature release.
    Config::Instance()->DlssNrWhitePointSource = 0;
    Config::Instance()->DlssNrEnabled = false;
    Scan::ReleaseTrackedResources();
    Expect(again->ScanRefs() == 0, "off with no frame of the pass: released at the next feature release");
}

// Review E-03: what the scan cannot certify about a placed resource -- aliasing, residency -- is not carried across a
// feature release: placed candidates are released there, as the wipe did; committed ones are kept.
void PlacedAtBoundary()
{
    Begin();
    Heap& heap = Heaps().emplace_back();
    heap.name = "heap";
    Resource* p = CreatePlaced("placed", 1.0f, &heap);
    Resource* c = Create("committed", 2.0f);
    for (int f = 0; f < 5; ++f)
        Frame();
    Scan::ReleaseTrackedResources();
    Expect(p->ScanRefs() == 0 && heap.refs == 1, "a placed candidate and its heap are released at the boundary");
    Expect(c->ScanRefs() == 1, "a committed one is kept");
}

// With the scan wanted but no frame of the pass reaching Tick any more, a committed candidate the game let go of is
// released at the next feature release, not held until a frame that never comes.
void BoundaryProbe()
{
    Begin();
    Resource* gone = Create("gone", 1.0f);
    Resource* kept = Create("kept", 2.0f);
    for (int f = 0; f < 5; ++f)
        Frame();
    GameRelease(gone);
    Scan::ReleaseTrackedResources();
    Expect(gone->destroyed, "let go and settled: released at the boundary without a frame");
    Expect(kept->ScanRefs() == 1, "still held by the game: kept");
}

// Review cost note: how many list probes the tracker calls of one Tick can make, with 64 candidates read every frame
// into lists the game keeps and never resets (the worst case for a usage's size) and into lists it resets.
void Cost()
{
    Begin();
    for (int i = 0; i < 64; ++i)
        Create("quiet" + std::to_string(i), 1.0f + 0.01f * i);
    std::deque<List> lists;
    unsigned long long worst = 0;
    for (int f = 0; f < 200; ++f)
    {
        List& l = lists.emplace_back();
        const unsigned long long before = g_listProbes;
        Scan::Tick(&g_device, &l);
        worst = std::max(worst, g_listProbes - before);
        Execute(l);
        Complete();
        Advance();
    }
    unsigned long long steady = 0;
    for (int f = 0; f < 200; ++f)
    {
        const unsigned long long before = g_listProbes;
        Frame();
        steady = std::max(steady, g_listProbes - before);
    }
    std::printf("  cost: at most %llu list probes in one Tick on kept lists, %llu on reset lists (64 candidates)\n",
                worst, steady);
    Expect(worst <= 64 * 6 + 64,
           "list probes per Tick stay within 64 candidates x a few recordings (" + std::to_string(worst) + ")");
    Expect(steady <= 64 * 2 + 16, "and stay small when lists are reset (" + std::to_string(steady) + ")");
}

#ifdef DLSSNR_EXPOSURE_SCAN_COPY_BUDGET
// Review E-05, against a budget of DLSSNR_EXPOSURE_SCAN_COPY_BUDGET (run.py builds this case with 8): held candidates
// that may still have GPU work -- copied and active or waiting, plus retiring -- never exceed it, so the retiring set
// cannot either.
void Budget()
{
    Begin();
    std::vector<Resource*> first;
    for (int i = 0; i < 20; ++i)
        first.push_back(Create("first" + std::to_string(i), 1.0f + 0.01f * i));
    const unsigned before = g_copiesRecorded;
    Scan::Tick(&g_device, &g_list);
    Expect(g_copiesRecorded - before <= 8,
           "no more first copies than the budget (" + std::to_string(g_copiesRecorded - before) + ")");
    Execute(g_list, 1); // held: these copies stay unconfirmed
    Reset(g_list);
    Advance();
    for (Resource* r : first)
        GameRelease(r);
    std::vector<Resource*> second;
    for (int i = 0; i < 20; ++i)
        second.push_back(Create("second" + std::to_string(i), 2.0f + 0.01f * i));
    for (int f = 0; f < 3; ++f)
    {
        const unsigned at = g_copiesRecorded;
        Scan::Tick(&g_device, &g_list);
        Expect(g_copiesRecorded == at, "no first copy while the budget is held by unconfirmed copies");
        Execute(g_list);
        Complete();
        Reset(g_list);
        Advance();
    }
    unsigned held = 0;
    for (Resource* r : first)
        held += r->ScanRefs();
    Expect(held <= 8,
           "let-go candidates retained for unconfirmed copies stay within the budget (" + std::to_string(held) + ")");
    Complete(1);
    for (int f = 0; f < 3; ++f)
        Frame();
    held = 0;
    for (Resource* r : first)
        held += r->ScanRefs();
    Expect(held == 0, "released once confirmed");
    unsigned copiedNow = 0;
    for (Resource* r : second)
        copiedNow += r->ScanRefs();
    Expect(copiedNow == 20, "the second set is held");
    const unsigned at = g_copiesRecorded;
    Frame();
    Expect(g_copiesRecorded > at, "and copied again once the budget is free");
}
#endif

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

// ---- [DlssNr] ScanMatch (docs/dlssnr/design/exposure-scan-match.md)
// --------------------------------------------------
//
// The meter arrives as the pulse pass delivers it in the renderer: one reading a frame, through NoteMeter.
void FeedMeter(float meter) { Scan::NoteMeter(meter); }

std::string MatchLineNow() { return Scan::MatchLine(); }

// The light the player walks through: 6 EV and back every 800 frames, from a dark interior to daylight.
float Light(int frame) { return std::exp2(-2.0f + 3.0f * (1.0f - std::cos(6.2831853f * (float) frame / 800.0f))); }

// One frame of the scene: the meter reads the light with a little noise (as the pulse pass reads the game's frame).
float MeterAt(int frame) { return Light(frame) * std::exp2(0.1f * std::sin((float) frame * 1.7f)); }

struct MatchScene
{
    Resource* timer = nullptr;   // a 1x1 that climbs every frame: x500 travel, the widest by far
    Resource* frozen = nullptr;  // one value for the whole run
    Resource* adapted = nullptr; // the game's exposure, a divisor eased behind the light: x64 travel
    Resource* target = nullptr;  // the game's per-frame target, a multiplier at lag 0 (made only when asked)
    float adaptedLog = 0.0f;
    bool started = false;

    void Make(bool withTarget)
    {
        timer = Create("timer", 0.02f);
        frozen = Create("frozen", 1.0f);
        adapted = Create("adapted", 1.0f);
        if (withTarget)
            target = Create("target", Light(0));
    }

    // Sets this frame's values, feeds the meter and runs the frame.
    void Step(int f, float flash = 1.0f)
    {
        timer->SetFloat(0.02f + 0.004f * (float) f);
        const float light = Light(f);
        adaptedLog = started ? adaptedLog + (std::log2(light) - adaptedLog) * (1.0f - std::exp(-1.0f / 48.0f))
                             : std::log2(light);
        started = true;
        adapted->SetFloat(0.5f / std::exp2(adaptedLog));
        if (target != nullptr)
            target->SetFloat(2.0f * light * flash);
        FeedMeter(MeterAt(f));
        Frame();
    }

    float Adapted() const { return 0.5f / std::exp2(adaptedLog); }
};

float Contents(const Resource* r)
{
    float v = 0.0f;
    std::memcpy(&v, r->contents, sizeof(v));
    return v;
}

// Stops between an offered value and a candidate's current one. The offered value is the last sweep's, read a few
// frames late and, after a mover is found, every fourth frame: close, not equal.
float StopsFrom(float offered, const Resource* r)
{
    return offered > 0.0f && Contents(r) > 0.0f ? std::fabs(std::log2(offered / Contents(r))) : 1e9f;
}

// Key off: the scan's rule is widest travel exactly as before -- the timer is offered, whatever the meter says.
void MatchOff()
{
    Begin();
    MatchScene scene;
    scene.Make(false);
    for (int f = 0; f < 2400; ++f)
        scene.Step(f);

    const Found best = Best();
    Expect(StopsFrom(best.value, scene.timer) < 0.02f && best.high > best.low * 100.0f,
           "key off: widest travel offers the timer (" + std::to_string(best.value) + ")");
    Expect(MatchLineNow().empty(), "key off: no match line");
}

// Key on: nothing is offered until the match is sure, and then it is the adapted exposure, not the timer.
void MatchOn()
{
    Begin();
    Config::Instance()->DlssNrScanMatch = true;
    MatchScene scene;
    scene.Make(false);

    bool offeredEarly = false;
    for (int f = 0; f < 2400; ++f)
    {
        scene.Step(f);
        if (f == 600)
            offeredEarly = Best().value != 0.0f;
    }

    Expect(!offeredEarly, "key on: nothing is offered before the match is sure (no widest-travel fallback)");
    const Found best = Best();
    Expect(StopsFrom(best.value, scene.adapted) < 0.3f && StopsFrom(best.value, scene.timer) > 1.0f,
           "key on: the adapted exposure is offered (" + std::to_string(best.value) + " vs " +
               std::to_string(Contents(scene.adapted)) + ", timer " + std::to_string(Contents(scene.timer)) + ")");
    const std::string line = MatchLineNow();
    Expect(line.find("divisor") != std::string::npos, "the match line names its units: " + line);
    std::printf("  match line: %s\n", line.c_str());
}

// A match at lag 0 (the game's per-frame target) is eased: a 1 EV flash over one sweep moves the offered value a
// little, not a stop. The candidate itself reads the flash.
void MatchEased()
{
    Begin();
    Config::Instance()->DlssNrScanMatch = true;
    MatchScene scene;
    scene.Make(true);
    // The target alone follows the light: the adapted one is left out of this case by holding it frozen.
    for (int f = 0; f < 2400; ++f)
    {
        scene.Step(f);
        scene.adapted->SetFloat(1.0f);
    }

    const float before = Best().value;
    Expect(StopsFrom(before, scene.target) < 0.3f && StopsFrom(before, scene.timer) > 1.0f,
           "the per-frame target is matched and offered (" + std::to_string(before) + " vs " +
               std::to_string(Contents(scene.target)) + ")");
    for (int f = 2400; f < 2404; ++f)
        scene.Step(f, 2.0f); // a pan into a sunlit wall and back: one sweep reads it
    const float during = Best().value;
    for (int f = 2404; f < 2408; ++f)
        scene.Step(f);

    const float moved = std::fabs(std::log2(during / std::max(before, 1e-6f)));
    Expect(moved < 0.25f, "the flash moved the offered value " + std::to_string(moved) + " EV (raw: 1 EV)");
    std::printf("  flash: 1 EV raw, %.3f EV offered\n", moved);
}

// The meter is ignored with the key off, and with it on but the scan unwanted nothing is held or recorded.
void MatchInert()
{
    Begin();
    Config::Instance()->DlssNrScanMatch = true;
    Config::Instance()->DlssNrWhitePointSource = 0;
    MatchScene scene;
    scene.Make(false);
    for (int f = 0; f < 50; ++f)
        scene.Step(f);
    Expect(scene.adapted->ScanRefs() == 0 && g_copiesRecorded == 0, "the scan unwanted: nothing held or copied");
    Expect(!Scan::MeterWanted(), "the meter is not wanted with the scan unwanted");
    Config::Instance()->DlssNrWhitePointSource = 2;
    Expect(Scan::MeterWanted(), "the meter is wanted with the scan the source and the key on");
    Config::Instance()->DlssNrScanMatch = false;
    Expect(!Scan::MeterWanted(), "the meter is not wanted with the key off");
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

// Settled refreshes orphan flags in production: one AddRef/Release pair per held list of each epoch it visits.
bool Settled(const Usage& usage)
{
    g_listProbes += Epochs(usage);
    return Detail::IsSettled(usage, completedValue, g_model.Clock(), g_model.Strict());
}

// Mirrors the production PruneFinished (retoque/dlssnr/DlssNr_Submission.cpp; run.py guards the match): the model's
// own PruneFinished, and no orphan refresh, so no list probes.
void PruneFinished(Usage& usage) { Detail::PruneFinished(usage, completedValue, g_model.Clock(), g_model.Strict()); }

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
        { "replay-prune", ReplayAfterPrune },
        { "prune-model", PruneModel },
        { "replay-slot", ReplaySlot },
        { "off-after-on", OffAfterOn },
        { "placed-boundary", PlacedAtBoundary },
        { "boundary-probe", BoundaryProbe },
        { "cost", Cost },
#ifdef DLSSNR_EXPOSURE_SCAN_COPY_BUDGET
        { "budget", Budget },
#endif
        { "inert-off", [] { Inert("off"); } },
        { "inert-unwanted", [] { Inert("unwanted"); } },
        { "untracked", [] { Inert("untracked"); } },
        { "match-off", MatchOff },
        { "match-on", MatchOn },
        { "match-eased", MatchEased },
        { "match-inert", MatchInert },
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
