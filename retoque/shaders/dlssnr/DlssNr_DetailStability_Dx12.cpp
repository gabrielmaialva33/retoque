#include "pch.h"

#include "DlssNr_DetailStability_Dx12.h"

#include <dlssnr/DlssNr_DetailStability.h>

#include "precompile/DlssNr_DetailStability_Shader.h"

namespace
{
void Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to)
{
    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, from, to);
    cmdList->ResourceBarrier(1, &barrier);
}

constexpr auto kRest = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr uint32_t kThreads = 8;
} // namespace

ID3D12Resource* DlssNr_DetailStability_Dx12::CreateTexture(ID3D12Device* device, const wchar_t* name)
{
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, _width, _height, 1, 1, 1, 0,
                                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    ID3D12Resource* resource = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kRest, nullptr,
                                               IID_PPV_ARGS(&resource))) ||
        resource == nullptr)
    {
        return nullptr;
    }

    resource->SetName(name);
    return resource;
}

DlssNr_DetailStability_Dx12::DlssNr_DetailStability_Dx12(std::string InName, ID3D12Device* InDevice, UINT InWidth,
                                                         UINT InHeight)
    : Shader_Dx12(InName, InDevice), _width(InWidth), _height(InHeight)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    if (InWidth == 0 || InHeight == 0)
    {
        LOG_WARN("[{}] Empty size {}x{}", _name, InWidth, InHeight);
        return;
    }

    // Ours, not the game's: the exposure scan must not take these for a candidate.
    ScopedInternalResourceCreation internalResources {};

    // Five inputs, two outputs, one constant buffer, no sampler: every read is a Load.
    if (!SetupRootSignature(InDevice, 5, 2, 1))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    // One upload buffer per slot, mapped for life: a single shared buffer is rewritten by the next frame's record while
    // the GPU may still be reading this one's.
    const auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(Constants));
    const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    for (uint32_t i = 0; i < DLSSNR_DETAILSTABILITY_NUM_OF_HEAPS; ++i)
    {
        auto result = InDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&_constantBuffers[i]));
        if (result != S_OK)
        {
            LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
            return;
        }

        const D3D12_RANGE noRead { 0, 0 };
        result = _constantBuffers[i]->Map(0, &noRead, &_mappedConstants[i]);
        if (FAILED(result) || _mappedConstants[i] == nullptr)
        {
            LOG_ERROR("[{0}] Map constants error {1:x}", _name, (unsigned int) result);
            return;
        }
    }

    if (!CreateComputePipeline(InDevice, &_pipelineState, DlssNr_DetailStability_cso,
                               sizeof(DlssNr_DetailStability_cso), nullptr))
    {
        LOG_ERROR("[{0}] Failed to create the compute pipeline", _name);
        return;
    }

    if (!InitHeaps(InDevice, _frameHeaps, DLSSNR_DETAILSTABILITY_NUM_OF_HEAPS))
        return;

    for (uint32_t i = 0; i < DLSSNR_DETAILSTABILITY_NUM_OF_HEAPS; ++i)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv {};
        cbv.BufferLocation = _constantBuffers[i]->GetGPUVirtualAddress();
        cbv.SizeInBytes = sizeof(Constants);
        InDevice->CreateConstantBufferView(&cbv, _frameHeaps[i].GetCbvCPU(0));
    }

    _history[0] = CreateTexture(InDevice, L"DLSS-NR detail stability history A");
    _history[1] = CreateTexture(InDevice, L"DLSS-NR detail stability history B");
    _out = CreateTexture(InDevice, L"DLSS-NR detail stability answer");

    if (_history[0] == nullptr || _history[1] == nullptr || _out == nullptr)
    {
        LOG_WARN("[{}] Could not allocate the filter surfaces for {}x{}", _name, _width, _height);
        return;
    }

    LOG_INFO("DLSS-NR detail stability: surfaces built for {}x{} (about {:.0f} MB)", _width, _height,
             (double) _width * _height * 24 / (1024.0 * 1024.0));
    _init = true;
}

DlssNr_DetailStability_Dx12::~DlssNr_DetailStability_Dx12()
{
    // Only ever deleted from the retired list, once the recordings that used it are done, or at a shutdown that has
    // already proved the same.
    if (State::Instance().isShuttingDown)
        return;

    // Shader_Dx12 releases these only for an object that finished initialising.
    if (!_init)
    {
        SAFE_RELEASE(_pipelineState);
        SAFE_RELEASE(_rootSignature);
    }

    for (auto& texture : _history)
        SAFE_RELEASE(texture);
    SAFE_RELEASE(_out);

    for (uint32_t i = 0; i < DLSSNR_DETAILSTABILITY_NUM_OF_HEAPS; ++i)
    {
        if (_constantBuffers[i] != nullptr && _mappedConstants[i] != nullptr)
            _constantBuffers[i]->Unmap(0, nullptr);
        _mappedConstants[i] = nullptr;
        SAFE_RELEASE(_constantBuffers[i]);
        _frameHeaps[i].ReleaseHeaps();
    }
}

ID3D12Resource* DlssNr_DetailStability_Dx12::Filter(ID3D12GraphicsCommandList* InCmdList, const Frame& InFrame,
                                                    ID3D12Resource* InAnswer, const Settings& InSettings)
{
    if (!_init || InCmdList == nullptr || _device == nullptr || InAnswer == nullptr || InFrame.proxy == nullptr ||
        InFrame.motion == nullptr)
    {
        return nullptr;
    }

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_DETAILSTABILITY_NUM_OF_HEAPS;
    FrameDescriptorHeap& heap = _frameHeaps[slot];

    ID3D12Resource* const historyPrev = _history[_current];
    ID3D12Resource* const historyOut = _history[_current ^ 1];

    Transition(InCmdList, historyOut, kRest, kWrite);
    Transition(InCmdList, _out, kRest, kWrite);

    CreateShaderResourceView(_device, InFrame.proxy, heap.GetSrvCPU(0), DXGI_FORMAT_UNKNOWN, true);
    CreateShaderResourceView(_device, InAnswer, heap.GetSrvCPU(1), DXGI_FORMAT_UNKNOWN, true);
    CreateShaderResourceView(_device, InFrame.motion, heap.GetSrvCPU(2), DXGI_FORMAT_UNKNOWN, true);
    CreateShaderResourceView(_device, historyPrev, heap.GetSrvCPU(3), DXGI_FORMAT_UNKNOWN, true);
    if (InFrame.uiAlpha != nullptr)
    {
        CreateShaderResourceView(_device, InFrame.uiAlpha, heap.GetSrvCPU(4), DXGI_FORMAT_UNKNOWN, true);
    }
    else
    {
        // A null descriptor of the slot's type: the shader never reads it without a mask.
        D3D12_SHADER_RESOURCE_VIEW_DESC empty {};
        empty.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        empty.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        empty.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        empty.Texture2D.MipLevels = 1;
        _device->CreateShaderResourceView(nullptr, &empty, heap.GetSrvCPU(4));
    }
    CreateUnorderedAccessView(_device, _out, heap.GetUavCPU(0), 0, true);
    CreateUnorderedAccessView(_device, historyOut, heap.GetUavCPU(1), 0, true);

    Constants constants {};
    constants.Width = _width;
    constants.Height = _height;
    constants.HistoryValid = InSettings.historyValid ? 1u : 0u;
    constants.HoldRepeats = InSettings.holdRepeats ? 1u : 0u;
    constants.HaveUi = InFrame.uiAlpha != nullptr ? 1u : 0u;
    constants.MotionBaseX = InFrame.motionRegion.baseX;
    constants.MotionBaseY = InFrame.motionRegion.baseY;
    constants.MotionWidth = std::max(InFrame.motionRegion.width, 1u);
    constants.MotionHeight = std::max(InFrame.motionRegion.height, 1u);
    constants.MvToWorkX = std::isfinite(InFrame.mvToWorkX) ? InFrame.mvToWorkX : 0.0f;
    constants.MvToWorkY = std::isfinite(InFrame.mvToWorkY) ? InFrame.mvToWorkY : 0.0f;
    constants.Alpha = DlssNr::DetailStability::Blend(InSettings.alpha);
    constants.BandLow = DlssNr::DetailStability::kBandLow;
    constants.BandHigh = DlssNr::DetailStability::kBandHigh;
    memcpy(_mappedConstants[slot], &constants, sizeof(constants));

    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    InCmdList->Dispatch((_width + kThreads - 1) / kThreads, (_height + kThreads - 1) / kThreads, 1);

    Transition(InCmdList, historyOut, kWrite, kRest);
    Transition(InCmdList, _out, kWrite, kRest);

    _current ^= 1;
    return _out;
}
