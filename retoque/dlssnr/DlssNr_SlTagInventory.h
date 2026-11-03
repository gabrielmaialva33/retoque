#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

// Which buffers the game tags for Streamline, and whether the two a HUD-aware present would need -- the HUD-less colour
// and the UI colour with alpha -- would still be valid at the game's Present. Log only: nothing reads it to decide.
// docs/dlssnr/design/sl-tag-inventory.md, tested by tests/nr-sl-tag-inventory.
//
// Why: the present pass at the Streamline boundary edits the HUD with the rest of the frame (nr-before-dlssg.md, H1).
// Its designed successors (H2/H3 there, mask (c) in hud-protection.md) would use the game's own HUD-less or UI-alpha
// buffer, which only works if the title tags one and tags it with a lifecycle that keeps it valid until Present
// (sl::eValidUntilPresent). Which titles do that is not known; this records it from the title's own calls.
//
// The hooks only note: atomics, no lock, no log, no allocation, no config read. They are fed only while armed, which
// StreamlineHooks does once, at hook time, when [DlssNr] Enabled is true; NR off is unchanged. A route with NR on asks
// for the line (TakeLine) at its own rate; the line is said when what was seen changed, at most once per kMinGapMs.
//
// The buffer-type and lifecycle numbers are Streamline's (sl_core_types.h; vendored 2.11.1, unchanged through 2.14).
namespace DlssNr::SlTagInventory
{
// sl::BufferType values this inventory names; any other type is still counted, by number.
inline constexpr uint32_t kDepth = 0;
inline constexpr uint32_t kMotionVectors = 1;
inline constexpr uint32_t kHudLessColor = 2;
inline constexpr uint32_t kScalingInputColor = 3;
inline constexpr uint32_t kScalingOutputColor = 4;
inline constexpr uint32_t kExposure = 13;
inline constexpr uint32_t kUiColorAndAlpha = 23;
inline constexpr uint32_t kHiResDepth = 48;
inline constexpr uint32_t kLinearDepth = 49;
inline constexpr uint32_t kBidirectionalDistortionField = 50;
inline constexpr uint32_t kBackbuffer = 53;
inline constexpr uint32_t kNoWarpMask = 54;

// Types 0 .. kTypes-1 are kept one by one (Streamline 2.14 defines up to 72); a larger number is counted as "other".
inline constexpr uint32_t kTypes = 96;

// sl::ResourceLifecycle.
inline constexpr uint32_t kOnlyValidNow = 0;
inline constexpr uint32_t kValidUntilPresent = 1;
inline constexpr uint32_t kValidUntilEvaluate = 2;

// Where a tag was seen, as bits.
inline constexpr uint32_t kViaSetTag = 1u << 0;         // slSetTag
inline constexpr uint32_t kViaSetTagForFrame = 1u << 1; // slSetTagForFrame
inline constexpr uint32_t kViaEvaluate = 1u << 2;       // a ResourceTag among slEvaluateFeature's inputs

// The line is said at most this often, and only when something changed.
inline constexpr uint64_t kMinGapMs = 2000;

// What a tag looked like, as the hook could see it. Zero is "not known".
struct Shape
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;  // DXGI_FORMAT or VkFormat, as the API has it; 32767 stands for any larger value
    bool subrect = false; // the tag's extent is a part of the resource
    bool buffer = false;  // a buffer, not an image: no width, height or format is said
};

inline uint64_t Pack(const Shape& s)
{
    return (uint64_t(s.width & 0xFFFFu)) | (uint64_t(s.height & 0xFFFFu) << 16) |
           (uint64_t(s.format > 0x7FFFu ? 0x7FFFu : s.format) << 32) | (uint64_t(s.buffer ? 1u : 0u) << 49) |
           (uint64_t(s.subrect ? 1u : 0u) << 47) | (uint64_t(1u) << 48); // bit 48: a shape was stored
}

inline Shape Unpack(uint64_t v)
{
    Shape s;
    s.width = uint32_t(v & 0xFFFFu);
    s.height = uint32_t((v >> 16) & 0xFFFFu);
    s.format = uint32_t((v >> 32) & 0x7FFFu);
    s.buffer = ((v >> 49) & 1u) != 0;
    s.subrect = ((v >> 47) & 1u) != 0;
    return s;
}

inline const char* TypeName(uint32_t type)
{
    switch (type)
    {
    case kDepth:
        return "depth";
    case kMotionVectors:
        return "motion";
    case kHudLessColor:
        return "HUD-less colour";
    case kScalingInputColor:
        return "scaling input";
    case kScalingOutputColor:
        return "scaling output";
    case kExposure:
        return "exposure";
    case kUiColorAndAlpha:
        return "UI colour+alpha";
    case kHiResDepth:
        return "hi-res depth";
    case kLinearDepth:
        return "linear depth";
    case kBidirectionalDistortionField:
        return "distortion field";
    case kBackbuffer:
        return "backbuffer";
    case kNoWarpMask:
        return "no-warp mask";
    default:
        return nullptr;
    }
}

// What a HUD-aware present can expect of one buffer type, from the lifecycles it was tagged with.
enum class AtPresent : uint8_t
{
    NotTagged, // never tagged with a resource
    Valid,     // every tag was eValidUntilPresent
    NotValid,  // every tag was eOnlyValidNow or eValidUntilEvaluate: Streamline may copy it, the game may reuse it
    Mixed,     // some of each
};

inline AtPresent PresentValidity(uint32_t lifecycleMask)
{
    if (lifecycleMask == 0)
        return AtPresent::NotTagged;
    const bool valid = (lifecycleMask & (1u << kValidUntilPresent)) != 0;
    const bool notValid = (lifecycleMask & ~(1u << kValidUntilPresent)) != 0;
    if (valid && notValid)
        return AtPresent::Mixed;
    return valid ? AtPresent::Valid : AtPresent::NotValid;
}

inline const char* PresentValidityText(AtPresent v)
{
    switch (v)
    {
    case AtPresent::NotTagged:
        return "not tagged";
    case AtPresent::Valid:
        return "valid until Present";
    case AtPresent::NotValid:
        return "not valid at Present";
    case AtPresent::Mixed:
        return "valid until Present only sometimes";
    }
    return "";
}

class Inventory
{
  public:
    // Fed only while armed (StreamlineHooks, at hook time, with NR on).
    void Arm() { _armed.store(true, std::memory_order_relaxed); }
    bool Armed() const { return _armed.load(std::memory_order_relaxed); }

    // One tag. hasResource: a resource with a native pointer (a tag without one removes a tag).
    void Note(uint32_t type, bool hasResource, uint32_t lifecycle, uint32_t via, const Shape& shape)
    {
        if (!Armed())
            return;
        if (!hasResource)
        {
            _removals.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (type >= kTypes)
        {
            _other.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        Slot& s = _slots[type];
        s.tags.fetch_add(1, std::memory_order_relaxed);
        if (lifecycle <= kValidUntilEvaluate)
            s.lifecycles.fetch_or(1u << lifecycle, std::memory_order_relaxed);
        else
            s.lifecycles.fetch_or(1u << 3, std::memory_order_relaxed); // a value Streamline does not define
        s.via.fetch_or(via, std::memory_order_relaxed);
        if (shape.width != 0 || shape.height != 0 || shape.format != 0 || shape.buffer)
            s.shape.store(Pack(shape), std::memory_order_relaxed);
    }

    // A route with NR on, at its own rate. Fills `line` and returns true when the inventory changed since the last
    // line and at least kMinGapMs have passed; the first line waits for the first tag.
    bool TakeLine(std::string& line, uint64_t nowMs)
    {
        std::lock_guard<std::mutex> lock(_lock);
        if (_saidAtMs != 0 && nowMs < _saidAtMs + kMinGapMs)
            return false;
        // One copy of the inventory makes both the number and the text, so a tag arriving in between cannot make
        // the line say one thing and the remembered number another.
        const Snapshot now = Take();
        const uint64_t signature = Signature(now);
        if (signature == _saidSignature)
            return false;
        line = Describe(now);
        if (line.empty())
            return false;
        _saidSignature = signature;
        _saidAtMs = nowMs == 0 ? 1 : nowMs;
        return true;
    }

    // What the HUD-aware present would find, for the two buffers it would use.
    AtPresent HudLess() const { return PresentValidity(_slots[kHudLessColor].lifecycles.load()); }
    AtPresent UiAlpha() const { return PresentValidity(_slots[kUiColorAndAlpha].lifecycles.load()); }

    uint64_t Tags(uint32_t type) const { return type < kTypes ? _slots[type].tags.load() : _other.load(); }
    uint32_t Lifecycles(uint32_t type) const { return type < kTypes ? _slots[type].lifecycles.load() : 0; }
    uint32_t Via(uint32_t type) const { return type < kTypes ? _slots[type].via.load() : 0; }
    Shape ShapeOf(uint32_t type) const { return type < kTypes ? Unpack(_slots[type].shape.load()) : Shape {}; }
    uint64_t Removals() const { return _removals.load(); }

    // The line, as it would be said now.
    std::string Describe() const { return Describe(Take()); }

    // Host fixtures only; production keeps one inventory for the process.
    void Reset()
    {
        std::lock_guard<std::mutex> lock(_lock);
        _armed = false;
        for (auto& s : _slots)
        {
            s.tags = 0;
            s.lifecycles = 0;
            s.via = 0;
            s.shape = 0;
        }
        _other = 0;
        _removals = 0;
        _saidSignature = 0;
        _saidAtMs = 0;
    }

  private:
    struct Slot
    {
        std::atomic<uint64_t> tags { 0 };
        std::atomic<uint32_t> lifecycles { 0 };
        std::atomic<uint32_t> via { 0 };
        std::atomic<uint64_t> shape { 0 };
    };

    // The inventory read once: what one line and its number are both made from.
    struct Seen
    {
        bool tagged = false;
        uint32_t lifecycles = 0;
        uint32_t via = 0;
        uint64_t shape = 0;
    };
    struct Snapshot
    {
        Seen types[kTypes];
        bool other = false;
    };

    Snapshot Take() const
    {
        Snapshot snap;
        for (uint32_t t = 0; t < kTypes; ++t)
        {
            const Slot& s = _slots[t];
            Seen& seen = snap.types[t];
            seen.tagged = s.tags.load(std::memory_order_relaxed) != 0;
            if (!seen.tagged)
                continue;
            seen.lifecycles = s.lifecycles.load(std::memory_order_relaxed);
            seen.via = s.via.load(std::memory_order_relaxed);
            seen.shape = s.shape.load(std::memory_order_relaxed);
        }
        snap.other = _other.load(std::memory_order_relaxed) != 0;
        return snap;
    }

    static std::string Describe(const Snapshot& snap)
    {
        std::string types;
        for (uint32_t t = 0; t < kTypes; ++t)
        {
            const Seen& s = snap.types[t];
            if (!s.tagged)
                continue;
            if (!types.empty())
                types += "; ";
            const char* name = TypeName(t);
            types += name != nullptr ? std::string(name) : "type " + std::to_string(t);
            types += " (" + LifecycleText(s.lifecycles);
            types += ViaText(s.via);
            const uint64_t packed = s.shape;
            if (packed != 0)
            {
                const Shape shape = Unpack(packed);
                if (shape.buffer)
                    types += ", buffer";
                if (shape.width != 0 && shape.height != 0)
                    types += ", " + std::to_string(shape.width) + "x" + std::to_string(shape.height);
                if (shape.format != 0)
                    types += ", format " + std::to_string(shape.format);
                if (shape.subrect)
                    types += ", subrect";
            }
            types += ")";
        }
        if (snap.other)
            types += std::string(types.empty() ? "" : "; ") + "types past " + std::to_string(kTypes - 1);
        if (types.empty())
            return {};
        return "DLSS-NR: Streamline tags from the game: " + types + " | for a HUD-aware present: HUD-less colour " +
               PresentValidityText(PresentValidity(snap.types[kHudLessColor].lifecycles)) + ", UI colour+alpha " +
               PresentValidityText(PresentValidity(snap.types[kUiColorAndAlpha].lifecycles)) + " (log only)";
    }

    static std::string LifecycleText(uint32_t mask)
    {
        std::string text;
        const auto add = [&text](const char* part)
        {
            if (!text.empty())
                text += "/";
            text += part;
        };
        if (mask & (1u << kOnlyValidNow))
            add("only valid now");
        if (mask & (1u << kValidUntilPresent))
            add("until Present");
        if (mask & (1u << kValidUntilEvaluate))
            add("until evaluate");
        if (mask & (1u << 3))
            add("unknown lifecycle");
        return text;
    }

    static std::string ViaText(uint32_t via)
    {
        std::string text;
        if (via & kViaSetTag)
            text += ", slSetTag";
        if (via & kViaSetTagForFrame)
            text += ", per frame";
        if (via & kViaEvaluate)
            text += ", evaluate input";
        return text;
    }

    // What the line says, as a number: the types seen, their lifecycles, where, and their shapes. Counts are not in it,
    // so a title that keeps tagging the same buffers is said once.
    static uint64_t Signature(const Snapshot& snap)
    {
        uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](uint64_t v)
        {
            h ^= v;
            h *= 1099511628211ull;
        };
        for (uint32_t t = 0; t < kTypes; ++t)
        {
            const Seen& s = snap.types[t];
            if (!s.tagged)
                continue;
            mix(t + 1);
            mix(s.lifecycles);
            mix(s.via);
            mix(s.shape);
        }
        mix(snap.other ? 1 : 0);
        return h;
    }

    std::atomic<bool> _armed { false };
    Slot _slots[kTypes];
    std::atomic<uint64_t> _other { 0 };
    std::atomic<uint64_t> _removals { 0 };

    std::mutex _lock;
    uint64_t _saidSignature = 0;
    uint64_t _saidAtMs = 0;
};

// sl::ResourceTag's shape: type, lifecycle, extent, and a resource with type, native, width, height, nativeFormat.
// describe(resource, shape) fills the shape from the native resource when the API can say more than the tag (D3D12's
// GetDesc), and marks a buffer as one.
template <class Tag, class Describe>
void NoteTags(Inventory& inventory, const Tag* tags, uint32_t count, uint32_t via, Describe&& describe)
{
    if (tags == nullptr || !inventory.Armed())
        return;
    for (uint32_t i = 0; i < count; ++i)
    {
        const Tag& tag = tags[i];
        const bool hasResource = tag.resource != nullptr && tag.resource->native != nullptr;
        Shape shape;
        if (hasResource)
        {
            shape.width = tag.resource->width;
            shape.height = tag.resource->height;
            shape.format = tag.resource->nativeFormat;
            describe(*tag.resource, shape);
            if (!shape.buffer && tag.extent.width != 0 && tag.extent.height != 0 &&
                (tag.extent.left != 0 || tag.extent.top != 0 || (shape.width != 0 && tag.extent.width != shape.width) ||
                 (shape.height != 0 && tag.extent.height != shape.height)))
                shape.subrect = true;
            if (!shape.buffer && shape.width == 0 && shape.height == 0)
            {
                shape.width = tag.extent.width;
                shape.height = tag.extent.height;
            }
        }
        inventory.Note(static_cast<uint32_t>(tag.type), hasResource, static_cast<uint32_t>(tag.lifecycle), via, shape);
    }
}

// --- The process's inventory --------------------------------------------------------------------------------------

inline Inventory& Instance()
{
    static Inventory inventory;
    return inventory;
}

inline uint64_t NowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
} // namespace DlssNr::SlTagInventory
