// Captured frames from D3D11 into D3D12: a ring of shared NT-handle textures and two shared fences, ordered on the
// GPU. D3D11 waits until D3D12 has finished reading a slot before writing it again; D3D12 waits until D3D11 has
// finished writing it before reading. The CPU never waits for either, except to prove the GPU idle before a resize or
// at exit (WaitIdle).
#pragma once

#include "Geometry.h"

#include <array>
#include <string>

namespace RetoqueWindowHost
{
class Transport
{
  public:
    static constexpr uint64_t kSlots = 3;

    bool Create(ID3D11Device5* d3d11, ID3D11DeviceContext4* context, ID3D12Device* d3d12, std::string& why);

    // (Re)creates the slots at this extent. Only after WaitIdle proved every earlier use complete.
    bool EnsureExtent(uint32_t width, uint32_t height, std::string& why);
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }

    // D3D11: copy the box out of the captured surface into the next slot and signal "captured" with its serial.
    uint64_t Submit(ID3D11Texture2D* source, const Geometry::Box& box);

    // D3D12: the slot that serial was copied into, and the fences to wait on and signal.
    ID3D12Resource* Slot(uint64_t serial) const { return slots12_[serial % kSlots].Get(); }
    ID3D12Fence* Captured() const { return captured12_.Get(); }
    ID3D12Fence* Consumed() const { return consumed12_.Get(); }

    // How far D3D11 has got: every captured surface read by a serial at or below this may go back to WGC.
    uint64_t CapturedCompleted() const { return captured11_ != nullptr ? captured11_->GetCompletedValue() : 0; }
    uint64_t LastSerial() const { return serial_; }

    // CPU: every submitted copy written and read, within the timeout. False when that cannot be proved.
    bool WaitIdle(DWORD milliseconds);

    // Nothing here is released again (completion could not be proved).
    void Abandon();

  private:
    ID3D11Device5* d3d11_ = nullptr;
    ID3D11DeviceContext4* context_ = nullptr;
    ID3D12Device* d3d12_ = nullptr;
    ComPtr<ID3D12Fence> captured12_, consumed12_;
    ComPtr<ID3D11Fence> captured11_, consumed11_;
    std::array<ComPtr<ID3D11Texture2D>, kSlots> slots11_;
    std::array<ComPtr<ID3D12Resource>, kSlots> slots12_;
    uint32_t width_ = 0, height_ = 0;
    uint64_t serial_ = 0;
    HANDLE event_ = nullptr;
};
} // namespace RetoqueWindowHost
