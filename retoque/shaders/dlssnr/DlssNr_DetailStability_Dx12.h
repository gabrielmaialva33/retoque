#pragma once

// Detail stability for Neural Rendering ([DlssNr] DetailStability): a temporal filter on the model's edit in the log
// domain, the history reprojected with the game's motion vectors, clipped to the current 3x3 and rejected where the
// input moved under it. docs/dlssnr/design/detail-stability.md is the reasoning; dlssnr/DlssNr_DetailStability.h
// decides which frames run it and precompile/dlssnr_detail_stability_rule.h is the per-pixel rule.
//
// One object serves one working size on one device. Its history and its output are sized at construction; a different
// size gets a new object, and the old one is parked with the pass's other retired objects rather than freed, because
// the recordings that used it may still be on the GPU.

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

// One dispatch per frame, reused round-robin with no fence: the same coverage the stabilizer keeps.
#define DLSSNR_DETAILSTABILITY_NUM_OF_HEAPS 16

class DlssNr_DetailStability_Dx12 : public Shader_Dx12
{
  public:
    // Where in the motion texture this frame's vectors lie; mapped point-wise onto the working size, the cadence's
    // rule.
    struct Region
    {
        uint32_t baseX = 0;
        uint32_t baseY = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct Frame
    {
        ID3D12Resource* proxy = nullptr;  // the model's input, at the working size, readable
        ID3D12Resource* motion = nullptr; // the game's motion, readable
        Region motionRegion {};
        float mvToWorkX = 1.0f; // the game's vectors into working pixels
        float mvToWorkY = 1.0f;
        ID3D12Resource* uiAlpha = nullptr; // may be null
    };

    struct Settings
    {
        float alpha = 0.15f;      // DlssNr::DetailStability::Blend
        bool holdRepeats = false; // an input shown before, exactly, is shown exactly as before
        bool historyValid = false;
    };

  private:
    // The Params cbuffer of dlssnr_detail_stability.hlsl, in order.
    struct alignas(256) Constants
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t HistoryValid = 0;
        uint32_t HoldRepeats = 0;
        uint32_t HaveUi = 0;
        uint32_t MotionBaseX = 0;
        uint32_t MotionBaseY = 0;
        uint32_t MotionWidth = 0;
        uint32_t MotionHeight = 0;
        float MvToWorkX = 1.0f;
        float MvToWorkY = 1.0f;
        float Alpha = 1.0f;
        float BandLow = 0.0f;
        float BandHigh = 0.0f;
    };

    FrameDescriptorHeap _frameHeaps[DLSSNR_DETAILSTABILITY_NUM_OF_HEAPS];
    ID3D12Resource* _constantBuffers[DLSSNR_DETAILSTABILITY_NUM_OF_HEAPS] = {};
    void* _mappedConstants[DLSSNR_DETAILSTABILITY_NUM_OF_HEAPS] = {};
    uint32_t _heapIndex = 0;

    // Last frame's and this frame's, swapped every dispatch, so no pixel reads a neighbour another thread is writing.
    // RGBA16F: .xyz the filtered edit, .w the input. Both rest in NON_PIXEL_SHADER_RESOURCE between dispatches.
    ID3D12Resource* _history[2] = {};
    ID3D12Resource* _out = nullptr; // what the resolve reads; rests in NON_PIXEL_SHADER_RESOURCE
    int _current = 0;

    UINT _width = 0;
    UINT _height = 0;

    ID3D12Resource* CreateTexture(ID3D12Device* device, const wchar_t* name);

  public:
    DlssNr_DetailStability_Dx12(std::string InName, ID3D12Device* InDevice, UINT InWidth, UINT InHeight);
    ~DlssNr_DetailStability_Dx12();

    // Whether this object was built for this size on this device. A failed object still fits, so it is not rebuilt, and
    // logged again, every frame.
    bool Fits(ID3D12Device* InDevice, UINT InWidth, UINT InHeight) const
    {
        return InDevice == _device && InWidth == _width && InHeight == _height;
    }

    // InAnswer is the model's answer at this object's size, readable and left so. Returns the filtered answer,
    // readable, valid until the next Filter; nullptr if this object could not be built or the pass could not be
    // recorded. Binds its own descriptor heap and root signature: the caller re-binds its own afterwards.
    ID3D12Resource* Filter(ID3D12GraphicsCommandList* InCmdList, const Frame& InFrame, ID3D12Resource* InAnswer,
                           const Settings& InSettings);
};
