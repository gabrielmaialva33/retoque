#include <dlssnr/DlssNr_Exposure.h>
#include <dlssnr/DlssNr_SubmissionModel.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

using HRESULT = int32_t;
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_FAIL = -1;
inline bool FAILED(HRESULT hr) { return hr < 0; }

enum DXGI_FORMAT : uint32_t
{
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R32_FLOAT = 41,
};

enum D3D12_RESOURCE_STATES : uint32_t
{
    D3D12_RESOURCE_STATE_COMMON = 0,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS = 0x8,
    D3D12_RESOURCE_STATE_COPY_DEST = 0x400,
    D3D12_RESOURCE_STATE_COPY_SOURCE = 0x800,
};

struct D3D12_RANGE
{
    size_t Begin = 0;
    size_t End = 0;
};

enum D3D12_TEXTURE_COPY_TYPE : uint32_t
{
    D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX = 0,
    D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT = 1,
};

struct D3D12_SUBRESOURCE_FOOTPRINT
{
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t Depth = 0;
    uint32_t RowPitch = 0;
};

struct D3D12_PLACED_SUBRESOURCE_FOOTPRINT
{
    uint64_t Offset = 0;
    D3D12_SUBRESOURCE_FOOTPRINT Footprint {};
};

struct ID3D12Device
{
};

struct ID3D12Resource
{
    float* backing = nullptr;
    size_t byteSize = 0;
    bool failMap = false;
    unsigned mapCalls = 0;
    unsigned unmapCalls = 0;
    unsigned releaseCalls = 0;
    bool mappedOpen = false;

    HRESULT Map(unsigned subresource, const D3D12_RANGE* readRange, void** data)
    {
        if (subresource != 0 || mappedOpen || failMap || !backing || !data)
            return E_FAIL;
        if (readRange && readRange->End > byteSize)
            return E_FAIL;
        ++mapCalls;
        mappedOpen = true;
        *data = backing;
        return S_OK;
    }

    void Unmap(unsigned subresource, const D3D12_RANGE* writtenRange)
    {
        assert(subresource == 0 && mappedOpen);
        if (writtenRange)
            assert(writtenRange->Begin == 0 && writtenRange->End == 0);
        ++unmapCalls;
        mappedOpen = false;
    }

    unsigned Release()
    {
        ++releaseCalls;
        return 0;
    }
};

struct D3D12_TEXTURE_COPY_LOCATION
{
    ID3D12Resource* pResource = nullptr;
    D3D12_TEXTURE_COPY_TYPE Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    uint32_t SubresourceIndex = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT PlacedFootprint {};
};

struct CopyOp
{
    ID3D12Resource* dst = nullptr;
    ID3D12Resource* src = nullptr;
    float stagedValue = 0.0f;
};

struct ID3D12GraphicsCommandList
{
    uintptr_t key = 0;
    bool trackable = true;
    std::vector<std::pair<D3D12_RESOURCE_STATES, D3D12_RESOURCE_STATES>> barriers;
    std::vector<CopyOp> copies;

    void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst, unsigned dstX, unsigned dstY, unsigned dstZ,
                           const D3D12_TEXTURE_COPY_LOCATION* src, const void* srcBox)
    {
        assert(dst && src && dstX == 0 && dstY == 0 && dstZ == 0 && srcBox == nullptr);
        assert(dst->pResource != nullptr && src->pResource != nullptr);
        const float value = src->pResource->backing ? src->pResource->backing[0] : 0.0f;
        copies.push_back({ dst->pResource, src->pResource, value });
    }
};

namespace Harness
{
DlssNr::Submission::Detail::Model model;
std::array<uint64_t, DlssNr::Submission::Detail::MaxQueues> completedFence {};
std::array<uint64_t, DlssNr::Submission::Detail::MaxQueues> nextSignal {};
const char* lastRefusal = "test-untracked";
unsigned infoLogs = 0;
unsigned warnLogs = 0;

struct InFlightBatch
{
    size_t queue = 0;
    uint64_t fence = 0;
    std::vector<CopyOp> copies;
};
std::vector<InFlightBatch> inFlight;

uint64_t FenceValue(size_t queue) { return completedFence[queue]; }

void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

uint64_t SubmitList(ID3D12GraphicsCommandList& list, size_t queue = 0, bool signalOk = true)
{
    auto epoch = model.BeforeExecute(list.key);
    const uint64_t fence = signalOk ? ++nextSignal[queue] : 0;
    if (epoch)
    {
        model.NoteQueue(queue);
        DlssNr::Submission::Detail::Model::AfterExecute(epoch, queue, fence, signalOk);
    }
    if (signalOk)
        inFlight.push_back({ queue, fence, list.copies });
    return fence;
}

void ResetList(ID3D12GraphicsCommandList& list, bool succeeded = true)
{
    auto epoch = model.Current(list.key);
    if (epoch)
        DlssNr::Submission::Detail::Model::AfterReset(epoch, succeeded);
    if (succeeded)
    {
        list.barriers.clear();
        list.copies.clear();
    }
}

void AdvanceGpu(size_t queue, uint64_t value)
{
    completedFence[queue] = value;
    if (value == std::numeric_limits<uint64_t>::max())
        return; // Device loss: copies are never proven complete.
    for (auto it = inFlight.begin(); it != inFlight.end();)
    {
        if (it->queue == queue && it->fence <= value)
        {
            for (const auto& op : it->copies)
            {
                if (op.dst && op.dst->backing)
                    op.dst->backing[0] = op.stagedValue;
            }
            it = inFlight.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
} // namespace Harness

#define LOG_INFO(...) (++Harness::infoLogs)
#define LOG_WARN(...) (++Harness::warnLogs)

namespace DlssNr::Submission
{
using Usage = Detail::Usage;
using Lists = Detail::Lists;

bool Track(ID3D12GraphicsCommandList* list, Usage& usage, bool chain = false, Lists lists = Lists::Direct)
{
    (void) lists;
    if (!list || !list->trackable || list->key == 0)
        return false;
    auto epoch = Harness::model.Track(list->key, usage, Harness::FenceValue);
    if (!epoch)
        return false;
    epoch->chain |= chain;
    return true;
}

bool Ready(const Usage& usage)
{
    for (const auto& e : usage.epochs)
        if (e && !Detail::IsReady(*e, Harness::FenceValue))
            return false;
    return true;
}

bool Strict() { return Harness::model.Strict(); }

bool Settled(const Usage& usage)
{
    return Detail::IsSettled(usage, Harness::FenceValue, Harness::model.Clock(), Harness::model.Strict());
}

void PruneFinished(Usage& usage)
{
    Detail::PruneFinished(usage, Harness::FenceValue, Harness::model.Clock(), Harness::model.Strict());
}

bool CaptureReusable(const Usage& usage) { return Detail::IsCaptureReusable(usage, Harness::FenceValue); }

bool Completed(const Usage& usage)
{
    bool any = false;
    for (const auto& e : usage.epochs)
    {
        if (!e)
            continue;
        any = true;
        if (!Detail::IsComplete(*e, Harness::FenceValue))
            return false;
    }
    return any;
}

const char* LastRefusal() { return Harness::lastRefusal; }
} // namespace DlssNr::Submission

namespace DlssNr::Dx12Internal
{
constexpr uint32_t kDlssNrMeterGrid = 64;
constexpr uint32_t kMeterRowBytes = 256;
constexpr size_t kMeterBytes = kMeterRowBytes * kDlssNrMeterGrid;

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    assert(cmdList != nullptr && res != nullptr);
    cmdList->barriers.push_back({ from, to });
}

struct NrState
{
    ID3D12Resource* meter = nullptr;
    ID3D12Resource* meterReadback[4] = {};
    bool meterExposureValid[4] = {};
    unsigned long long meterFrames = 0;
    float gameExposure = 0.0f;
    float gamePreExposure = 1.0f;
    DlssNr::Exposure::PlausibilityLatch exposureLatch;
} g_nr;

// PRODUCTION_METER_FUNCTIONS
} // namespace DlssNr::Dx12Internal

namespace
{
using Harness::AdvanceGpu;
using Harness::Check;
using Harness::ResetList;
using Harness::SubmitList;
using namespace DlssNr::Dx12Internal;

struct Fixture
{
    ID3D12Device device {};
    float meterTexel = 0.5f;
    ID3D12Resource meterRes {};
    std::array<std::array<float, 64>, 4> backing {};
    std::array<ID3D12Resource, 4> readbacks {};

    Fixture()
    {
        Harness::model = DlssNr::Submission::Detail::Model {};
        Harness::completedFence.fill(0);
        Harness::nextSignal.fill(0);
        Harness::inFlight.clear();
        Harness::infoLogs = 0;
        Harness::warnLogs = 0;

        g_nr = NrState {};
        meterRes.backing = &meterTexel;
        meterRes.byteSize = sizeof(float);
        g_nr.meter = &meterRes;
        g_nr.gameExposure = 0.25f;
        g_nr.gamePreExposure = 1.0f;

        for (size_t i = 0; i < 4; ++i)
        {
            // Stale/unwritten readback heap contents that would trip the latch if mapped without completion proof.
            backing[i].fill((i & 1) ? 116.0f : 0.00025f);
            readbacks[i] = ID3D12Resource {};
            readbacks[i].backing = backing[i].data();
            readbacks[i].byteSize = sizeof(backing[i]);
            g_nr.meterReadback[i] = &readbacks[i];
        }
#if TEST_DX12_METER_SESSION
        s_meterReadback = MeterReadbackSession {};
#endif
    }

    unsigned TotalMaps() const
    {
        unsigned total = 0;
        for (const auto& rb : readbacks)
            total += rb.mapCalls;
        return total;
    }
};

void UnsubmittedListsNeverMapOrOverwrite()
{
    Fixture f;
    std::array<ID3D12GraphicsCommandList, 8> lists {};
    for (size_t i = 0; i < lists.size(); ++i)
    {
        lists[i].key = 100 + i;
        f.meterTexel = 0.4f + 0.05f * static_cast<float>(i);
        Harness::model.Advance(i + 1);
        CopyMeterToReadback(&lists[i], &f.device, true);
        ConsumeMeterReadback();
    }

    Check(f.TotalMaps() == 0, "unsubmitted meter recordings were mapped by CPU frame age");
    Check(g_nr.gameExposure == 0.25f, "unsubmitted meter recordings replaced held gameExposure");
    Check(!g_nr.exposureLatch.Latched() && g_nr.exposureLatch.LowCount() == 0 && g_nr.exposureLatch.HighCount() == 0,
          "unsubmitted meter recordings polluted exposureLatch");
    // Only four slots exist; once all four hold unsubmitted copies, later dispatches skip without overwriting.
    for (size_t i = 0; i < 4; ++i)
        Check(lists[i].copies.size() == 1, "initial four slots did not record one copy each");
    for (size_t i = 4; i < 8; ++i)
        Check(lists[i].copies.empty(), "busy meter slots were overwritten while unsubmitted");
}

void GpuLagWaitsForFenceWithoutBlockingAndConsumesInOrder()
{
    Fixture f;
    std::array<ID3D12GraphicsCommandList, 6> lists {};
    const float samples[4] = { 0.2f, 0.4f, 0.5f, 0.8f };
    for (size_t i = 0; i < 4; ++i)
    {
        lists[i].key = 200 + i;
        f.meterTexel = samples[i];
        Harness::model.Advance(i + 1);
        CopyMeterToReadback(&lists[i], &f.device, true);
        ConsumeMeterReadback();
        SubmitList(lists[i], 0);
    }

    // GPU is still at fence 0 (>3 frames behind): frames 4 and 5 must keep the last proven value and skip overwrite.
    for (size_t i = 4; i < 6; ++i)
    {
        lists[i].key = 200 + i;
        f.meterTexel = 0.9f;
        Harness::model.Advance(i + 1);
        CopyMeterToReadback(&lists[i], &f.device, true);
        ConsumeMeterReadback();
        Check(f.TotalMaps() == 0 && g_nr.gameExposure == 0.25f, "lagging GPU read back an incomplete slot");
        Check(lists[i].copies.empty(), "lagging GPU slot was overwritten before fence completion");
    }

    // Completing fence 2 publishes slots 0 and 1 in recording order (ending at 0.4f) and frees two slots.
    AdvanceGpu(0, 2);
    ConsumeMeterReadback();
    Check(std::abs(g_nr.gameExposure - 0.4f) < 1e-6f, "completed fence 2 did not publish second sample");
    ConsumeMeterReadback();
    Check(f.TotalMaps() == 2, "completed slots were mapped more than once");

    // Freed slot can now record a new frame immediately.
    ID3D12GraphicsCommandList next {};
    next.key = 299;
    f.meterTexel = 0.625f;
    Harness::model.Advance(10);
    CopyMeterToReadback(&next, &f.device, true);
    Check(next.copies.size() == 1, "completed slot did not become reusable");
    const uint64_t fence = SubmitList(next, 0);
    AdvanceGpu(0, fence);
    ConsumeMeterReadback();
    Check(std::abs(g_nr.gameExposure - 0.625f) < 1e-6f, "reused slot did not publish newest exposure");
}

void PerSlotPreExposurePreventsFalseSessionLatch()
{
    Fixture f;
    // Every frame has an exact implied white point of 1.0 (PreExposure == ExposureTexture), while auto-exposure
    // swings between 1000 and 0.001 across frames. Pairing historical exposure with current gamePreExposure
    // invents 0.001 and 1000 implied white points and falsely latches for the session.
    const float pairs[] = { 1000.0f, 0.001f, 1000.0f, 0.001f, 1.0f, 1.0f, 1.0f, 1.0f };
    std::array<ID3D12GraphicsCommandList, 8> lists {};
    std::array<uint64_t, 8> fences {};

    for (size_t i = 0; i < 8; ++i)
    {
        lists[i].key = 300 + i;
        g_nr.gamePreExposure = pairs[i];
        f.meterTexel = pairs[i];
        Harness::model.Advance(i + 1);
        if (i >= 3)
            AdvanceGpu(0, fences[i - 3]);
        CopyMeterToReadback(&lists[i], &f.device, true);
        ConsumeMeterReadback();
        fences[i] = SubmitList(lists[i], 0);
        if (i >= 3)
            ResetList(lists[i - 3]);
    }
    AdvanceGpu(0, fences[7]);
    ConsumeMeterReadback();

    Check(!g_nr.exposureLatch.Latched(), "per-frame PreExposure==Exposure falsely tripped exposureLatch");
    Check(g_nr.exposureLatch.LowCount() == 0 && g_nr.exposureLatch.HighCount() == 0,
          "cross-frame preExposure mismatch recorded out-of-range excursions");
    Check(std::abs(g_nr.gameExposure - 1.0f) < 1e-6f, "final proven exposure was not published");
}

void OutOfOrderCancelledInvalidAndInvalidatedSlots()
{
    // 1. Out-of-order multi-queue completion: newer sequence wins and older late arrival is ignored.
    {
        Fixture f;
        ID3D12GraphicsCommandList older {}, newer {};
        older.key = 401;
        newer.key = 402;
        f.meterTexel = 0.2f;
        CopyMeterToReadback(&older, &f.device, true);
        const uint64_t fOld = SubmitList(older, 0);

        f.meterTexel = 0.75f;
        CopyMeterToReadback(&newer, &f.device, true);
        const uint64_t fNew = SubmitList(newer, 1);

        AdvanceGpu(1, fNew);
        ConsumeMeterReadback();
        Check(std::abs(g_nr.gameExposure - 0.75f) < 1e-6f, "newer completed queue sample was not published");

        AdvanceGpu(0, fOld);
        ConsumeMeterReadback();
        Check(std::abs(g_nr.gameExposure - 0.75f) < 1e-6f, "older late-completing sample overwrote newer exposure");
    }

    // 2. Cancelled list (Reset before ExecuteCommandLists) frees its slot without mapping stale data.
    {
        Fixture f;
        std::array<ID3D12GraphicsCommandList, 4> cancelled {};
        for (size_t i = 0; i < 4; ++i)
        {
            cancelled[i].key = 410 + i;
            f.meterTexel = 0.5f;
            CopyMeterToReadback(&cancelled[i], &f.device, true);
            ResetList(cancelled[i], true);
        }
        ConsumeMeterReadback();
        Check(f.TotalMaps() == 0 && g_nr.gameExposure == 0.25f, "cancelled command lists were mapped");

        ID3D12GraphicsCommandList fresh {};
        fresh.key = 420;
        f.meterTexel = 0.5f;
        CopyMeterToReadback(&fresh, &f.device, true);
        Check(fresh.copies.size() == 1, "cancelled meter slots did not return to the free pool");
    }

    // 3. Invalid samples and unbound exposure keep the last good reading.
    {
        for (float bad : { 0.0f, -1.0f, 1e-6f, 1e6f, std::numeric_limits<float>::quiet_NaN(),
                           std::numeric_limits<float>::infinity() })
        {
            Fixture f;
            ID3D12GraphicsCommandList cmd {};
            cmd.key = 430;
            f.meterTexel = bad;
            CopyMeterToReadback(&cmd, &f.device, true);
            const uint64_t fence = SubmitList(cmd, 0);
            AdvanceGpu(0, fence);
            ConsumeMeterReadback();
            Check(g_nr.gameExposure == 0.25f, "invalid sample replaced held gameExposure");
        }

        Fixture f;
        ID3D12GraphicsCommandList unbound {};
        unbound.key = 431;
        f.meterTexel = 0.8f;
        CopyMeterToReadback(&unbound, &f.device, false);
        const uint64_t fence = SubmitList(unbound, 0);
        AdvanceGpu(0, fence);
        ConsumeMeterReadback();
        Check(g_nr.gameExposure == 0.25f, "unbound exposure slot published scene pixel as exposure");
    }

    // 4. InvalidateExposureMeter keeps in-flight slots occupied until fence completion, discards their values,
    // and allows fresh post-invalidation readings through remaining slots.
    {
        Fixture f;
        ID3D12GraphicsCommandList pre {};
        pre.key = 440;
        f.meterTexel = 0.9f;
        CopyMeterToReadback(&pre, &f.device, true);
        const uint64_t preFence = SubmitList(pre, 0);

        InvalidateExposureMeter();
        Check(g_nr.gameExposure == 0.0f, "InvalidateExposureMeter did not clear gameExposure");

        // Slot 0 is still in flight on the GPU, so three new unsubmitted recordings fill slots 1..3 and a fourth skips.
        std::array<ID3D12GraphicsCommandList, 4> post {};
        for (size_t i = 0; i < 4; ++i)
        {
            post[i].key = 441 + i;
            f.meterTexel = 0.35f;
            CopyMeterToReadback(&post[i], &f.device, true);
        }
        Check(post[0].copies.size() == 1 && post[1].copies.size() == 1 && post[2].copies.size() == 1,
              "free slots after InvalidateExposureMeter did not accept recordings");
        Check(post[3].copies.empty(), "InvalidateExposureMeter freed an in-flight slot before its GPU copy completed");

        AdvanceGpu(0, preFence);
        ConsumeMeterReadback();
        Check(g_nr.gameExposure == 0.0f, "pre-invalidation in-flight sample survived InvalidateExposureMeter");

        const uint64_t postFence = SubmitList(post[0], 0);
        AdvanceGpu(0, postFence);
        ConsumeMeterReadback();
        Check(std::abs(g_nr.gameExposure - 0.35f) < 1e-6f, "post-invalidation sample was not published");
    }

    // 5. Untracked command list refuses cleanly before any barrier or copy.
    {
        Fixture f;
        ID3D12GraphicsCommandList untracked {};
        untracked.key = 450;
        untracked.trackable = false;
        CopyMeterToReadback(&untracked, &f.device, true);
        CopyMeterToReadback(&untracked, &f.device, true);
        Check(untracked.barriers.empty() && untracked.copies.empty(),
              "untracked command list recorded meter barriers or copies");
        Check(Harness::infoLogs == 1, "untracked refusal was not logged exactly once");
    }
}

#if TEST_DX12_METER_SHUTDOWN
void ShutdownRetainsInFlightOrDeviceLostAndReleasesSettled()
{
    // Settled (completed + Reset) releases meter + all four readbacks.
    {
        Fixture f;
        ID3D12GraphicsCommandList cmd {};
        cmd.key = 500;
        f.meterTexel = 0.5f;
        CopyMeterToReadback(&cmd, &f.device, true);
        const uint64_t fence = SubmitList(cmd, 0);
        AdvanceGpu(0, fence);
        ResetList(cmd, true);

        ShutdownExposureMeter();
        Check(f.meterRes.releaseCalls == 1, "settled meter texture was not released at shutdown");
        for (const auto& rb : f.readbacks)
            Check(rb.releaseCalls == 1, "settled meter readback was not released at shutdown");
        Check(g_nr.meter == nullptr && g_nr.meterReadback[0] == nullptr, "shutdown left dangling meter pointers");
    }

    // Device-lost (UINT64_MAX) or still-in-flight copy retains the affected readback and meter source texture.
    for (uint64_t fenceState : { uint64_t { 0 }, std::numeric_limits<uint64_t>::max() })
    {
        Fixture f;
        ID3D12GraphicsCommandList cmd {};
        cmd.key = 510;
        f.meterTexel = 0.5f;
        CopyMeterToReadback(&cmd, &f.device, true);
        SubmitList(cmd, 0);
        AdvanceGpu(0, fenceState);

        ShutdownExposureMeter();
        Check(f.meterRes.releaseCalls == 0, "meter source texture was freed while copy was unsettled/device-lost");
        Check(f.readbacks[0].releaseCalls == 0, "in-flight/device-lost meter readback was freed under the GPU");
        Check(f.readbacks[1].releaseCalls == 1 && f.readbacks[2].releaseCalls == 1 && f.readbacks[3].releaseCalls == 1,
              "unused meter readback slots were not released cleanly");
        Check(g_nr.meter == nullptr && g_nr.meterReadback[0] == nullptr,
              "shutdown did not null retained meter pointers");
    }

#if TEST_DX12_METER_BEGIN
    // BeginMeterDispatch refuses when all slots are busy or untracked, and 200 frames of un-reset completed lists
    // prune cleanly across the four per-slot lifetime usages without saturating MaxUses (64).
    {
        Fixture f;
        ID3D12GraphicsCommandList untracked {};
        untracked.key = 600;
        untracked.trackable = false;
        Check(!BeginMeterDispatch(&untracked), "BeginMeterDispatch accepted an untracked command list");
        Check(Harness::infoLogs == 1, "BeginMeterDispatch did not log untracked refusal once");

        std::array<ID3D12GraphicsCommandList, 200> lists {};
        for (size_t i = 0; i < lists.size(); ++i)
        {
            lists[i].key = 700 + i;
            f.meterTexel = 0.3f + 0.001f * static_cast<float>(i);
            Harness::model.Advance(i + 1);
            Check(BeginMeterDispatch(&lists[i]), "BeginMeterDispatch exhausted per-slot lifetime usage");
            CopyMeterToReadback(&lists[i], &f.device, true);
            const uint64_t fence = SubmitList(lists[i], 0);
            AdvanceGpu(0, fence);
            ConsumeMeterReadback();
        }
        Check(std::abs(g_nr.gameExposure - (0.3f + 0.199f)) < 1e-6f, "200-frame run did not publish final sample");

        // Age past AgeOutTicks (128) so un-reset completed lists settle cleanly at shutdown.
        Harness::model.Advance(400);
        ShutdownExposureMeter();
        Check(f.meterRes.releaseCalls == 1 && f.readbacks[0].releaseCalls == 1,
              "aged completed lists did not settle cleanly at shutdown");
    }
#endif
}
#endif
} // namespace

int main()
{
    UnsubmittedListsNeverMapOrOverwrite();
    GpuLagWaitsForFenceWithoutBlockingAndConsumesInOrder();
    PerSlotPreExposurePreventsFalseSessionLatch();
    OutOfOrderCancelledInvalidAndInvalidatedSlots();
#if TEST_DX12_METER_SHUTDOWN
    ShutdownRetainsInFlightOrDeviceLostAndReleasesSettled();
#endif
    std::printf("PASS: D3D12 exposure meter readback -- fence-proven consumption, per-slot preExposure, bounded ring, "
                "invalidation and shutdown lifetime\n");
    return 0;
}
