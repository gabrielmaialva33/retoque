#pragma once

// D3D12 compute pass for de-jittering render-resolution motion vectors before DLSS-NR reads them.
// docs/dlssnr/design/2026-11-22-dejitter-motion-vectors.md.

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

// One dispatch per frame across 16 frames in flight without a fence.
#define DLSSNR_DEJITTER_HEAPS 16

class DlssNr_Dejitter_Dx12 : public Shader_Dx12
{
  public:
    struct alignas(256) Constants
    {
        uint32_t BaseX = 0;
        uint32_t BaseY = 0;
        uint32_t Width = 0;
        uint32_t Height = 0;
        float DeltaMvX = 0.0f;
        float DeltaMvY = 0.0f;
        uint32_t Pad0 = 0;
        uint32_t Pad1 = 0;
    };

  private:
    FrameDescriptorHeap _frameHeaps[DLSSNR_DEJITTER_HEAPS];
    ID3D12Resource* _constantBuffers[DLSSNR_DEJITTER_HEAPS] = {};
    void* _mappedConstants[DLSSNR_DEJITTER_HEAPS] = {};
    uint32_t _heapIndex = 0;

  public:
    static bool Available();

    DlssNr_Dejitter_Dx12(std::string InName, ID3D12Device* InDevice);
    ~DlssNr_Dejitter_Dx12();

    // InMotion arrives in NON_PIXEL_SHADER_RESOURCE; OutMotion arrives and stays in UNORDERED_ACCESS.
    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InMotion, ID3D12Resource* OutMotion,
                  uint32_t InBaseX, uint32_t InBaseY, uint32_t InWidth, uint32_t InHeight, float InDeltaMvX,
                  float InDeltaMvY);
};
