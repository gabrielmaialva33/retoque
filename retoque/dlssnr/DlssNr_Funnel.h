#pragma once

// The NR engagement funnel (docs/dlssnr/design/engagement-funnel.md).
//
// "Is NR doing anything in this game?" had no one answer. The panel's status line says whether the pass ran lately,
// the coverage log line counts calls per stage every five seconds, and every early return of the pass said why once
// (ReportSkipOnce) and then fell silent. A session where the pass ran on one evaluate in a hundred looked, from all of
// them, like a session where it ran on every one.
//
// So each route keeps a ladder whose rungs are a partition, after RenoDX-DLSS5-Generic's funnel and decline table
// (PEQHUB, MIT): every evaluate that reaches the route with NR on is eligible, and ends exactly one way, either the
// pass ran or one named decline. The balance
//
//     eligible - ran - sum(declines) = unaccounted
//
// is zero once nothing is in flight. An unaccounted evaluate is a way out of the pass that names no reason, the bug
// this exists to show (tests/nr-funnel holds the balance; its source guard holds that every return of the route names
// one decline).
//
// Cost: two relaxed-or-release atomic increments and three relaxed stores per evaluate, with NR on only, and no lock.
// The counters are constant-initialised and allocate nothing, so the crash note (DlssNr_CrashNote.h) reads them from
// an unhandled-exception filter.
//
// Routes: the D3D12 pass after the upscaler is counted (it carries the bridges' after-upscale passes too, which call
// EvaluateAfterUpscale). The present hosts, the D3D11 bridge's present host and the Vulkan routes are not counted yet;
// the Route enum is where they go.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace DlssNr::Funnel
{
enum class Route : uint8_t
{
    D3d12AfterUpscale, // EvaluateAfterUpscale: native D3D12, and the D3D11/Vulkan-on-D3D12 bridges' after-upscale pass
    Count
};
inline constexpr size_t kRouteCount = static_cast<size_t>(Route::Count);

// Every way the D3D12 after-upscale pass ends without recording, in the order the pass meets them. Append only: the
// tags appear in logs and pasted reports.
enum class Decline : uint8_t
{
    RayReconstructionOff, // the evaluate is ray reconstruction and ApplyAfterRR is off
    NoListOrParameters,   // no command list or no parameter block
    Stage1BeforeUpscaler, // Stage=1 and the pre-upscale scope ran the model on this evaluate
    MissingResources,     // no output, depth or motion vectors in the parameters
    DeferredToPresent,    // the present-time pass takes the frame (HookMethod, PresentBoundary)
    MultiView,            // several views in one colour allocation
    NoDevice,             // the command list belongs to no D3D12 device
    PassUnavailable,      // the composition could not be built on this device, or its inputs are on another
    PassRecordedNothing,  // the pass was asked and recorded nothing: building, failed, paused, a refused frame
    GameOwnNr,            // the game runs its own NR through Streamline (DlssNr_SlNativeNr.h)
    Count
};
inline constexpr size_t kDeclineCount = static_cast<size_t>(Decline::Count);

// Where NR last was, on which thread. One word, so a crash handler reads a consistent triple.
enum class Stage : uint8_t
{
    None,      // nothing recorded yet
    Entered,   // inside the route, before the pass records
    Recording, // inside the pass's Dispatch: the model, the composition
    Left,      // the route returned
};

struct StageMark
{
    Route route = Route::D3d12AfterUpscale;
    Stage stage = Stage::None;
    uint32_t thread = 0;
};

inline const char* RouteName(Route route)
{
    switch (route)
    {
    case Route::D3d12AfterUpscale:
        return "D3D12 after the upscaler";
    default:
        return "unknown route";
    }
}

inline const char* RouteTag(Route route)
{
    switch (route)
    {
    case Route::D3d12AfterUpscale:
        return "d3d12_after_upscale";
    default:
        return "unknown";
    }
}

// Short, stable, [a-z0-9_]: the crash note's and the log's.
inline const char* DeclineTag(Decline decline)
{
    switch (decline)
    {
    case Decline::RayReconstructionOff:
        return "rr_without_apply_after_rr";
    case Decline::NoListOrParameters:
        return "no_list_or_parameters";
    case Decline::Stage1BeforeUpscaler:
        return "stage1_before_upscaler";
    case Decline::MissingResources:
        return "missing_output_depth_or_motion";
    case Decline::DeferredToPresent:
        return "deferred_to_present";
    case Decline::MultiView:
        return "multi_view";
    case Decline::NoDevice:
        return "no_device";
    case Decline::PassUnavailable:
        return "pass_unavailable";
    case Decline::PassRecordedNothing:
        return "pass_recorded_nothing";
    case Decline::GameOwnNr:
        return "game_own_nr";
    default:
        return "unknown";
    }
}

// The diagnostic report's.
inline const char* DeclineText(Decline decline)
{
    switch (decline)
    {
    case Decline::RayReconstructionOff:
        return "ray reconstruction, ApplyAfterRR off";
    case Decline::NoListOrParameters:
        return "no command list or parameter block";
    case Decline::Stage1BeforeUpscaler:
        return "stage 1: the model ran before the upscaler";
    case Decline::MissingResources:
        return "no output, depth or motion vectors";
    case Decline::DeferredToPresent:
        return "deferred to the present-time pass";
    case Decline::MultiView:
        return "several views in one colour allocation";
    case Decline::NoDevice:
        return "the command list has no D3D12 device";
    case Decline::PassUnavailable:
        return "the pass could not be built on this device";
    case Decline::PassRecordedNothing:
        return "the pass recorded nothing";
    case Decline::GameOwnNr:
        return "the game runs its own NR (Streamline)";
    default:
        return "unknown";
    }
}

inline const char* StageText(Stage stage)
{
    switch (stage)
    {
    case Stage::Entered:
        return "entered the route";
    case Stage::Recording:
        return "recording the pass";
    case Stage::Left:
        return "left the pass";
    default:
        return "none recorded";
    }
}

// One read of a ladder.
struct Snapshot
{
    uint64_t eligible = 0;
    uint64_t ran = 0;        // the pass recorded
    uint64_t modelCalls = 0; // of those, the model was called (a carried cadence frame records without it)
    uint64_t declines[kDeclineCount] {};
    const char* lastPassReason = nullptr; // the pass's own reason for its last PassRecordedNothing; a literal

    uint64_t Declined() const
    {
        uint64_t sum = 0;
        for (const auto count : declines)
            sum += count;
        return sum;
    }
    uint64_t Accounted() const { return ran + Declined(); }
    // Never negative from Read; signed so a report shows an impossible count as it is.
    int64_t Unaccounted() const { return static_cast<int64_t>(eligible) - static_cast<int64_t>(Accounted()); }
    bool Balanced() const { return Unaccounted() == 0; }

    bool TopDecline(Decline* which, uint64_t* count) const
    {
        size_t top = kDeclineCount;
        for (size_t i = 0; i < kDeclineCount; ++i)
            if (declines[i] != 0 && (top == kDeclineCount || declines[i] > declines[top]))
                top = i;
        if (count != nullptr)
            *count = top == kDeclineCount ? 0 : declines[top];
        if (top == kDeclineCount)
            return false;
        if (which != nullptr)
            *which = static_cast<Decline>(top);
        return true;
    }
};

// One route's counters. An evaluate is counted eligible when it enters and once more, by its outcome, when it ends.
// Outcomes are released and read acquired before eligible, so a reader on another thread never sees an outcome
// without its entry: Unaccounted() is never negative, and is the number in flight plus any silent way out.
class Ladder
{
    std::atomic<uint64_t> _eligible { 0 };
    std::atomic<uint64_t> _ran { 0 };
    std::atomic<uint64_t> _modelCalls { 0 };
    std::atomic<uint64_t> _declines[kDeclineCount] {};
    std::atomic<const char*> _lastPassReason { nullptr };

  public:
    constexpr Ladder() = default;
    Ladder(const Ladder&) = delete;
    Ladder& operator=(const Ladder&) = delete;

    void Enter() { _eligible.fetch_add(1, std::memory_order_relaxed); }

    void Ran(bool modelCalled)
    {
        if (modelCalled)
            _modelCalls.fetch_add(1, std::memory_order_relaxed);
        _ran.fetch_add(1, std::memory_order_release);
    }

    // detail, when given, is a string literal (it is read after the caller returns, and from a crash handler).
    void Declined(Decline why, const char* detail)
    {
        const auto index = static_cast<size_t>(why);
        if (index >= kDeclineCount)
            return;
        if (detail != nullptr)
            _lastPassReason.store(detail, std::memory_order_relaxed);
        _declines[index].fetch_add(1, std::memory_order_release);
    }

    Snapshot Read() const
    {
        Snapshot s;
        s.ran = _ran.load(std::memory_order_acquire);
        for (size_t i = 0; i < kDeclineCount; ++i)
            s.declines[i] = _declines[i].load(std::memory_order_acquire);
        s.modelCalls = _modelCalls.load(std::memory_order_relaxed);
        s.lastPassReason = _lastPassReason.load(std::memory_order_relaxed);
        s.eligible = _eligible.load(std::memory_order_relaxed);
        return s;
    }
};

namespace Detail
{
inline Ladder g_ladders[kRouteCount];

// route << 40 | stage << 32 | thread: one store, one load.
inline std::atomic<uint64_t> g_lastStage { 0 };

inline uint64_t PackStage(Route route, Stage stage, uint32_t thread)
{
    return static_cast<uint64_t>(route) << 40 | static_cast<uint64_t>(stage) << 32 | thread;
}
} // namespace Detail

inline Ladder& For(Route route)
{
    const auto index = static_cast<size_t>(route);
    return Detail::g_ladders[index < kRouteCount ? index : 0];
}

inline Snapshot Read(Route route) { return For(route).Read(); }

inline void MarkStage(Route route, Stage stage, uint32_t thread)
{
    Detail::g_lastStage.store(Detail::PackStage(route, stage, thread), std::memory_order_relaxed);
}

inline StageMark LastStage()
{
    const auto packed = Detail::g_lastStage.load(std::memory_order_relaxed);
    StageMark mark;
    mark.route = static_cast<Route>((packed >> 40) & 0xff);
    mark.stage = static_cast<Stage>((packed >> 32) & 0xff);
    mark.thread = static_cast<uint32_t>(packed);
    return mark;
}

// One evaluate on one route, from entry to its one outcome. The first outcome named counts; later ones are ignored.
// An evaluation that ends with none counts nothing past its entry, and the ladder stays unbalanced to say so.
class Evaluation
{
    Ladder& _ladder;
    Route _route;
    uint32_t _thread;
    bool _ended = false;

  public:
    Evaluation(Route route, uint32_t thread, Ladder& ladder) : _ladder(ladder), _route(route), _thread(thread)
    {
        _ladder.Enter();
        MarkStage(_route, Stage::Entered, _thread);
    }
    Evaluation(Route route, uint32_t thread) : Evaluation(route, thread, For(route)) {}
    ~Evaluation() { MarkStage(_route, Stage::Left, _thread); }

    Evaluation(const Evaluation&) = delete;
    Evaluation& operator=(const Evaluation&) = delete;

    // Just before the pass records: a crash from here to the outcome is inside it.
    void Recording() { MarkStage(_route, Stage::Recording, _thread); }

    void Ran(bool modelCalled)
    {
        if (_ended)
            return;
        _ended = true;
        _ladder.Ran(modelCalled);
    }

    void Declined(Decline why, const char* detail = nullptr)
    {
        if (_ended)
            return;
        _ended = true;
        _ladder.Declined(why, detail);
    }

    bool Ended() const { return _ended; }
};
} // namespace DlssNr::Funnel
