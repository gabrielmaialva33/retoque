// Recording a game's NR-boundary frames for offline replay: the D3D12 side
// (docs/dlssnr/design/offline-replay-recorder.md).
//
//   Poll()        once per after-upscale evaluation, before anything can return: hands certified frames to the
//                 writer and closes a finished clip. One relaxed load while no clip was ever asked for.
//   Wanted()      a clip is requested or recording: the caller then describes the frame and calls AtBoundary.
//   AtBoundary()  records this frame's copies on the game's list, before the pass writes the output.
//
// The menu's side is Request() and GetStatus(). Nothing here allocates, copies or starts a thread until a first
// request.

#pragma once

#include "DlssNr_ReplayRecorder.h"

#include <d3d12.h>

#include <cstdint>
#include <string>

namespace DlssNr::ReplayRecorder
{
struct BoundaryFacts
{
    bool afterUpscaleD3D12 = true;
    bool exposureTextureBound = false;
    FrameMeta meta;       // the parameter block and present counter; planes, id and clock are filled here
    std::string settings; // TimingContract, for the manifest's provenance (first frame only)
};

struct Status
{
    Phase phase = Phase::Idle;
    const char* reason = nullptr;
    uint32_t recorded = 0;
    uint32_t written = 0;
    std::string folder; // the clip's folder, once known
};

// False, with why, when a clip is already in progress.
bool Request(const char** why);
Status GetStatus();
bool Wanted();
void Poll();
void AtBoundary(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* color, D3D12_RESOURCE_STATES colorState,
                ID3D12Resource* depth, D3D12_RESOURCE_STATES depthState, ID3D12Resource* motion,
                D3D12_RESOURCE_STATES motionState, const BoundaryFacts& facts);
} // namespace DlssNr::ReplayRecorder
