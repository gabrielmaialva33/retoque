#pragma once

// D3D12 compute pass for DLSS-NR interpass clamp and PassFeedback hand-off between sequential model passes.
// docs/dlssnr/design/2026-11-22-interpass-clamp.md.

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

// Up to 9 interpass boundaries per frame (MaxPasses = 10) across 16 frames in flight without a fence.
#define DLSSNR_PASS_CLAMP_HEAPS 144

class DlssNr_PassClamp_Dx12 : public Shader_Dx12
{
  public:
    struct alignas(256) Constants
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t ClampEnabled = 0;
        float PassFeedback = 1.0f;
    };

  private:
    FrameDescriptorHeap _frameHeaps[DLSSNR_PASS_CLAMP_HEAPS];
    ID3D12Resource* _constantBuffers[DLSSNR_PASS_CLAMP_HEAPS] = {};
    void* _mappedConstants[DLSSNR_PASS_CLAMP_HEAPS] = {};
    uint32_t _heapIndex = 0;

  public:
    static bool Available();

    DlssNr_PassClamp_Dx12(std::string InName, ID3D12Device* InDevice);
    ~DlssNr_PassClamp_Dx12();

    // InRaw and InProxy arrive in NON_PIXEL_SHADER_RESOURCE; OutClamped arrives and stays in UNORDERED_ACCESS.
    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InRaw, ID3D12Resource* InProxy,
                  ID3D12Resource* OutClamped, uint32_t InWidth, uint32_t InHeight, bool InClampEnabled,
                  float InPassFeedback);
};
