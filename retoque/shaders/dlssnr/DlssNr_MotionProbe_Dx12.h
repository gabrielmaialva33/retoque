#pragma once

// Zero-motion detection's GPU half (docs/dlssnr/design/zero-motion-detection.md): a 32x32 grid over the game's depth
// and motion vectors -- per cell the depth at its centre, its slope, and how many motion samples are not exactly zero
// -- copied to a readback ring. The CPU half, which turns grids into the verdict, is dlssnr/DlssNr_MotionProbe.h.
//
// Built on the first probe of a device and parked with the pass's other retired objects when the device changes: the
// last recordings still reference its buffers. Nothing in it depends on the guides' size, so a resize keeps it.
//
// Its bytecode may be absent: DlssNr_MotionProbe_Dx12.cpp includes the precompiled header only if it exists, and
// without it Available() is false and no probe is recorded.

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <dlssnr/DlssNr_MotionProbe.h>

// Readbacks in flight. One probe every eighth evaluate, so four slots cover a GPU thirty-two frames behind the CPU;
// past that a probe is skipped, never waited for.
#define DLSSNR_MOTION_PROBE_SLOTS 4

class DlssNr_MotionProbe_Dx12 : public Shader_Dx12
{
  public:
    // The Params cbuffer of dlssnr_motion_probe.hlsl, padded to the 256 bytes every constant-buffer view must have.
    struct alignas(256) Params
    {
        uint32_t depthBaseX = 0;
        uint32_t depthBaseY = 0;
        uint32_t depthWidth = 0;
        uint32_t depthHeight = 0;
        uint32_t motionBaseX = 0;
        uint32_t motionBaseY = 0;
        uint32_t motionWidth = 0;
        uint32_t motionHeight = 0;
    };

    static bool Available();

    DlssNr_MotionProbe_Dx12(std::string name, ID3D12Device* device);
    ~DlssNr_MotionProbe_Dx12();

    bool BuiltFor(ID3D12Device* device) const { return device == _device; }

    // Records the probe and its copy into the slot's readback. Both guides arrive readable
    // (NON_PIXEL_SHADER_RESOURCE) and are left so; each is read only inside its region.
    bool Record(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* depth, ID3D12Resource* motion,
                const DlssNr::MotionProbe::Shape& shape, uint32_t slot);

    // The slot's grid (DlssNr::MotionProbe::kGridFloats floats), once the recording that filled it has completed.
    const float* Grid(uint32_t slot) const;

  private:
    static constexpr uint32_t kSrvCount = 2;
    static constexpr uint32_t kUavCount = 1;
    static constexpr uint32_t kGrid = DlssNr::MotionProbe::kGrid;
    static constexpr uint32_t kThreads = 8;

    ID3D12Resource* _grid = nullptr; // RGBA32F kGrid x kGrid, rests in UNORDERED_ACCESS
    ID3D12Resource* _constantBuffers[DLSSNR_MOTION_PROBE_SLOTS] = {};
    void* _mappedConstants[DLSSNR_MOTION_PROBE_SLOTS] = {};
    ID3D12Resource* _readbackBuffers[DLSSNR_MOTION_PROBE_SLOTS] = {};
    void* _mappedReadback[DLSSNR_MOTION_PROBE_SLOTS] = {};
    FrameDescriptorHeap _frameHeaps[DLSSNR_MOTION_PROBE_SLOTS];
};
