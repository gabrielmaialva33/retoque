// NR on any window (docs/dlssnr/design/nr-anything.md): what the window host's export says about one present.
//
// RetoqueWindowHost.exe (retoque/window_host/) captures another program's window and presents it on a swapchain of
// its own. Before each Present it hands that swapchain and its queue to the production present pass through the
// RetoqueWindow_* exports in DlssNr_WindowHost.cpp, which are inert unless the host process set
// RETOQUE_WINDOW_HOST=1 in its own environment. A game never sets it, and nothing in the DLL calls them.
//
// The struct is the export's ABI with the host and its tests: plain data, versioned, append-only.

#pragma once

#include <cstdint>

namespace DlssNr::WindowHost
{
// The environment variable the capture program sets in its own process before it loads OptiScaler.dll.
inline constexpr char kEnvironment[] = "RETOQUE_WINDOW_HOST";

// RetoqueWindow_Version() returns this. A host refuses any other.
inline constexpr uint32_t kVersion = 1;

struct Outcome
{
    uint32_t version = 1;
    uint32_t entered = 0;        // the present pass ran its course for this present (it may still have skipped)
    uint32_t recorded = 0;       // and recorded an NR pass onto the host's queue
    uint32_t running = 0;        // the NR feature exists and has not failed
    uint64_t passesRecorded = 0; // NR passes recorded in this process so far
    char status[160] = {};       // the refusal; else the failure reason, or the pass's status line
};
} // namespace DlssNr::WindowHost
