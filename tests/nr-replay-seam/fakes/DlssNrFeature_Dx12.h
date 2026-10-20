// The production entry, faked: cases.cpp scripts what the pass notes during one evaluation.
#pragma once
struct ID3D12GraphicsCommandList;
struct ID3D12CommandQueue;
struct NVSDK_NGX_Parameter;
namespace DlssNr
{
void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          ID3D12CommandQueue* timingQueue = nullptr, bool preUpscaleDeclined = false,
                          bool isRayReconstruction = false);
}
