// Characterisation of the production descriptor/resource tracking, sliced from ResTrack_dx12.{h,cpp}.
// run.py prepends fakes.h, ResourceInfo, the HeapInfo region and ResourceDestroyed/TrackResourceRelease.
// The cases observe behaviour only (what a descriptor slot returns after views, copies, clears, heap
// teardown and resource destruction), so the same file holds before and after the reference index change.

#include <random>
#include <string>
#include <thread>

static std::atomic<int> failures { 0 };

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);                         \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace
{
constexpr UINT Increment = 32;

std::shared_ptr<HeapInfo> MakeHeap(UINT descriptors, SIZE_T cpuBase, SIZE_T gpuBase)
{
    static ID3D12DescriptorHeap heaps[64];
    static unsigned next = 0;
    const SIZE_T bytes = static_cast<SIZE_T>(descriptors) * Increment;
    return std::make_shared<HeapInfo>(&heaps[next++ % 64], cpuBase, cpuBase + bytes, gpuBase, gpuBase + bytes,
                                      descriptors, Increment, 0);
}

ResourceInfo View(ID3D12Resource* resource, UINT64 width = 1920, UINT height = 1080,
                  DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM, ResourceType type = SRV)
{
    ResourceInfo info {};
    info.buffer = resource;
    info.width = width;
    info.height = height;
    info.format = format;
    info.type = type;
    info.lifetimeTracked = true;
    return info;
}

ID3D12Resource* Bound(const HeapInfo& heap, UINT index)
{
    ResourceInfo info {};
    return heap.GetByIndex(index, info) ? info.buffer : nullptr;
}

void Untracked()
{
    ID3D12Resource never;
    auto heap = MakeHeap(8, 0x1000, 0x900000);

    heap->SetByIndex(1, View(&never));
    CHECK(Bound(*heap, 1) == nullptr); // a view of an unregistered resource is not retained
    heap->SetByCpuHandle(0x1000 + 2 * Increment, View(&never));
    CHECK(Bound(*heap, 2) == nullptr);
    CHECK(!ResTrack_Dx12::TrackResourceRelease(nullptr));
}

void Registration()
{
    ID3D12Resource resource;
    CHECK(ResTrack_Dx12::TrackResourceRelease(&resource));
    CHECK(resource.CallbackCount() == 1);
    CHECK(ResTrack_Dx12::TrackResourceRelease(&resource)); // already tracked: no second callback
    CHECK(resource.CallbackCount() == 1);
    CHECK(resource.notifier.references.load() == 0);

    ID3D12Resource noNotifier;
    noNotifier.notifierAvailable = false;
    CHECK(!ResTrack_Dx12::TrackResourceRelease(&noNotifier));
    auto heap = MakeHeap(4, 0x2000, 0xA00000);
    heap->SetByIndex(0, View(&noNotifier));
    CHECK(Bound(*heap, 0) == nullptr);

    ID3D12Resource failing;
    failing.registerResult = -1;
    CHECK(!ResTrack_Dx12::TrackResourceRelease(&failing));
    CHECK(failing.CallbackCount() == 0);
    CHECK(failing.notifier.references.load() == 0);
    heap->SetByIndex(1, View(&failing));
    CHECK(Bound(*heap, 1) == nullptr);

    resource.Destroy();
}

void ViewAndDestroy()
{
    ID3D12Resource resource;
    CHECK(ResTrack_Dx12::TrackResourceRelease(&resource));
    auto heap = MakeHeap(16, 0x3000, 0xB00000);

    heap->SetByCpuHandle(0x3000 + 5 * Increment, View(&resource, 2560, 1440, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV));
    ResourceInfo info {};
    CHECK(heap->GetByGpuHandle(0xB00000 + 5 * Increment, info));
    CHECK(info.buffer == &resource && info.width == 2560 && info.height == 1440);
    CHECK(info.format == DXGI_FORMAT_R16G16B16A16_FLOAT && info.type == UAV && info.lifetimeTracked);

    // Metadata refresh for the same buffer keeps the slot tracked
    heap->SetByIndex(5, View(&resource, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, SRV));
    CHECK(heap->GetByIndex(5, info) && info.format == DXGI_FORMAT_R8G8B8A8_UNORM && info.type == SRV);

    {
        std::scoped_lock lock(Hudfix_Dx12::removedMutex);
        Hudfix_Dx12::removed.clear();
    }
    resource.Destroy();
    CHECK(Bound(*heap, 5) == nullptr);
    CHECK(Hudfix_Dx12::removed.size() == 1 && Hudfix_Dx12::removed[0] == &resource);

    // Destruction forgets the registration: a reused address registers a fresh callback
    CHECK(ResTrack_Dx12::TrackResourceRelease(&resource));
    CHECK(resource.CallbackCount() == 1);
    heap->SetByIndex(5, View(&resource));
    CHECK(Bound(*heap, 5) == &resource);
    resource.Destroy();
    CHECK(Bound(*heap, 5) == nullptr);
}

void OverwriteAndClear()
{
    ID3D12Resource first, second;
    CHECK(ResTrack_Dx12::TrackResourceRelease(&first));
    CHECK(ResTrack_Dx12::TrackResourceRelease(&second));
    auto heap = MakeHeap(16, 0x4000, 0xC00000);

    heap->SetByIndex(0, View(&first));
    heap->SetByIndex(0, View(&first)); // same buffer twice
    heap->SetByIndex(0, View(&second));
    heap->SetByIndex(1, View(&first));
    heap->SetByIndex(2, View(&first));
    heap->ClearByCpuHandle(0x4000 + 2 * Increment);
    heap->SetByIndex(3, View(&first));
    heap->ClearByGpuHandle(0xC00000 + 3 * Increment);
    heap->SetByIndex(3, View(&second));
    heap->ClearByIndex(4);

    first.Destroy();
    CHECK(Bound(*heap, 0) == &second); // the overwritten slot no longer belongs to the first resource
    CHECK(Bound(*heap, 1) == nullptr);
    CHECK(Bound(*heap, 2) == nullptr);
    CHECK(Bound(*heap, 3) == &second);

    // A slot reassigned to a different resource is not cleared by the old resource's stale reference
    heap->SetByIndex(6, View(&second));
    second.Destroy();
    CHECK(Bound(*heap, 0) == nullptr);
    CHECK(Bound(*heap, 3) == nullptr);
    CHECK(Bound(*heap, 6) == nullptr);
}

void ManySlotsManyHeaps()
{
    ID3D12Resource shared, other;
    CHECK(ResTrack_Dx12::TrackResourceRelease(&shared));
    CHECK(ResTrack_Dx12::TrackResourceRelease(&other));
    auto a = MakeHeap(4096, 0x100000, 0x10000000);
    auto b = MakeHeap(4096, 0x400000, 0x20000000);

    for (UINT i = 0; i < 4096; ++i)
    {
        a->SetByIndex(i, View(i % 3 == 0 ? &other : &shared));
        b->SetByIndex(4095 - i, View(&shared));
    }

    for (UINT i = 0; i < 4096; i += 2)
        b->ClearByIndex(i);

    shared.Destroy();
    bool ok = true;
    for (UINT i = 0; i < 4096; ++i)
    {
        ok &= Bound(*a, i) == (i % 3 == 0 ? &other : nullptr);
        ok &= Bound(*b, i) == nullptr;
    }
    CHECK(ok);

    other.Destroy();
    ok = true;
    for (UINT i = 0; i < 4096; ++i)
        ok &= Bound(*a, i) == nullptr;
    CHECK(ok);
}

void HeapTeardown()
{
    ID3D12Resource resource;
    CHECK(ResTrack_Dx12::TrackResourceRelease(&resource));
    auto released = MakeHeap(8, 0x5000, 0xD00000);
    auto dropped = MakeHeap(8, 0x6000, 0xE00000);
    auto live = MakeHeap(8, 0x7000, 0xF00000);

    for (UINT i = 0; i < 8; ++i)
    {
        released->SetByIndex(i, View(&resource));
        dropped->SetByIndex(i, View(&resource));
        live->SetByIndex(i, View(&resource));
    }

    CHECK(released->DeactivateAndClear());
    CHECK(!released->DeactivateAndClear()); // once
    CHECK(Bound(*released, 0) == nullptr);
    released->SetByIndex(0, View(&resource)); // inactive heaps ignore writes
    CHECK(Bound(*released, 0) == nullptr);

    // A heap object destroyed without teardown leaves expired references behind; destruction skips them
    dropped.reset();

    // A replacement heap (new version) at the same slots tracks independently
    auto replacement = MakeHeap(8, 0x5000, 0xD00000);
    replacement->SetByIndex(3, View(&resource));

    resource.Destroy();
    bool ok = true;
    for (UINT i = 0; i < 8; ++i)
        ok &= Bound(*live, i) == nullptr;
    CHECK(ok);
    CHECK(Bound(*replacement, 3) == nullptr);
}

void ShuttingDown()
{
    ID3D12Resource resource;
    CHECK(ResTrack_Dx12::TrackResourceRelease(&resource));
    auto heap = MakeHeap(4, 0x8000, 0x1000000);
    heap->SetByIndex(0, View(&resource));

    State::Instance().isShuttingDown = true;
    ID3D12Resource late;
    CHECK(!ResTrack_Dx12::TrackResourceRelease(&late));
    ResTrack_Dx12::ResourceDestroyed(&resource); // ignored during shutdown
    CHECK(Bound(*heap, 0) == &resource);
    State::Instance().isShuttingDown = false;

    resource.Destroy();
    CHECK(Bound(*heap, 0) == nullptr);
}

// Concurrent views/copies/clears from several threads on shared heaps and resources, with per-thread
// resources destroyed and re-registered (address reuse) during the run. After quiescing, destroying every
// shared resource must empty every slot: a lost reverse reference leaves a slot still bound.
void SliceStress()
{
    constexpr unsigned Threads = 8;
    constexpr unsigned Shared = 24;
    constexpr UINT Descriptors = 2048;
    constexpr unsigned Iterations = 20000;

    std::vector<std::unique_ptr<ID3D12Resource>> shared(Shared);
    for (auto& resource : shared)
    {
        resource = std::make_unique<ID3D12Resource>();
        CHECK(ResTrack_Dx12::TrackResourceRelease(resource.get()));
    }

    std::vector<std::shared_ptr<HeapInfo>> heaps;
    for (SIZE_T h = 0; h < 4; ++h)
        heaps.push_back(MakeHeap(Descriptors, 0x10000000 * (h + 1), 0x100000000ull * (h + 1)));

    std::vector<std::unique_ptr<ID3D12Resource>> privateResources(Threads);
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < Threads; ++t)
    {
        privateResources[t] = std::make_unique<ID3D12Resource>();
        workers.emplace_back(
            [&, t]
            {
                std::mt19937_64 random(0x5EED + t);
                auto* mine = privateResources[t].get();
                CHECK(ResTrack_Dx12::TrackResourceRelease(mine));
                std::vector<std::pair<HeapInfo*, UINT>> mySlots;

                for (unsigned i = 0; i < Iterations; ++i)
                {
                    auto& heap = *heaps[random() % heaps.size()];
                    const auto index = static_cast<UINT>(random() % Descriptors);
                    switch (random() % 8)
                    {
                    case 0:
                    case 1:
                    case 2:
                        heap.SetByIndex(index, View(shared[random() % Shared].get()));
                        break;
                    case 3:
                    {
                        // Copy, as hkCopyDescriptors does: read the source then write the destination
                        ResourceInfo source {};
                        auto& from = *heaps[random() % heaps.size()];
                        if (from.GetByIndex(static_cast<UINT>(random() % Descriptors), source) && source.buffer != mine)
                            heap.SetByIndex(index, source);
                        else
                            heap.ClearByIndex(index);
                        break;
                    }
                    case 4:
                        heap.ClearByIndex(index);
                        break;
                    case 5:
                    case 6:
                    {
                        // Private resource lives only in this thread's own index range of heap 0
                        const auto own = static_cast<UINT>(t * (Descriptors / Threads) + random() % 16);
                        heaps[0]->SetByIndex(own, View(mine));
                        break;
                    }
                    default:
                    {
                        mine->Destroy();
                        for (UINT k = 0; k < 16; ++k)
                            if (Bound(*heaps[0], static_cast<UINT>(t * (Descriptors / Threads) + k)) == mine)
                                failures++;
                        CHECK(ResTrack_Dx12::TrackResourceRelease(mine));
                        break;
                    }
                    }
                }

                mine->Destroy();
            });
    }

    for (auto& worker : workers)
        worker.join();

    for (auto& resource : shared)
        resource->Destroy();

    UINT stillBound = 0;
    for (auto& heap : heaps)
        for (UINT i = 0; i < Descriptors; ++i)
            stillBound += Bound(*heap, i) != nullptr;
    CHECK(stillBound == 0);
}
} // namespace

int main(int argc, char** argv)
{
    const std::string selected = argc > 1 ? argv[1] : "all";
    const std::pair<const char*, void (*)()> cases[] = {
        { "untracked", Untracked },           { "registration", Registration },
        { "view-destroy", ViewAndDestroy },   { "overwrite-clear", OverwriteAndClear },
        { "many-slots", ManySlotsManyHeaps }, { "heap-teardown", HeapTeardown },
        { "shutting-down", ShuttingDown },    { "slice-stress", SliceStress },
    };

    bool ran = false;
    for (const auto& [name, run] : cases)
    {
        if (selected != "all" && selected != name)
            continue;
        const int before = failures;
        run();
        std::printf("%s %s\n", failures == before ? "PASS" : "FAIL", name);
        ran = true;
    }

    if (!ran)
    {
        std::fprintf(stderr, "unknown case %s\n", selected.c_str());
        return 2;
    }

    return failures == 0 ? 0 : 1;
}
