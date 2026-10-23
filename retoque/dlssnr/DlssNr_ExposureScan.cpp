#include "pch.h"

#include "DlssNr_ExposureScan.h"

#include <dlssnr/DlssNr_Submission.h>

#include <Config.h>
#include <Util.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

namespace DlssNr
{
namespace ExposureScan
{
namespace
{

// How a candidate lives, is read and is let go: docs/dlssnr/design/exposure-scan-capacity.md. In short, a candidate
// is ACTIVE (copied every sweep it is certified for), WAITING (held, not copied, until a place frees up) or RETIRING
// (never copied again, held until every recording that copied it has settled). Readings are credited by the
// candidate's id, never by list position, and only from readback slots whose recording the GPU completed.

// How many candidates are read at once: the readback slot's offsets. A cap on how many candidates are copied
// a sweep, kept at the size Cyberpunk's REDengine needed (its exposure lands past slot 24, and janblade measured it
// at 61-63): it creates dozens of tiny UAV buffers the same 4/12 bytes as an exposure. More than this wait their turn
// rather than being refused.
constexpr size_t kMaxCandidates = 64;

// How many wait for a place, held but never copied. A game that makes hundreds of buffers of the scan's shape at
// load (The Witcher 3: 387, the first 256 holding integers or zero) fills the active list long before its exposure
// is made; the ones after it wait here. When this is full, the oldest one that was never copied gives way.
constexpr size_t kMaxWaiting = 192;

// Retiring candidates are held until their copies settle. Past this many, nothing new is promoted to active -- so
// nothing new is copied, so nothing new can need settling -- until the GPU answers for the ones held.
constexpr size_t kMaxRetiring = 256;

// Ring depth for the readbacks. A slot is rewritten only once its recording completed, so four is how many sweeps
// can be in flight, not how far behind a read is assumed to be retired.
constexpr unsigned int kSlots = 4;

// Every candidate's value lands in one buffer, at its own offset, so there is one copy per candidate
// but only one buffer per slot. 16 bytes each is enough for the widest format worth reading.
// D3D12 requires a placed-footprint offset to be a multiple of
// D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT (512). A texture candidate at index i copies to i*kStride,
// so the stride is that alignment; buffer copies have no such rule and are unaffected by the waste.
constexpr unsigned int kStride = 512;

// What an exposure could plausibly be. Outside these it is a flag, a counter, a sentinel or a
// zeroed buffer nobody has written yet -- Nioh 3 offers all four, including one holding 1000000.
constexpr float kFloor = 1e-6f;
constexpr float kCeiling = 1e4f;

// Readings, all certified, none of them plausible, after which a candidate is false: it gives way to a waiting one.
// An exposure is written every frame from the start; zero, a counter, a sentinel or an integer read as a float
// (65536 is 9.18e-41) is not one.
constexpr unsigned int kFalseAfter = 60;

// Once a mover has been found the sweep only needs to keep feeding the white point a fresh value,
// not to re-scan for a mover. An auto-exposure eases over many frames, so re-reading the whole
// candidate ring (up to kMaxCandidates x {2 barriers + 1 copy} on the game's command list plus a CPU
// readback) every frame is wasted work. Run the full sweep every kScanThrottle frames instead and
// reuse the last read value between sweeps. Before discovery the sweep still runs every frame (see
// Tick), so a mover is found as fast as the unthrottled code. The cost removed is proportional to
// this; the white-point staleness is proportional to it too.
constexpr unsigned int kScanThrottle = 4;

// How many frames of watching without movement before the scan is considered settled-barren. At
// sixty frames a second this is about half a minute, which is long enough to have walked somewhere
// with different light in it and short enough that nobody waits on it wondering. Once a candidate has
// been seen this many reads without any moving, the exposure is not in this game (e.g. KCD2 computes
// one but never hands it to the upscaler), so the sweep's value is never consumed (BestValue -> 0) and
// the sweep cost is pure waste; drop to the liveness cadence below then. While a candidate that has
// not been read this long waits for a place, the scan is not barren: a quiet one is moved aside for it.
constexpr unsigned int kPatience = 1800;

// Barren cadence. With nothing moving, a full sweep no longer feeds any value anywhere; its only job
// is to notice a NEW candidate (created by a later level load) or a long-quiet buffer that finally
// starts to move. Neither is quick: buffers appear at level load, and an auto-exposure eases over many
// frames. So a barren scan sweeps once every kBarrenSweepInterval frames instead of every
// kScanThrottle -- kBarrenSweepInterval / kScanThrottle times less GPU/CPU work for the KCD2 case, at
// the cost of detecting a late mover up to ~2s later. That cost is invisible in practice: while the
// scan is barren it feeds nothing (BestValue -> 0), so the white point comes from another source until
// a mover is found, and once one is found the cadence returns to kScanThrottle. A freshly adopted
// candidate (reads == 0) still gets its first sweep next frame, so a new buffer is not left unwatched
// for a full interval.
constexpr unsigned int kBarrenSweepInterval = 120;

// Individual lines for adoptions, let-gos, false and moved-aside candidates, written for the first this many events
// of the session. After that only the periodic summary carries them: a game that recycles buffers every few frames
// would otherwise write a line every few frames for as long as it runs.
constexpr unsigned int kEventLines = 256;

// Where a candidate was seen, which decides whether the scan can keep its memory alive (section 3.1).
enum class Origin
{
    Committed, // owns its memory: our reference is a reference to the memory
    Placed,    // in a heap we hold a reference on, released after the resource
    View       // seen only through a view; heap unknown, so the feature-release boundary releases it as it always did
};

const char* OriginName(Origin o)
{
    switch (o)
    {
    case Origin::Committed:
        return "committed";
    case Origin::Placed:
        return "placed";
    default:
        return "seen through a view";
    }
}

struct Tracked
{
    ID3D12Resource* resource = nullptr; // our reference
    ID3D12Heap* heap = nullptr;         // our reference on a placed resource's heap, or null
    Origin origin = Origin::Committed;
    unsigned int id = 0;
    std::string shape;
    bool isBuffer = false;
    unsigned int bytes = 4;
    DXGI_FORMAT texFormat = DXGI_FORMAT_UNKNOWN; // the source texture's format, for CopyTextureRegion

    float latest = 0.0f;
    float lowest = 0.0f;
    float highest = 0.0f;
    unsigned long long latestSerial = 0; // the sweep `latest` came from, so a late slot cannot overwrite a newer value
    unsigned int reads = 0;
    unsigned int inRange = 0; // reads that could plausibly be an exposure
    bool moves = false;

    bool alive = false;              // the last probe saw a holder besides the scan
    bool copied = false;             // a copy of it was ever recorded
    DlssNr::Submission::Usage usage; // every recording that copied it
};

// One readback buffer's last recording: which candidate went to which offset, and its submission evidence.
struct Slot
{
    bool pending = false;
    unsigned long long serial = 0;
    std::array<unsigned int, kMaxCandidates> ids {}; // 0: nobody was copied to that offset
    DlssNr::Submission::Usage usage;
};

// Event counts since the last summary line.
struct Counts
{
    unsigned int adopted = 0, letGo = 0, falseOnes = 0, movedAside = 0, dropped = 0, refused = 0, released = 0;

    bool Any() const { return adopted + letGo + falseOnes + movedAside + dropped + refused + released != 0; }
};

struct ScanState
{
    unsigned int examined = 0;
    std::vector<Tracked> active;   // copied, at most kMaxCandidates; position i copies to offset i * kStride
    std::deque<Tracked> waiting;   // held, not copied
    std::vector<Tracked> retiring; // held until settled, never copied
    ID3D12Resource* readback[kSlots] = {};
    Slot slots[kSlots];
    unsigned int nextSlot = 0;
    unsigned int nextId = 0;
    unsigned long long frames = 0; // Tick() calls (drives the periodic readout cadence)
    unsigned long long writes = 0; // sweeps actually recorded
    const char* status = "not started";
    bool complained = false;   // once per release generation: a newcomer refused
    bool barrenLogged = false; // once per transition into the barren cadence
    bool saidUntracked = false;
    bool saidUnconfirmed = false;
    bool saidRetiringFull = false;
    unsigned int nearMissLogged = 0; // bounded diagnostic; see NoteResource
    unsigned int passedOver = 0;     // candidates seen while the scan was not wanted, and not kept (Adopt)
    unsigned int eventLines = 0;
    Counts since;
};

ScanState g_scan;
std::mutex g_scanMutex;

// Whether the resource-creation detours went in when the device was hooked. They are attached only if
// Neural Rendering was on at that moment, so a session that turns it on later has a scan that can
// never see anything -- which has to be said, not shown as an endless "waiting".
std::atomic<bool> g_resourceHooksAttached { false };

constexpr const char* kNeedsRestart =
    "The exposure scan needs a restart: Neural Rendering was off when the game started.";

constexpr const char* kUntracked =
    "the pass's command list cannot be tracked, so the scan cannot prove its readbacks and copies nothing";

constexpr const char* kUnconfirmed =
    "the GPU has not confirmed the scan's last copies; nothing is read or copied until it does";

// True when the scan is wanted but cannot see. Logs the first time.
bool BlindSinceStart()
{
    if (g_resourceHooksAttached.load(std::memory_order_relaxed))
        return false;

    static std::atomic<bool> logged { false };
    if (!logged.exchange(true))
        LOG_WARN("DLSS-NR exposure scan: {}", kNeedsRestart);

    return true;
}

// Formats an exposure could plausibly be in: floating point, one or two channels.
//
// Two channels because eye adaptation commonly carries the value and something alongside it -- the
// previous frame's value, or a target it is easing toward. Anything wider is a picture rather than a
// number. Integer formats are excluded because an exposure is a scale and a normalised integer
// cannot hold one.
// outBytes is the size of the FIRST channel only -- that is the one texel this reads back, and a
// two-channel format that stored 4 or 8 there would be decoded as the wrong type. The copy uses the
// real format (outFormat) so it matches the source texture; the read uses outBytes.
bool PlausibleFormat(DXGI_FORMAT f, unsigned int* outBytes, const char** outName, DXGI_FORMAT* outFormat)
{
    *outFormat = f;
    switch (f)
    {
    case DXGI_FORMAT_R32_FLOAT:
        *outBytes = 4;
        *outName = "R32_FLOAT";
        return true;
    case DXGI_FORMAT_R16_FLOAT:
        *outBytes = 2;
        *outName = "R16_FLOAT";
        return true;
    case DXGI_FORMAT_R32G32_FLOAT:
        *outBytes = 4;
        *outName = "R32G32_FLOAT";
        return true;
    case DXGI_FORMAT_R16G16_FLOAT:
        *outBytes = 2;
        *outName = "R16G16_FLOAT";
        return true;
    default:
        return false;
    }
}

float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (uint32_t) (h & 0x8000u) << 16;
    uint32_t exponent = (h >> 10) & 0x1Fu;
    uint32_t mantissa = h & 0x3FFu;

    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            const uint32_t bits = sign;
            float out;
            std::memcpy(&out, &bits, sizeof(out));
            return out;
        }

        // Subnormal: normalise it by hand.
        exponent = 1;

        while ((mantissa & 0x400u) == 0)
        {
            mantissa <<= 1;
            --exponent;
        }

        mantissa &= 0x3FFu;
    }
    else if (exponent == 31)
    {
        const uint32_t bits = sign | 0x7F800000u | (mantissa << 13);
        float out;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }

    const uint32_t bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    if (res == nullptr || from == to)
        return;

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = res;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    cmdList->ResourceBarrier(1, &barrier);
}

bool EnsureReadback(ID3D12Device* device)
{
    for (unsigned int i = 0; i < kSlots; ++i)
    {
        if (g_scan.readback[i] != nullptr)
            continue;

        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = kStride * kMaxCandidates;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&g_scan.readback[i]))))
        {
            g_scan.status = "could not allocate the readback buffers";
            return false;
        }
    }

    return true;
}

// Whether the scan should be running at all.
//
// Choosing it as the white point's source is the whole of the answer for anybody using this.
// The separate setting survives as a developer override, for the one case a user has no reason
// to want: watching the scan in a game that supplies a REAL exposure, so the two can be
// compared in the log. That is validation work, not a control, and it does not belong in a
// panel.
bool Wanted()
{
    return Config::Instance()->DlssNrWhitePointSource.value_or_default() == 2 ||
           Config::Instance()->DlssNrScanExposure.value_or_default();
}

// Under g_scanMutex. Whether one more individual event line may be written (kEventLines).
bool EventLineLocked()
{
    if (g_scan.eventLines >= kEventLines)
        return false;

    if (++g_scan.eventLines == kEventLines)
        LOG_INFO("DLSS-NR exposure scan: {} candidate events logged; from here on only the periodic summary "
                 "counts them",
                 kEventLines);

    return true;
}

// How many references other than the scan's own one hold the resource. The scan's reference keeps the object
// alive, so the probe's Release is never the last one.
unsigned long OthersHolding(ID3D12Resource* resource)
{
    resource->AddRef();
    const unsigned long count = resource->Release();
    return count > 0 ? count - 1 : 0;
}

bool IsFalse(const Tracked& t) { return t.reads >= kFalseAfter && t.inRange == 0; }

bool Untried(const Tracked& t) { return t.reads < kPatience; }

// Whether the device the resource belongs to was removed. Nothing recorded on it can run again, so a reference held
// only until the GPU is done with the scan's copies can go: releasing its objects is what recovering from a removal
// is, and the fences of a removed device never answer (GetCompletedValue reads UINT64_MAX), so waiting for them
// would hold the removed device's objects, and the heaps under them, for the rest of the session.
bool DeviceRemoved(ID3D12Resource* resource)
{
    ID3D12Device* device = nullptr;

    if (resource == nullptr || FAILED(resource->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return false;

    const bool removed = FAILED(device->GetDeviceRemovedReason());
    device->Release();
    return removed;
}

// The resource first, then its heap: never the other way round.
void ReleaseNow(Tracked& t)
{
    if (t.resource != nullptr)
        t.resource->Release();

    if (t.heap != nullptr)
        t.heap->Release();

    t.resource = nullptr;
    t.heap = nullptr;
}

// Under g_scanMutex. Nothing copies `t` from here on; its references go once every recording that copied it has
// settled (at once if none did), or its device was removed (ProbeLocked).
void RetireLocked(Tracked&& t)
{
    if (DlssNr::Submission::Settled(t.usage))
    {
        ReleaseNow(t);
        g_scan.since.released++;
        return;
    }

    g_scan.retiring.push_back(std::move(t));
}

// Under g_scanMutex. The candidate with this id among those still read (active or waiting), or null.
Tracked* HeldLocked(unsigned int id, size_t hint)
{
    if (hint < g_scan.active.size() && g_scan.active[hint].id == id)
        return &g_scan.active[hint];

    for (Tracked& t : g_scan.active)
        if (t.id == id)
            return &t;

    for (Tracked& t : g_scan.waiting)
        if (t.id == id)
            return &t;

    return nullptr;
}

// Under g_scanMutex. Section 3.3: before anything is copied, every held candidate is checked for a holder other
// than the scan. One the game let go of is never copied again. Then the retiring ones whose copies settled go.
void ProbeLocked()
{
    auto probe = [](auto& list)
    {
        for (auto it = list.begin(); it != list.end();)
        {
            it->alive = it->resource != nullptr && OthersHolding(it->resource) > 0;

            if (it->alive)
            {
                ++it;
                continue;
            }

            g_scan.since.letGo++;

            if (EventLineLocked())
                LOG_INFO("DLSS-NR exposure scan: candidate {} ({}) was let go by the game; it is not copied again",
                         it->id, it->shape);

            Tracked t = std::move(*it);
            it = list.erase(it);
            RetireLocked(std::move(t));
        }
    };

    probe(g_scan.active);
    probe(g_scan.waiting);

    for (auto it = g_scan.retiring.begin(); it != g_scan.retiring.end();)
    {
        if (!DlssNr::Submission::Settled(it->usage) && !DeviceRemoved(it->resource))
        {
            ++it;
            continue;
        }

        ReleaseNow(*it);
        g_scan.since.released++;
        it = g_scan.retiring.erase(it);
    }
}

// Under g_scanMutex. One certified reading for `t`.
void CreditLocked(Tracked& t, float value, unsigned long long serial)
{
    const bool newest = t.reads == 0 || serial >= t.latestSerial;
    t.reads++;

    // A non-finite value is a reading too, and not a plausible one: a buffer that only ever holds NaN can be
    // judged false like any other.
    if (!std::isfinite(value))
        return;

    if (newest)
    {
        t.latest = value;
        t.latestSerial = serial;
    }

    if (value <= kFloor || value >= kCeiling)
        return;

    if (t.inRange == 0)
    {
        t.lowest = value;
        t.highest = value;
    }
    else
    {
        t.lowest = std::min(t.lowest, value);
        t.highest = std::max(t.highest, value);
    }

    t.inRange++;

    if (t.inRange > 1 && t.highest > t.lowest * 1.25f)
        t.moves = true;
}

// Under g_scanMutex. Section 3.4: a reading comes from a slot whose recording completed, credited to the id the slot
// recorded at that offset. A slot whose recording settled without completing -- the list was discarded and will
// never run -- is freed unread. Anything else stays pending and is not reused.
void HarvestLocked()
{
    unsigned int order[kSlots];
    unsigned int count = 0;

    for (unsigned int s = 0; s < kSlots; ++s)
        if (g_scan.slots[s].pending)
            order[count++] = s;

    std::sort(order, order + count,
              [](unsigned int a, unsigned int b) { return g_scan.slots[a].serial < g_scan.slots[b].serial; });

    for (unsigned int k = 0; k < count; ++k)
    {
        Slot& slot = g_scan.slots[order[k]];

        if (!DlssNr::Submission::Completed(slot.usage))
        {
            if (DlssNr::Submission::Settled(slot.usage))
                slot.pending = false; // never ran: nothing to read, nothing still writing it

            continue;
        }

        slot.pending = false;

        ID3D12Resource* buffer = g_scan.readback[order[k]];
        void* mapped = nullptr;
        D3D12_RANGE range { 0, kStride * kMaxCandidates };

        if (buffer == nullptr || FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
            continue;

        const unsigned char* base = (const unsigned char*) mapped;

        for (size_t i = 0; i < kMaxCandidates; ++i)
        {
            if (slot.ids[i] == 0)
                continue;

            Tracked* t = HeldLocked(slot.ids[i], i);

            if (t == nullptr)
                continue; // retiring or gone: its value is nobody's now

            const unsigned char* at = base + i * kStride;
            float value = 0.0f;

            if (t->bytes == 2)
            {
                uint16_t half = 0;
                std::memcpy(&half, at, sizeof(half));
                value = HalfToFloat(half);
            }
            else
            {
                std::memcpy(&value, at, sizeof(value));
            }

            CreditLocked(*t, value, slot.serial);
        }

        D3D12_RANGE nothingWritten { 0, 0 };
        buffer->Unmap(0, &nothingWritten);
    }
}

// Under g_scanMutex. Takes the waiting candidate that has been tried least: the oldest untried one, else the oldest.
void PromoteOneLocked()
{
    auto pick = std::find_if(g_scan.waiting.begin(), g_scan.waiting.end(), Untried);

    if (pick == g_scan.waiting.end())
        pick = g_scan.waiting.begin();

    g_scan.active.push_back(std::move(*pick));
    g_scan.waiting.erase(pick);
}

// Under g_scanMutex. Section 4: free places are filled from the waiting list; while something waits and the list is
// full, a false candidate is retired, then a quiet one is moved aside for one that has not been tried. Nothing that
// moves is ever moved aside or retired here.
void RebalanceLocked()
{
    if (g_scan.retiring.size() >= kMaxRetiring)
    {
        if (!g_scan.saidRetiringFull)
        {
            g_scan.saidRetiringFull = true;
            LOG_WARN("DLSS-NR exposure scan: {} let-go candidates are still waiting for the GPU to confirm their "
                     "copies; nothing new is read until it does",
                     g_scan.retiring.size());
        }

        return;
    }

    auto fill = []
    {
        while (g_scan.active.size() < kMaxCandidates && !g_scan.waiting.empty())
            PromoteOneLocked();
    };

    fill();

    while (!g_scan.waiting.empty() && g_scan.active.size() >= kMaxCandidates)
    {
        auto found = std::find_if(g_scan.active.begin(), g_scan.active.end(), IsFalse);

        if (found == g_scan.active.end())
            break;

        g_scan.since.falseOnes++;

        if (EventLineLocked())
            LOG_INFO("DLSS-NR exposure scan: candidate {} ({}) held nothing plausible in {} readings; it gives way "
                     "to one waiting",
                     found->id, found->shape, found->reads);

        Tracked t = std::move(*found);
        g_scan.active.erase(found);
        RetireLocked(std::move(t));
        fill();
    }

    while (g_scan.active.size() >= kMaxCandidates && std::any_of(g_scan.waiting.begin(), g_scan.waiting.end(), Untried))
    {
        auto quiet = g_scan.active.end();

        for (auto it = g_scan.active.begin(); it != g_scan.active.end(); ++it)
        {
            if (!it->moves && it->reads >= kPatience && (quiet == g_scan.active.end() || it->reads > quiet->reads))
                quiet = it;
        }

        if (quiet == g_scan.active.end())
            break;

        g_scan.since.movedAside++;

        if (EventLineLocked())
            LOG_INFO("DLSS-NR exposure scan: candidate {} ({}) has not moved in {} readings; moved aside for one "
                     "not yet tried",
                     quiet->id, quiet->shape, quiet->reads);

        Tracked t = std::move(*quiet);
        g_scan.active.erase(quiet);
        PromoteOneLocked();
        g_scan.waiting.push_back(std::move(t));
    }
}

// Under g_scanMutex. Records one sweep into a free readback slot: each active candidate that is certified to copy
// (probed alive this Tick, tracked into its own usage) is copied to its position's offset, and the slot records whose
// value each offset holds. Returns the problem that stopped it, or null.
const char* SweepLocked(ID3D12GraphicsCommandList* cmdList)
{
    unsigned int chosen = kSlots;

    for (unsigned int k = 0; k < kSlots; ++k)
    {
        const unsigned int s = (g_scan.nextSlot + k) % kSlots;

        if (!g_scan.slots[s].pending)
        {
            chosen = s;
            break;
        }
    }

    if (chosen == kSlots)
    {
        if (!g_scan.saidUnconfirmed)
        {
            g_scan.saidUnconfirmed = true;
            LOG_WARN("DLSS-NR exposure scan: {}", kUnconfirmed);
        }

        return kUnconfirmed;
    }

    Slot& slot = g_scan.slots[chosen];
    ID3D12Resource* dst = g_scan.readback[chosen];

    if (dst == nullptr)
        return nullptr;

    // The previous recording into this slot completed (and was read) or was discarded: none of it is needed now.
    slot.usage = {};

    if (!DlssNr::Submission::Track(cmdList, slot.usage))
    {
        if (!g_scan.saidUntracked)
        {
            g_scan.saidUntracked = true;
            LOG_WARN("DLSS-NR exposure scan: {} ({})", kUntracked, DlssNr::Submission::LastRefusal());
        }

        return kUntracked;
    }

    slot.ids.fill(0);

    // The state a candidate is in is the game's business and nothing here has a contract about it.
    //
    // UNORDERED_ACCESS is the assumption, and it is the reasonable one: every candidate got here by
    // having an unordered access view created on it, which is what a compute shader writes through,
    // and an eye adaptation buffer is written every frame and read by the next pass. It is still an
    // assumption, which is why the whole scan is behind a setting that is off by default -- getting
    // this wrong on someone's machine costs them a frame or a device, and nobody who has not asked
    // for the scan should be exposed to that.
    for (size_t i = 0; i < g_scan.active.size(); ++i)
    {
        Tracked& t = g_scan.active[i];

        if (t.resource == nullptr || !t.alive)
            continue;

        // Into the candidate's own usage before anything of it is recorded: its release waits for this recording.
        // Finished recordings are let go of first; a game that never resets its old lists would otherwise fill the
        // usage, and the candidate would stop being copied.
        DlssNr::Submission::PruneFinished(t.usage);

        if (!DlssNr::Submission::Track(cmdList, t.usage))
            continue;

        Barrier(cmdList, t.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

        if (t.isBuffer)
        {
            cmdList->CopyBufferRegion(dst, i * kStride, t.resource, 0, t.bytes);
        }
        else
        {
            D3D12_TEXTURE_COPY_LOCATION src {};
            src.pResource = t.resource;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION to {};
            to.pResource = dst;
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint.Offset = i * kStride;
            to.PlacedFootprint.Footprint.Format = t.texFormat;
            to.PlacedFootprint.Footprint.Width = 1;
            to.PlacedFootprint.Footprint.Height = 1;
            to.PlacedFootprint.Footprint.Depth = 1;
            to.PlacedFootprint.Footprint.RowPitch = 256;

            D3D12_BOX one { 0, 0, 0, 1, 1, 1 };
            cmdList->CopyTextureRegion(&to, 0, 0, 0, &src, &one);
        }

        Barrier(cmdList, t.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        slot.ids[i] = t.id;
        t.copied = true;
    }

    slot.pending = true;
    slot.serial = ++g_scan.writes;
    g_scan.nextSlot = (chosen + 1) % kSlots;
    return nullptr;
}

// Under g_scanMutex. Barren: nothing moves, a candidate has been read kPatience times, and nothing untried waits.
bool BarrenLocked(bool* outMover = nullptr, bool* outFresh = nullptr)
{
    bool anyMover = false;
    bool anyFresh = false;
    unsigned int mostReads = 0;

    for (const Tracked& t : g_scan.active)
    {
        anyMover |= t.moves;
        anyFresh |= t.reads == 0;
        mostReads = std::max(mostReads, t.reads);
    }

    if (outMover != nullptr)
        *outMover = anyMover;

    if (outFresh != nullptr)
        *outFresh = anyFresh;

    return !anyMover && mostReads >= kPatience && std::none_of(g_scan.waiting.begin(), g_scan.waiting.end(), Untried);
}

// Under g_scanMutex. The moving candidate with the widest travel among those still read, or null.
const Tracked* BestLocked()
{
    const Tracked* best = nullptr;
    float bestRatio = 0.0f;

    auto consider = [&](const Tracked& t)
    {
        if (!t.moves || t.lowest <= kFloor)
            return;

        const float ratio = t.highest / t.lowest;

        if (ratio > bestRatio)
        {
            bestRatio = ratio;
            best = &t;
        }
    };

    for (const Tracked& t : g_scan.active)
        consider(t);

    for (const Tracked& t : g_scan.waiting)
        consider(t);

    return best;
}

} // namespace

void SetResourceHooksAttached(bool attached) { g_resourceHooksAttached.store(attached, std::memory_order_relaxed); }

bool ResourceHooksAttached() { return g_resourceHooksAttached.load(std::memory_order_relaxed); }

// Everything the two entry points share: does this description look like a number rather than a
// picture, and if so what is it.
//
// Widened from the first attempt, which asked for at most 4x4 and one or two channels and found
// nothing anywhere. That was tuned on what an exposure buffer ought to look like rather than on what
// engines actually allocate: some keep a small histogram beside the value, some keep a few frames of
// history, and some put the whole thing in a four-channel texture and use one channel. The filter
// only has to be tight enough that the list stays readable.
bool LooksLikeANumber(const D3D12_RESOURCE_DESC& rd, std::string* outShape, unsigned int* outBytes, bool* outIsBuffer,
                      DXGI_FORMAT* outFormat)
{
    // An exposure is computed, so it is written by a shader. This is the one condition worth being
    // strict about: it removes almost everything without removing anything that could be the answer.
    if ((rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
        return false;

    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        // 256 texels rather than 16. A 16x16 texture is still a number by any reasonable measure and
        // a 256-bin histogram is exactly how a lot of engines compute one.
        if (rd.Width * rd.Height > 256 || rd.Width == 0 || rd.Height == 0)
            return false;

        const char* name = nullptr;

        if (!PlausibleFormat(rd.Format, outBytes, &name, outFormat))
            return false;

        *outIsBuffer = false;
        *outShape = std::to_string((unsigned int) rd.Width) + "x" + std::to_string(rd.Height) + " " + name;
        return true;
    }

    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        *outFormat = DXGI_FORMAT_UNKNOWN;

        // Unreal moved eye adaptation off a texture and onto a buffer, so buffers have to be in
        // scope or a whole engine's worth of games is invisible.
        //
        // 128 bytes, down from 4kB. The wider bound filled all twelve slots in Nioh 3 with 256, 512
        // and 768 byte buffers that never held anything but zero, and the real answer -- eight bytes
        // -- only made the list because it happened to be created early. A cap that can be filled by
        // junk is a cap that can hide the answer.
        if (rd.Width == 0 || rd.Width > 128)
            return false;

        *outIsBuffer = true;
        *outBytes = 4;
        *outShape = "buffer, " + std::to_string((unsigned int) rd.Width) + " bytes";
        return true;
    }

    return false;
}

// Under g_scanMutex. Section 4, admission: active while there is room, else waiting; a full waiting list gives up its
// oldest never-copied candidate, and refuses the newcomer only if every waiting one has been copied before.
void Adopt(ID3D12Resource* resource, const std::string& shape, unsigned int bytes, bool isBuffer, DXGI_FORMAT texFormat,
           Origin origin, ID3D12Heap* heap)
{
    // Adopting holds a reference, and that is only worth its risk while the scan is wanted. Held whenever
    // NR was on, Cyberpunk 2077 with DLSS Frame Generation had 64 references to small buffers Streamline
    // and the game create, recycle and free every few frames, and removed the device (0x887A0006) within
    // seconds of NR starting -- the pinned-wrapper hazard ReleaseTrackedResources describes, taken on for
    // a scan nobody had chosen. The cost is the case NoteUav was ungated for: an engine that creates its
    // exposure once at start-up is not seen by a scan chosen later, and the game has to be restarted with
    // it selected. Tick says so when that is what happened.
    if (!Wanted())
    {
        if (g_scan.passedOver++ == 0)
            LOG_INFO("DLSS-NR exposure scan: candidates are not kept while the scan is not the white point's "
                     "source; choose it and restart the game if it then finds nothing");
        return;
    }

    // One resource, one candidate. A held resource is alive (our reference), so its address cannot have been
    // handed to a new one: the same pointer here is the same resource seen again (its view after its creation).
    auto held = [resource](const Tracked& t) { return t.resource == resource; };

    if (std::any_of(g_scan.active.begin(), g_scan.active.end(), held) ||
        std::any_of(g_scan.waiting.begin(), g_scan.waiting.end(), held) ||
        std::any_of(g_scan.retiring.begin(), g_scan.retiring.end(), held))
        return;

    // Past the retiring bound nothing new is copied (RebalanceLocked), so a newcomer waits even with a place free.
    const bool toActive = g_scan.active.size() < kMaxCandidates && g_scan.retiring.size() < kMaxRetiring;

    if (!toActive && g_scan.waiting.size() >= kMaxWaiting)
    {
        auto oldest =
            std::find_if(g_scan.waiting.begin(), g_scan.waiting.end(), [](const Tracked& t) { return !t.copied; });

        if (oldest == g_scan.waiting.end())
        {
            g_scan.since.refused++;

            if (!g_scan.complained)
            {
                g_scan.complained = true;
                LOG_WARN("DLSS-NR exposure scan: {} candidates read and {} waiting, all of them read before; a new "
                         "one is refused, so the filter is too loose here rather than the game having that many "
                         "exposures",
                         kMaxCandidates, kMaxWaiting);
            }

            return;
        }

        // Never copied, so nothing on the GPU can still be reading it.
        g_scan.since.dropped++;

        if (EventLineLocked())
            LOG_INFO("DLSS-NR exposure scan: candidate {} ({}) waited longest and gives its place to a new one",
                     oldest->id, oldest->shape);

        ReleaseNow(*oldest);
        g_scan.since.released++;
        g_scan.waiting.erase(oldest);
    }

    Tracked t;
    t.resource = resource;
    t.heap = heap;
    t.origin = origin;
    t.id = ++g_scan.nextId;
    t.shape = shape;
    t.isBuffer = isBuffer;
    t.bytes = bytes;
    t.texFormat = texFormat;
    resource->AddRef();

    if (heap != nullptr)
        heap->AddRef();

    g_scan.since.adopted++;

    if (EventLineLocked())
        LOG_INFO("DLSS-NR exposure scan: candidate {} -- {} ({}){}", t.id, shape, OriginName(origin),
                 toActive ? "" : ", waiting for a place");

    if (toActive)
        g_scan.active.push_back(std::move(t));
    else
        g_scan.waiting.push_back(std::move(t));
}

void NoteResource(const D3D12_RESOURCE_DESC* desc, ID3D12Resource* resource, ID3D12Heap* heap)
{
    if (!Config::Instance()->DlssNrEnabled.value_or_default())
        return;

    // Not the game's: FFX's own buffers (frame generation, upscaling) come through the same device. An
    // 8-byte FSR-FG buffer was adopted as the exposure in PCSX2 on D3D12 and wrecked the picture.
    if (ScopedInternalResourceCreation::Active())
        return;

    if (desc == nullptr || resource == nullptr)
        return;

    std::string shape;
    unsigned int bytes = 4;
    bool isBuffer = false;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;

    std::lock_guard<std::mutex> lock(g_scanMutex);
    g_scan.examined++;

    if (!LooksLikeANumber(*desc, &shape, &bytes, &isBuffer, &fmt))
    {
        // Near-miss diagnostic. A resource a shader writes (UAV) that the filter rejected: logging its
        // shape -- bounded to the first 40 so it cannot flood -- reveals whether a game the scan finds
        // nothing in (Cyberpunk 2077) has an exposure the filter narrowly misses (a small UAV texture
        // in an unlisted format, or a UAV buffer just over 128 bytes -> widen precisely to match) or
        // nothing scannable at all (only large buffers/textures -> the exposure is baked in a bigger
        // buffer and no filter change can help). Read these against Examined() in the log.
        if ((desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 && g_scan.nearMissLogged < 40)
        {
            g_scan.nearMissLogged++;
            LOG_INFO("DLSS-NR scan near-miss #{}: UAV dim {} {}x{}x{} fmt {} (filter rejected)", g_scan.nearMissLogged,
                     (int) desc->Dimension, (unsigned int) desc->Width, desc->Height, desc->DepthOrArraySize,
                     (int) desc->Format);
        }

        return;
    }

    Adopt(resource, shape, bytes, isBuffer, fmt, heap != nullptr ? Origin::Placed : Origin::Committed, heap);
}

unsigned int Examined()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);
    return g_scan.examined;
}

void NoteUav(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc)
{
    // Views OptiScaler's own backends create (FFX frame generation and upscaling) describe resources
    // that are not the game's, and are never an exposure. See NoteResource.
    if (ScopedInternalResourceCreation::Active())
        return;

    // Not gated on the scan setting here; Adopt is, since 2026-09-29.
    //
    // An engine creates its eye adaptation view once, when it builds its render targets, which is
    // long before anybody opens a menu and ticks a box. Gating the recording meant every candidate
    // was thrown away before the scan could want it, and the readout then said "nothing matched
    // yet -- play for a few seconds", which is advice that could never come true no matter how long
    // anyone played.
    //
    // Recording was taken to be a resource description and a pointer, but the pointer is a reference
    // held on a resource the game owns, and holding dozens of them for a scan nobody chose is what
    // Cyberpunk under DLSS-G removed the device over. So the pass-over now happens in Adopt, and the
    // readout says to restart rather than to keep playing.
    if (!Config::Instance()->DlssNrEnabled.value_or_default())
        return;

    if (resource == nullptr)
        return;

    const D3D12_RESOURCE_DESC rd = resource->GetDesc();

    std::string shape;
    unsigned int bytes = 4;
    bool isBuffer = false;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;

    std::lock_guard<std::mutex> lock(g_scanMutex);
    g_scan.examined++;

    if (!LooksLikeANumber(rd, &shape, &bytes, &isBuffer, &fmt))
        return;

    // A resource whose creation the scan saw is already held, as committed or placed; this one was made some way
    // the creation hooks do not see, so where its memory lives is unknown.
    Adopt(resource, shape, bytes, isBuffer, fmt, Origin::View, nullptr);
}

void Tick(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
{
    if (!Wanted())
        return;

    if (BlindSinceStart())
        return;

    if (device == nullptr || cmdList == nullptr)
        return;

    // Before the lock, deliberately. EnsureReadback creates a committed resource, and that call is
    // detoured to hkCreateCommittedResource -> NoteResource, which takes g_scanMutex. Holding it
    // here would be a self-deadlock on a non-recursive mutex the first frame the scan runs. The
    // readback ring is only ever touched on this (render) thread, so it needs no lock of its own.
    if (!EnsureReadback(device))
        return;

    std::lock_guard<std::mutex> lock(g_scanMutex);

    // In this order, every frame: who is still held by the game, what the GPU has confirmed, who takes which place.
    // Nothing is copied that this Tick did not see alive.
    ProbeLocked();
    HarvestLocked();
    RebalanceLocked();

    if (g_scan.active.empty() && g_scan.waiting.empty())
    {
        g_scan.status = g_scan.passedOver != 0
                            ? "nothing kept yet: exposures created before the scan was chosen were not kept -- "
                              "restart the game with the scan selected"
                            : "no buffer in this game is shaped like an exposure";
        return;
    }

    // Throttle the sweep once it is "settled". The per-frame sweep -- up to kMaxCandidates x {2
    // transition barriers + 1 copy} spliced onto the GAME's command list plus a CPU readback --
    // exists to discover which buffer is the exposure and, after that, to keep feeding
    // the white point a fresh value. It is settled in either of two ways: a mover has been found (the
    // exposure is tracking the light, so the white point just needs a fresh value every few frames --
    // an auto-exposure eases over many frames, so the last read value is fine between sweeps), or the
    // scan is barren (BarrenLocked: watched for kPatience reads with nothing moving and nothing untried
    // waiting -- the exposure is not in this game, e.g. KCD2 computes one but never hands it to the
    // upscaler, so BestValue returns 0 and the sweep's value is never consumed). Until settled the
    // sweep runs every frame, so a mover is found as fast as the unthrottled code.
    bool anyMover = false;
    bool anyFresh = false;
    const bool barren = BarrenLocked(&anyMover, &anyFresh);
    const bool settled = anyMover || barren;

    bool doSweep;

    if (!settled)
        doSweep = true; // discovery: every frame, as fast as the unthrottled code
    else if (anyMover)
        doSweep = (g_scan.frames % kScanThrottle == 0); // feed the white point a fresh value
    else
        // Barren: the value goes nowhere, so the sweep only watches for a new candidate (anyFresh --
        // sampled next frame, then back to the slow cadence) or a quiet buffer that starts to move.
        doSweep = anyFresh || (g_scan.frames % kBarrenSweepInterval == 0);

    if (barren && !g_scan.barrenLogged)
    {
        g_scan.barrenLogged = true;
        LOG_INFO("DLSS-NR exposure scan: nothing moved in {} sweeps; nothing to feed, so the sweep drops "
                 "to every {} frames (new buffers are still picked up)",
                 kPatience, kBarrenSweepInterval);
    }

    const char* problem = doSweep ? SweepLocked(cmdList) : nullptr;

    // Periodic movement readout. The menu's Advanced panel shows which candidate tracks the light, but
    // the log did not -- so a game the scan is being taught (Cyberpunk) could not be cracked from a log
    // alone. Every ~300 frames, name the candidates that MOVE and their travel: the exposure is the one
    // that swings widely between bright and dark. Throttled, and only while the scan is wanted.
    if (g_scan.frames > 0 && g_scan.frames % 300 == 0)
    {
        unsigned int movers = 0;

        for (const Tracked& t : g_scan.active)
        {
            if (!t.moves)
                continue;

            movers++;
            LOG_INFO("DLSS-NR scan mover: candidate {} ({}) range {:.5f}..{:.5f} (x{:.1f}), latest {:.5f}", t.id,
                     t.shape, t.lowest, t.highest, t.lowest > kFloor ? t.highest / t.lowest : 0.0f, t.latest);
        }

        if (movers == 0)
            LOG_INFO("DLSS-NR scan: {} candidates tracked, none moving yet -- go between bright and dark",
                     (unsigned int) g_scan.active.size());

        // Where the candidates are and what happened to them since the last summary (section 7).
        const Counts& c = g_scan.since;

        if (c.Any() || !g_scan.waiting.empty() || !g_scan.retiring.empty())
            LOG_INFO("DLSS-NR scan: {} read, {} waiting, {} retiring; since the last summary {} adopted, {} let go "
                     "by the game, {} held nothing plausible, {} moved aside, {} dropped, {} refused, {} released",
                     (unsigned int) g_scan.active.size(), (unsigned int) g_scan.waiting.size(),
                     (unsigned int) g_scan.retiring.size(), c.adopted, c.letGo, c.falseOnes, c.movedAside, c.dropped,
                     c.refused, c.released);

        g_scan.since = {};
    }

    g_scan.frames++;
    g_scan.status = problem != nullptr ? problem : "";
}

Verdict Where()
{
    if (!Wanted())
        return Verdict::Off;

    if (BlindSinceStart())
        return Verdict::NeedsRestart;

    std::lock_guard<std::mutex> lock(g_scanMutex);

    if (g_scan.active.empty() && g_scan.waiting.empty())
        return Verdict::Waiting;

    bool anyMover = false;
    const bool barren = BarrenLocked(&anyMover);

    if (anyMover)
        return Verdict::Found;

    return barren ? Verdict::Barren : Verdict::Watching;
}

const char* Headline()
{
    static std::string line;

    switch (Where())
    {
    case Verdict::Off:
        line = "";
        break;

    case Verdict::NeedsRestart:
        line = kNeedsRestart;
        break;

    case Verdict::Waiting:
    {
        // The examined count is the whole diagnosis. Zero means the hook is not running and no
        // amount of playing will change that; a large number means the game genuinely has nothing
        // shaped like an exposure, which is an answer rather than a failure.
        const unsigned int seen = Examined();
        line = seen == 0 ? "DLSS-NR exposure scan: NOT RUNNING -- no resources seen at all"
                         : "DLSS-NR exposure scan: examined " + std::to_string(seen) +
                               " resources, none shaped like an exposure";
        break;
    }

    case Verdict::Watching:
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);
        unsigned int mostReads = 0;

        for (const Tracked& t : g_scan.active)
            mostReads = std::max(mostReads, t.reads);

        const std::string waiting =
            g_scan.waiting.empty() ? "" : " (+" + std::to_string(g_scan.waiting.size()) + " waiting)";

        line = "DLSS-NR exposure scan: watching " + std::to_string(g_scan.active.size()) + waiting +
               ", none moving yet -- walk between light and shade  (" +
               std::to_string(std::min(mostReads, kPatience) * 100 / kPatience) + "%)";
        break;
    }

    case Verdict::Found:
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);

        // The widest travel wins where several move. An exposure swings by orders of magnitude
        // between a dark interior and open daylight; anything that merely wobbles is something else.
        const Tracked* best = BestLocked();

        if (best == nullptr)
        {
            line = "DLSS-NR exposure scan: FOUND";
            break;
        }

        char buf[192];
        // The live value is in here so the line visibly ticks. Without it the indicator looks stuck
        // the moment the range settles, which is exactly when it has succeeded.
        snprintf(buf, sizeof(buf), "DLSS-NR exposure scan: FOUND -- candidate %u = %.5f  (%.5f..%.5f, x%.0f)  done",
                 best->id, best->latest, best->lowest, best->highest, best->highest / best->lowest);
        line = buf;
        break;
    }

    case Verdict::Barren:
        line = "DLSS-NR exposure scan: nothing moved. No exposure to find here.";
        break;
    }

    return line.c_str();
}

float BestValue(int* outIndex, float* outLowest, float* outHighest)
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    const Tracked* best = BestLocked();

    if (best == nullptr)
        return 0.0f;

    if (outIndex != nullptr)
        *outIndex = (int) best->id;

    if (outLowest != nullptr)
        *outLowest = best->lowest;

    if (outHighest != nullptr)
        *outHighest = best->highest;

    return best->latest;
}

std::vector<Candidate> Report()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    std::vector<Candidate> out;
    out.reserve(g_scan.active.size());

    for (const Tracked& t : g_scan.active)
    {
        Candidate c;
        c.id = t.id;
        c.shape = t.shape;
        c.latest = t.latest;
        c.lowest = t.lowest;
        c.highest = t.highest;
        c.reads = t.reads;
        c.moves = t.moves;
        out.push_back(c);
    }

    return out;
}

const char* Status()
{
    if (Wanted() && BlindSinceStart())
        return kNeedsRestart;

    std::lock_guard<std::mutex> lock(g_scanMutex);
    return g_scan.status;
}

bool Scanning() { return Wanted(); }

// ---------------------------------------------------------------------------------------------
// Multi-point anchoring
// ---------------------------------------------------------------------------------------------

namespace
{
std::vector<AnchorPoint> g_anchors; // guarded by g_scanMutex, kept sorted by scan ascending

void SortAnchorsLocked()
{
    std::sort(g_anchors.begin(), g_anchors.end(),
              [](const AnchorPoint& a, const AnchorPoint& b) { return a.scan < b.scan; });
}
} // namespace

// Load the persisted table (or migrate a pre-existing single anchor) exactly once, before any lock
// is taken -- LoadAnchors/AnchorAdd take g_scanMutex themselves, so this must not hold it.
void EnsureAnchorsLoaded()
{
    static std::once_flag once;
    std::call_once(once,
                   []
                   {
                       auto& cfg = *Config::Instance();
                       const std::string ser = cfg.DlssNrScanAnchors.value_or_default();

                       if (!ser.empty())
                       {
                           LoadAnchors(ser);
                           return;
                       }

                       // Migration: fold a single-anchor ini from before this feature into a one-row table so the
                       // user does not lose the calibration they already set.
                       const float v = cfg.DlssNrScanAnchorValue.value_or_default();
                       const float w = cfg.DlssNrScanAnchorWhitePoint.value_or_default();

                       if (v > kFloor && w > 1e-6f)
                           AnchorAdd(v, w);
                   });
}

std::vector<AnchorPoint> Anchors()
{
    EnsureAnchorsLoaded();
    std::lock_guard<std::mutex> lock(g_scanMutex);
    return g_anchors;
}

bool AnchorAdd(float scan, float white)
{
    if (!(scan > kFloor && scan < kCeiling) || !(white > 1e-6f))
        return false;

    std::lock_guard<std::mutex> lock(g_scanMutex);

    // A near-duplicate scan value would make log(v_{k+1}) - log(v_k) ~ 0 and divide the interpolation
    // by zero. Replace the existing point's white instead of adding a second at the same place.
    for (auto& p : g_anchors)
    {
        if (scan > p.scan * 0.98f && scan < p.scan * 1.02f)
        {
            p.white = white;
            return true;
        }
    }

    if (g_anchors.size() >= 8)
        return false;

    g_anchors.push_back({ scan, white });
    SortAnchorsLocked();
    return true;
}

void AnchorSetWhite(int index, float white)
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    if (index >= 0 && index < (int) g_anchors.size() && white > 1e-6f)
        g_anchors[index].white = white;
}

void AnchorRemove(int index)
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    if (index >= 0 && index < (int) g_anchors.size())
        g_anchors.erase(g_anchors.begin() + index);
}

void AnchorClear()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);
    g_anchors.clear();
}

float AnchoredWhitePoint(float scanNow, bool inverted, float trim)
{
    EnsureAnchorsLoaded();
    trim = std::clamp(trim, 0.01f, 4.0f);

    std::lock_guard<std::mutex> lock(g_scanMutex);

    if (g_anchors.empty() || !(scanNow > kFloor))
        return 0.0f;

    // One point is the original ratio law, and it needs the direction the two-point case reads off
    // the data instead.
    if (g_anchors.size() == 1)
    {
        const AnchorPoint& p = g_anchors[0];

        if (!(p.scan > kFloor) || !(p.white > 1e-6f))
            return 0.0f;

        const float ratio = inverted ? scanNow / p.scan : p.scan / scanNow;
        return std::clamp(p.white * ratio * trim, 0.01f, 4096.0f);
    }

    // Clamp at the ends: holding the outermost calibrated white point beats extrapolating a ratio
    // into a scene darker or brighter than anything the user calibrated.
    if (scanNow <= g_anchors.front().scan)
        return std::clamp(g_anchors.front().white * trim, 0.01f, 4096.0f);

    if (scanNow >= g_anchors.back().scan)
        return std::clamp(g_anchors.back().white * trim, 0.01f, 4096.0f);

    // Between two points: interpolate log(white) against log(scan). A white point is a divisor, so
    // log is the space in which it is linear -- and it makes the two-point case reproduce the exact
    // ratio law of the single-point case, so adding a second point never causes a jump.
    for (size_t k = 0; k + 1 < g_anchors.size(); ++k)
    {
        const AnchorPoint& a = g_anchors[k];
        const AnchorPoint& b = g_anchors[k + 1];

        if (scanNow >= a.scan && scanNow <= b.scan && b.scan > a.scan * 1.0001f)
        {
            const float t = (std::log(scanNow) - std::log(a.scan)) / (std::log(b.scan) - std::log(a.scan));
            const float w = std::exp(std::log(a.white) + t * (std::log(b.white) - std::log(a.white)));
            return std::clamp(w * trim, 0.01f, 4096.0f);
        }
    }

    return std::clamp(g_anchors.back().white * trim, 0.01f, 4096.0f);
}

void LoadAnchors(const std::string& serialized)
{
    std::vector<AnchorPoint> parsed;
    size_t i = 0;

    while (i < serialized.size() && parsed.size() < 8)
    {
        size_t semi = serialized.find(';', i);
        std::string tok = serialized.substr(i, semi == std::string::npos ? std::string::npos : semi - i);
        i = semi == std::string::npos ? serialized.size() : semi + 1;

        const size_t colon = tok.find(':');
        if (colon == std::string::npos)
            continue;

        try
        {
            const float v = std::stof(tok.substr(0, colon));
            const float w = std::stof(tok.substr(colon + 1));

            if (v > kFloor && v < kCeiling && w > 1e-6f)
                parsed.push_back({ v, w });
        }
        catch (...)
        {
            // A malformed token is skipped rather than aborting the whole table.
        }
    }

    std::lock_guard<std::mutex> lock(g_scanMutex);
    g_anchors = std::move(parsed);
    SortAnchorsLocked();
}

std::string SerializeAnchors()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    std::string out;
    char buf[64];

    for (const AnchorPoint& p : g_anchors)
    {
        snprintf(buf, sizeof(buf), "%.6g:%.6g;", p.scan, p.white);
        out += buf;
    }

    return out;
}

// A feature is being released: the boundary at the top of NVSDK_NGX_D3D12_ReleaseFeature, before anything the
// feature owns is freed. docs/dlssnr/design/exposure-scan-capacity.md, section 5.
//
// This used to release and forget every candidate, which is what stopped the use-after-free that removed the device
// in Cyberpunk (70f17250): a Streamline/DLSS-D resource the scan held outlived the placed heap its owner freed at
// teardown, and the scan went on copying it. That guarantee is now given by other means for everything whose memory
// the scan can keep alive, and kept by the old means for the rest:
//
// - A candidate seen only through a view (Origin::View) has a heap the scan does not know, so it is released here,
//   while that heap still exists, exactly as before. So is any candidate whose device was removed (DeviceRemoved).
// - A committed candidate owns its memory, and a placed one's heap is held by the scan alongside it, so keeping the
//   reference cannot leave either pointing at freed memory. Neither is copied again until a Tick has probed it, and
//   every Tick probes before it records: one the teardown let go of is found let go at the next frame, never copied
//   after it, and released once its earlier copies settle. One the game still holds stays watched -- a game that
//   keeps its exposure across a DLSS quality change keeps it in the scan, and one that re-created it just before
//   (Cyberpunk, 0.25 s before the release) has the new one waiting for a place the let-go old ones free.
//
// Our own readback ring is not touched here: it may have GPU copies in flight. Nothing is held unless the scan is
// wanted (Adopt), so with the scan off, or NR off, this finds nothing to do.
void ReleaseTrackedResources()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    unsigned int kept = 0;
    unsigned int released = 0;

    auto boundary = [&](auto& list)
    {
        for (auto it = list.begin(); it != list.end();)
        {
            if (it->origin != Origin::View && !DeviceRemoved(it->resource))
            {
                it->alive = false; // re-checked before anything copies it again
                ++kept;
                ++it;
                continue;
            }

            ReleaseNow(*it);
            ++released;
            it = list.erase(it);
        }
    };

    boundary(g_scan.active);
    boundary(g_scan.waiting);
    boundary(g_scan.retiring);

    g_scan.since.released += released;
    g_scan.complained = false;
    g_scan.barrenLogged = false;

    if (kept != 0 || released != 0)
        LOG_INFO("DLSS-NR exposure scan: a feature is being released; {} candidate(s) kept and re-checked before "
                 "the next copy, {} seen only through a view or on a removed device released now",
                 kept, released);
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    for (Tracked& t : g_scan.active)
        ReleaseNow(t);

    for (Tracked& t : g_scan.waiting)
        ReleaseNow(t);

    for (Tracked& t : g_scan.retiring)
        ReleaseNow(t);

    g_scan.active.clear();
    g_scan.waiting.clear();
    g_scan.retiring.clear();

    for (unsigned int i = 0; i < kSlots; ++i)
    {
        if (g_scan.readback[i] != nullptr)
        {
            g_scan.readback[i]->Release();
            g_scan.readback[i] = nullptr;
        }

        g_scan.slots[i] = Slot {};
    }

    g_scan.nextSlot = 0;
    g_scan.frames = 0;
    g_scan.writes = 0;
    g_scan.barrenLogged = false;
    g_scan.status = "not started";
}

} // namespace ExposureScan
} // namespace DlssNr
