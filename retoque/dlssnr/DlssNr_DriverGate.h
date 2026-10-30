#pragma once

#include <atomic>
#include <cstdint>
#include <string>

// The NVIDIA driver Neural Rendering needs (docs/dlssnr/design/driver-gate.md).
//
// DLSS Unlocked #60: an RTX 2080 Ti on Windows driver 576.88. NGX refused feature 18 with FAIL_OutOfDate, a loader
// that calls the model itself created it anyway, and the GPU hung until a reboot. Both loaders here (the forwarder and
// ModelLoader=direct) call the model's own entry points, so nothing on their path asks the driver whether it can run
// it. Two refusals close that:
//
//   * the version: on native Windows a known NVAPI driver below 610.88 turns NR off for the session (volatile, the
//     ini is never written). Wine/Proton is not gated: dxvk-nvapi reports Linux driver numbers, which are not
//     Windows' and cannot be compared with these. An unknown version (0) does not block.
//   * the answer: once any NGX call on the NR route returns FAIL_OutOfDate, no model is created for the rest of the
//     process, by any loader. This also covers Wine and an unknown version.
//
// 610.88 is the oldest driver verified to run NR and 616.64 the first with official support. The rule and both numbers
// are ShyVortex's (OptiScaler-DLSSNR-PreSR-Multipass 97f35314, 521138df, dec66857, GPL-3.0).
//
// Header-only and free of Windows headers, so tests/nr-driver-gate compiles it as shipped.
namespace DlssNr::DriverGate
{
inline constexpr uint32_t kMinimumWindowsDriver = 61088;     // NVAPI encoding of 610.88
inline constexpr uint32_t kRecommendedWindowsDriver = 61664; // 616.64
inline constexpr unsigned int kNgxResultOutOfDate = 0xBAD0000Cu;

// Fixed text, because the renderer keeps its failure reason as a const char*.
inline constexpr const char* kTooOldReason =
    "this NVIDIA driver is too old for Neural Rendering; update to 610.88 or newer (616.64 or newer recommended)";
inline constexpr const char* kOutOfDateReason =
    "NGX answered FAIL_OutOfDate: this NVIDIA driver cannot run Neural Rendering; update to 610.88 or newer "
    "(616.64 or newer recommended)";

enum class Verdict
{
    Supported,
    TooOld,
    Unknown,      // NVAPI gave no version: not blocked; the FAIL_OutOfDate latch still applies
    NotApplicable // not NVIDIA, or Wine/Proton (dxvk-nvapi's numbers are Linux driver numbers)
};

constexpr Verdict Resolve(bool nvidia, bool onWine, uint32_t driverVersion)
{
    if (!nvidia || onWine)
        return Verdict::NotApplicable;
    if (driverVersion == 0)
        return Verdict::Unknown;
    return driverVersion < kMinimumWindowsDriver ? Verdict::TooOld : Verdict::Supported;
}

constexpr bool Blocks(Verdict verdict) { return verdict == Verdict::TooOld; }

constexpr bool IsOutOfDate(unsigned int ngxResult) { return ngxResult == kNgxResultOutOfDate; }

// 61664 -> "616.64", 60005 -> "600.05".
inline std::string FormatVersion(uint32_t driverVersion)
{
    const uint32_t minor = driverVersion % 100;
    return std::to_string(driverVersion / 100) + (minor < 10 ? ".0" : ".") + std::to_string(minor);
}

// The log line and the hotkey's toast. The menu says the same through its own translatable format.
inline std::string BlockedMessage(uint32_t driverVersion)
{
    return "Neural Rendering is off for this session: it needs NVIDIA driver " + FormatVersion(kMinimumWindowsDriver) +
           " or newer (" + FormatVersion(kRecommendedWindowsDriver) + " or newer recommended), and this one is " +
           FormatVersion(driverVersion) + ". Your settings are unchanged.";
}

// Process state: published once by the GPU worker, read by the menu and every create site. Sticky; nothing resets it.
class Gate
{
    std::atomic<uint32_t> _version { 0 };
    std::atomic<bool> _blocked { false };
    std::atomic<bool> _outOfDate { false };

  public:
    void Publish(Verdict verdict, uint32_t driverVersion)
    {
        _version.store(driverVersion, std::memory_order_release);
        if (Blocks(verdict))
            _blocked.store(true, std::memory_order_release);
    }

    // Records an NGX result from the NR route. True when it was FAIL_OutOfDate (and the latch is now set).
    bool Note(unsigned int ngxResult)
    {
        if (!IsOutOfDate(ngxResult))
            return false;
        _outOfDate.store(true, std::memory_order_release);
        return true;
    }

    bool Blocked() const { return _blocked.load(std::memory_order_acquire); }
    bool OutOfDateSeen() const { return _outOfDate.load(std::memory_order_acquire); }
    bool Refuses() const { return Blocked() || OutOfDateSeen(); }
    uint32_t Version() const { return _version.load(std::memory_order_acquire); }

    // Why the model is refused, or "" when it is not.
    const char* Reason() const
    {
        if (Blocked())
            return kTooOldReason;
        if (OutOfDateSeen())
            return kOutOfDateReason;
        return "";
    }
};

inline Gate& Process()
{
    static Gate gate;
    return gate;
}

inline void Publish(Verdict verdict, uint32_t driverVersion) { Process().Publish(verdict, driverVersion); }
inline bool NoteNgxResult(unsigned int ngxResult) { return Process().Note(ngxResult); }
inline bool Blocked() { return Process().Blocked(); }
inline bool Refuses() { return Process().Refuses(); }
inline uint32_t DetectedVersion() { return Process().Version(); }
inline const char* Reason() { return Process().Reason(); }
} // namespace DlssNr::DriverGate
