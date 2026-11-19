#pragma once
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_4.h>

namespace DlssNr::PresentDepthDx12
{
bool Active();
void Install(ID3D12Device* device);
void Stop();
void DeviceDestroyed(IUnknown* device);
void Created(ID3D12Device* device, const D3D12_RESOURCE_DESC* desc, ID3D12Resource* resource,
             D3D12_RESOURCE_STATES state, ID3D12Heap* heap = nullptr);
void Destroyed(ID3D12Resource* resource);
void Captured(size_t slot, ID3D12Resource* source, bool inverted, bool fullExtent);
void Eligible(size_t slot);
// A scope suppresses only this thread's own recording. Its lists carry a private CPU marker at Close;
// submission excludes that marker, never a process-wide flag or a guess from the submitting thread.
class InternalScope
{
    bool _active;

  public:
    InternalScope();
    ~InternalScope();
    InternalScope(const InternalScope&) = delete;
    InternalScope& operator=(const InternalScope&) = delete;
};
class Observation
{
    bool _active = false;

  public:
    Observation(bool ownsPresent, IDXGISwapChain3* chain, ID3D12CommandQueue* queue);
    ~Observation();
    Observation(const Observation&) = delete;
    Observation& operator=(const Observation&) = delete;
};
// Shares the existing ExecuteCommandLists detour. No observer lock survives into the real call.
class Submission
{
    void* _queue = nullptr;

  public:
    Submission(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
    ~Submission();
    Submission(const Submission&) = delete;
    Submission& operator=(const Submission&) = delete;
};
} // namespace DlssNr::PresentDepthDx12
