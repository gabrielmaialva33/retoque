#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// The game's own DLSS Neural Rendering through Streamline (the sl.dlss_nr plugin, Streamline 2.14), and Retoque's NR
// standing down while it runs, so the picture is not given NR twice. docs/dlssnr/design/streamline-native-nr.md,
// tested by tests/nr-sl-native-nr.
//
// The signal is an slEvaluateFeature of kFeatureDlssNr that Streamline accepted (returned eOk), seen by
// StreamlineHooks::hkslEvaluateFeature, which is installed with every Streamline 2 interposer whatever the
// frame-generation input. A load request in slInit is the game being able to run it, not running it; the uplift tags
// are seen only where a tag hook already exists (slSetTag is installed only for a Streamline FG input) and in the
// evaluate's own inputs. Both are recorded and said once, and decide nothing; the control mask is recorded for later
// use only.
//
// The hooks only note: atomics, no lock, no log, no allocation, no config read, so NR off is unchanged. The routes ask
// (AskInto) with NR on, before the model: the D3D12 routes and Dispatch, the Vulkan after-upscale route, and the
// Vulkan present host. The detector's mutex is a leaf: nothing is called while it is held.
//
// The constants are Streamline 2.14.1's (NVIDIA-RTX/Streamline, tag v2.14.1, commit 2122257e0f,
// include/sl_core_types.h; unchanged at v2.14.16). The vendored external/streamline is 2.11.1, declares none of them,
// and is not edited.
namespace DlssNr::SlNativeNr
{
// sl::kFeatureDLSS_NR, "DLSS 3D-Guided Neural Rendering" (changelog.txt, release 2.14.0), plugin sl.dlss_nr.
inline constexpr uint32_t kFeatureDlssNr = 1004;
// sl::kBufferTypeUpliftInputColor: input colour for neural-net "uplift" passes.
inline constexpr uint32_t kBufferTypeUpliftInputColor = 70;
// sl::kBufferTypeUpliftOutputColor: may alias the input (an in-place uplift).
inline constexpr uint32_t kBufferTypeUpliftOutputColor = 71;
// sl::kBufferTypeUpliftControlMask: optional, a 4-channel control mask the uplift consumes.
inline constexpr uint32_t kBufferTypeUpliftControlMask = 72;

// How long after the game's last accepted NR evaluate this pass keeps standing down. A game that turns its NR off in
// its own menu gets this pass back; a loading screen shorter than this does not flip it.
inline constexpr uint64_t kQuietMs = 5000;
// One "stood down so far" line per this many stand-downs.
inline constexpr uint64_t kSayEvery = 3600;

// What the hooks have seen, as bits; each is said once in the log.
inline constexpr uint32_t kSeenRequested = 1u << 0;    // slInit's featuresToLoad named it
inline constexpr uint32_t kSeenEvaluated = 1u << 1;    // an evaluate Streamline accepted
inline constexpr uint32_t kSeenRefused = 1u << 2;      // an evaluate Streamline refused
inline constexpr uint32_t kSeenColourTagged = 1u << 3; // an uplift input or output colour with a resource
inline constexpr uint32_t kSeenControlMask = 1u << 4;  // an uplift control mask with a resource

inline constexpr const char* kStandDownReason =
    "the game runs its own Neural Rendering through Streamline (sl.dlss_nr); this pass stands down so the picture is "
    "not given NR twice";

// The panel's line (DlssNr_Menu.cpp), shown while NR is on and the stand-down holds.
inline constexpr const char* kMenuLine =
    "The game runs its own NR (Streamline DLSS-NR), so this pass stands down rather than apply NR twice. It runs again "
    "5 s after the game's NR stops.";

struct Answer
{
    bool standDown = false;   // the route does not run the model on this frame
    bool latchedNow = false;  // this ask found the game's NR running
    bool releasedNow = false; // ... or found it quiet for kQuietMs after it was
    uint64_t standDowns = 0;  // asks stood down this session, this one included
    uint64_t evaluates = 0;   // accepted NR evaluates the hooks saw
    uint64_t refused = 0;     // refused ones
    uint32_t newlySeen = 0;   // kSeen* bits not reported by an earlier ask
};

class Detector
{
  public:
    // slInit, with the game's own list, before OptiScaler edits it.
    void NoteFeaturesToLoad(const uint32_t* features, uint32_t count)
    {
        if (features == nullptr)
            return;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (features[i] == kFeatureDlssNr)
            {
                _seen.fetch_or(kSeenRequested);
                return;
            }
        }
    }

    // slEvaluateFeature, after Streamline answered. accepted: it returned eOk.
    void NoteEvaluate(uint32_t feature, bool accepted, uint64_t nowMs)
    {
        if (feature != kFeatureDlssNr)
            return;
        if (!accepted)
        {
            _refused.fetch_add(1);
            _seen.fetch_or(kSeenRefused);
            return;
        }
        // The time before the count: an ask that sees the count sees a time at least this recent.
        _lastMs.store(nowMs);
        _evaluates.fetch_add(1);
        _seen.fetch_or(kSeenEvaluated);
    }

    // A tagged buffer. hasResource: a resource with a native pointer (a tag without one removes a tag).
    void NoteTag(uint32_t type, bool hasResource)
    {
        if (!hasResource)
            return;
        if (type == kBufferTypeUpliftInputColor || type == kBufferTypeUpliftOutputColor)
            _seen.fetch_or(kSeenColourTagged);
        else if (type == kBufferTypeUpliftControlMask)
            _seen.fetch_or(kSeenControlMask);
    }

    // A route about to run the model, with NR on.
    Answer Ask(uint64_t nowMs);

    bool Active() const
    {
        std::lock_guard<std::mutex> lock(_lock);
        return _latched;
    }

    uint32_t SeenMask() const { return _seen.load(); }
    uint64_t Evaluates() const { return _evaluates.load(); }
    uint64_t Refused() const { return _refused.load(); }

    uint64_t StandDowns() const
    {
        std::lock_guard<std::mutex> lock(_lock);
        return _standDowns;
    }

    // Host fixtures only; production keeps one detector for the process.
    void Reset()
    {
        std::lock_guard<std::mutex> lock(_lock);
        _seen = 0;
        _evaluates = 0;
        _refused = 0;
        _lastMs = 0;
        _latched = false;
        _standDowns = 0;
        _reported = 0;
    }

  private:
    std::atomic<uint32_t> _seen { 0 };
    std::atomic<uint64_t> _evaluates { 0 };
    std::atomic<uint64_t> _refused { 0 };
    std::atomic<uint64_t> _lastMs { 0 };

    mutable std::mutex _lock;
    bool _latched = false;
    uint64_t _standDowns = 0;
    uint32_t _reported = 0;
};

inline Answer Detector::Ask(uint64_t nowMs)
{
    std::lock_guard<std::mutex> lock(_lock);
    Answer a;

    const uint64_t evaluates = _evaluates.load();
    const uint64_t last = _lastMs.load();
    // A hook thread may have stamped its evaluate after this thread read the clock.
    const uint64_t since = nowMs > last ? nowMs - last : 0;
    const bool recent = evaluates != 0 && since <= kQuietMs;

    a.standDown = recent;
    if (recent && !_latched)
    {
        _latched = true;
        a.latchedNow = true;
    }
    else if (!recent && _latched)
    {
        _latched = false;
        a.releasedNow = true;
    }

    if (a.standDown)
        ++_standDowns;

    a.newlySeen = _seen.load() & ~_reported & ~kSeenEvaluated;
    _reported |= a.newlySeen;

    a.standDowns = _standDowns;
    a.evaluates = evaluates;
    a.refused = _refused.load();
    return a;
}

inline const char* Reason(const Answer& a) { return a.standDown ? kStandDownReason : nullptr; }

// What one ask has to say, in the order it happened: each observation once, the latch and its release, and the count
// at a bounded rate.
inline std::vector<std::string> Lines(const Answer& a)
{
    std::vector<std::string> lines;
    if (a.newlySeen & kSeenRequested)
        lines.emplace_back("DLSS-NR: the game asked Streamline to load its own Neural Rendering (sl.dlss_nr, feature "
                           "1004); it decides nothing until the game evaluates it");
    if (a.newlySeen & kSeenRefused)
        lines.emplace_back("DLSS-NR: Streamline refused the game's own Neural Rendering evaluate (feature 1004); a "
                           "refused evaluate is not counted as the game's NR running");
    if (a.newlySeen & kSeenColourTagged)
        lines.emplace_back("DLSS-NR: the game tagged an uplift colour buffer for its own Neural Rendering (Streamline "
                           "buffer 70 or 71)");
    if (a.newlySeen & kSeenControlMask)
        lines.emplace_back("DLSS-NR: the game tagged an uplift control mask (Streamline buffer 72); recorded only, not "
                           "used");
    if (a.latchedNow)
        lines.emplace_back("DLSS-NR: " + std::string(kStandDownReason) +
                           "; every route stands down while it runs and for 5 s after its last frame");
    if (a.releasedNow)
        lines.emplace_back("DLSS-NR: the game's own Neural Rendering has not run for 5 s; this pass runs again (" +
                           std::to_string(a.standDowns) + " evaluates were stood down)");
    if (a.standDown && !a.latchedNow && a.standDowns % kSayEvery == 0)
        lines.emplace_back("DLSS-NR: " + std::to_string(a.standDowns) +
                           " evaluates stood down so far for the game's own Neural Rendering");
    return lines;
}

// The routes' one call: asks, appends what is to be said, and returns the reason to stand down, or nullptr to run.
inline const char* AskInto(Detector& detector, std::vector<std::string>& lines, uint64_t nowMs)
{
    const Answer a = detector.Ask(nowMs);
    for (auto& line : Lines(a))
        lines.push_back(std::move(line));
    return Reason(a);
}

// sl::ResourceTag's shape: a type, and a resource whose native pointer is the texture.
template <class Tag> void NoteTags(Detector& detector, const Tag* tags, uint32_t count)
{
    if (tags == nullptr)
        return;
    for (uint32_t i = 0; i < count; ++i)
        detector.NoteTag(tags[i].type, tags[i].resource != nullptr && tags[i].resource->native != nullptr);
}

// --- The process's detector and the hooks' entry points ----------------------------------------------------------

inline Detector& Instance()
{
    static Detector detector;
    return detector;
}

inline uint64_t NowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline void OnInit(const uint32_t* features, uint32_t count) { Instance().NoteFeaturesToLoad(features, count); }

// Every slEvaluateFeature; any other feature returns on one compare, before the clock.
inline void OnEvaluate(uint32_t feature, bool accepted)
{
    if (feature != kFeatureDlssNr)
        return;
    Instance().NoteEvaluate(feature, accepted, NowMs());
}

template <class Tag> void OnTags(const Tag* tags, uint32_t count) { NoteTags(Instance(), tags, count); }
} // namespace DlssNr::SlNativeNr
