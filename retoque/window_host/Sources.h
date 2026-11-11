// The Windows side of choosing a source: the facts Policy.h decides on, read off a window handle; the catalog; the
// geometry Geometry.h crops with; and which adapter drives the monitor the window is on.
#pragma once

#include "Geometry.h"
#include "Policy.h"

#include <string>
#include <vector>

namespace RetoqueWindowHost::Sources
{
std::string Narrow(const std::wstring& text);

Policy::WindowFacts Facts(HWND window);
bool ReadIdentity(HWND window, Policy::Identity& identity);
std::string Title(HWND window);

struct Entry
{
    Policy::Identity identity;
    std::string title;
    int32_t width = 0, height = 0;
};

// Every top-level window CandidateRefusal accepts, in z-order; refused counts how many were left out.
std::vector<Entry> Catalog(size_t& refused);

// For the running capture: is it still there, the same window, visible?
Policy::SourceFacts SourceState(HWND window, const Policy::Identity& expected, bool itemClosed);

// Client rectangle, frame bounds and monitor facts for ClientCrop; the frame's content size is filled by the caller.
bool CropInputs(HWND window, Geometry::CropInputs& in);

struct AdapterChoice
{
    bool outputFound = false;
    uint32_t vendorId = 0;
    LUID luid {};
    ComPtr<IDXGIAdapter1> adapter;
    std::string name;
};

// The adapter with an output on the monitor the window is mostly on.
AdapterChoice AdapterFor(HWND window, IDXGIFactory1* factory);
} // namespace RetoqueWindowHost::Sources
