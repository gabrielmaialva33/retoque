#include "pch.h"

#include "DlssNr_MotionProbe_Dx12.h"
#include <Logger.h>
#include <State.h>

#include <cstring>

// The probe's bytecode, when the build carries it. Its absence is a supported state: the probe reports itself
// unavailable and no verdict forms. See dlssnr_motion_probe.hlsl for the command that makes it.
#if __has_include("precompile/DlssNr_MotionProbe_Shader.h")
#include "precompile/DlssNr_MotionProbe_Shader.h"
#define DLSSNR_MOTION_PROBE_BYTECODE 1
#else
#define DLSSNR_MOTION_PROBE_BYTECODE 0
#endif

namespace
{
void Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to)
{
    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, from, to);
    cmdList->ResourceBarrier(1, &barrier);
}

// One row of the grid in the readback: kGrid texels of four floats is 512 bytes, a multiple of the pitch D3D12
// requires a placed footprint's rows to be, so the readback is a flat array of floats.
constexpr UINT kRowPitch = DlssNr::MotionProbe::kGrid * sizeof(float) * 4;
static_assert(kRowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0, "the grid's rows must meet the copy pitch");
constexpr size_t kGridBytes = (size_t) kRowPitch * DlssNr::MotionProbe::kGrid;
static_assert(kGridBytes == DlssNr::MotionProbe::kGridFloats * sizeof(float), "the readback is the CPU's grid");
} // namespace

bool DlssNr_MotionProbe_Dx12::Available() { return DLSSNR_MOTION_PROBE_BYTECODE != 0; }

DlssNr_MotionProbe_Dx12::DlssNr_MotionProbe_Dx12(std::string name, ID3D12Device* device) : Shader_Dx12(name, device)
{
    if (device == nullptr || !Available())
        return;

    // Two inputs, one output, one constant buffer, no sampler: every read is a Load.
    if (!SetupRootSignature(device, kSrvCount, kUavCount, 1))
    {
        LOG_ERROR("[{}] Failed to setup root signature", _name);
        return;
    }

    const auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(Params));
    const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto rbDesc = CD3DX12_RESOURCE_DESC::Buffer(kGridBytes);

    for (uint32_t i = 0; i < DLSSNR_MOTION_PROBE_SLOTS; ++i)
    {
        if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&_constantBuffers[i]))))
        {
            LOG_ERROR("[{}] Failed to create constant buffer {}", _name, i);
            return;
        }

        const D3D12_RANGE noRead { 0, 0 };
        if (FAILED(_constantBuffers[i]->Map(0, &noRead, &_mappedConstants[i])) || _mappedConstants[i] == nullptr)
        {
            LOG_ERROR("[{}] Failed to map constant buffer {}", _name, i);
            return;
        }

        if (FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&_readbackBuffers[i]))))
        {
            LOG_ERROR("[{}] Failed to create readback buffer {}", _name, i);
            return;
        }

        const D3D12_RANGE readRange { 0, kGridBytes };
        if (FAILED(_readbackBuffers[i]->Map(0, &readRange, &_mappedReadback[i])) || _mappedReadback[i] == nullptr)
        {
            LOG_ERROR("[{}] Failed to map readback buffer {}", _name, i);
            return;
        }
    }

#if DLSSNR_MOTION_PROBE_BYTECODE
    if (!CreateComputePipeline(device, &_pipelineState, DlssNr_MotionProbe_cso, sizeof(DlssNr_MotionProbe_cso),
                               nullptr))
    {
        LOG_ERROR("[{}] Failed to create compute pipeline", _name);
        return;
    }
#endif

    if (!InitHeaps(device, _frameHeaps, DLSSNR_MOTION_PROBE_SLOTS))
        return;

    const auto gridDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, kGrid, kGrid, 1, 1, 1, 0,
                                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &gridDesc,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_grid))) ||
        _grid == nullptr)
    {
        LOG_ERROR("[{}] Failed to create the grid", _name);
        return;
    }
    _grid->SetName(L"DLSS-NR motion probe grid");

    for (uint32_t i = 0; i < DLSSNR_MOTION_PROBE_SLOTS; ++i)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv {};
        cbv.BufferLocation = _constantBuffers[i]->GetGPUVirtualAddress();
        cbv.SizeInBytes = sizeof(Params);
        device->CreateConstantBufferView(&cbv, _frameHeaps[i].GetCbvCPU(0));
    }

    _init = true;
}

DlssNr_MotionProbe_Dx12::~DlssNr_MotionProbe_Dx12()
{
    // Only ever deleted from the retired list, once the recordings that used it are done, or at a shutdown that
    // has already proved the same.
    if (State::Instance().isShuttingDown)
        return;

    // Shader_Dx12 releases these only for an object that finished initialising.
    if (!_init)
    {
        SAFE_RELEASE(_pipelineState);
        SAFE_RELEASE(_rootSignature);
    }

    SAFE_RELEASE(_grid);

    for (uint32_t i = 0; i < DLSSNR_MOTION_PROBE_SLOTS; ++i)
    {
        if (_constantBuffers[i] != nullptr && _mappedConstants[i] != nullptr)
            _constantBuffers[i]->Unmap(0, nullptr);
        _mappedConstants[i] = nullptr;
        SAFE_RELEASE(_constantBuffers[i]);

        if (_readbackBuffers[i] != nullptr && _mappedReadback[i] != nullptr)
        {
            const D3D12_RANGE noWrite { 0, 0 };
            _readbackBuffers[i]->Unmap(0, &noWrite);
        }
        _mappedReadback[i] = nullptr;
        SAFE_RELEASE(_readbackBuffers[i]);

        _frameHeaps[i].ReleaseHeaps();
    }
}

bool DlssNr_MotionProbe_Dx12::Record(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* depth, ID3D12Resource* motion,
                                     const DlssNr::MotionProbe::Shape& shape, uint32_t slot)
{
    if (!_init || cmdList == nullptr || depth == nullptr || motion == nullptr || _grid == nullptr ||
        shape.depthWidth == 0 || shape.depthHeight == 0 || shape.motionWidth == 0 || shape.motionHeight == 0)
    {
        return false;
    }

    slot %= DLSSNR_MOTION_PROBE_SLOTS;
    FrameDescriptorHeap& heap = _frameHeaps[slot];

    // Views are re-created every probe: the guides the game hands over can be a different texture from one frame to
    // the next. Typeless guides arrive here as the pass's typed clones.
    CreateShaderResourceView(_device, depth, heap.GetSrvCPU(0));
    CreateShaderResourceView(_device, motion, heap.GetSrvCPU(1));
    CreateUnorderedAccessView(_device, _grid, heap.GetUavCPU(0), 0);

    Params p {};
    p.depthBaseX = shape.depthBaseX;
    p.depthBaseY = shape.depthBaseY;
    p.depthWidth = shape.depthWidth;
    p.depthHeight = shape.depthHeight;
    p.motionBaseX = shape.motionBaseX;
    p.motionBaseY = shape.motionBaseY;
    p.motionWidth = shape.motionWidth;
    p.motionHeight = shape.motionHeight;
    std::memcpy(_mappedConstants[slot], &p, sizeof(p));

    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(_pipelineState);
    cmdList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    cmdList->Dispatch((kGrid + kThreads - 1) / kThreads, (kGrid + kThreads - 1) / kThreads, 1);

    // The transition doubles as the wait for the writes before the copy reads them.
    Transition(cmdList, _grid, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout {};
    layout.Footprint.Width = kGrid;
    layout.Footprint.Height = kGrid;
    layout.Footprint.Depth = 1;
    layout.Footprint.RowPitch = kRowPitch;
    layout.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;

    const CD3DX12_TEXTURE_COPY_LOCATION dst(_readbackBuffers[slot], layout);
    const CD3DX12_TEXTURE_COPY_LOCATION src(_grid, 0);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    Transition(cmdList, _grid, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
}

const float* DlssNr_MotionProbe_Dx12::Grid(uint32_t slot) const
{
    slot %= DLSSNR_MOTION_PROBE_SLOTS;
    return static_cast<const float*>(_mappedReadback[slot]);
}
