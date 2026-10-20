// --replay-nr PLAN: recorded NR-boundary frames through the production seam
// (docs/dlssnr/design/offline-replay-seam.md).
//
// The plan is a line-oriented rendering of a capture.json (docs/dlssnr/design/offline-replay.md), written by
// run_replay.py after it validated the capture: the harness parses numbers and paths, not JSON. Every frame is
// uploaded into real textures, described in a parameter block allocated by the production DLL, handed to
// RetoqueReplay_Evaluate, executed, fenced and read back. One REPLAY line per frame says what the pass did and how
// long the GPU took over it; the output is written raw beside the log.
#pragma once

#include "../../retoque/dlssnr/DlssNr_Replay.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ReplayNr
{
using Frame = DlssNr::Replay::Frame;
using Outcome = DlssNr::Replay::Outcome;
using PFN_Version = uint32_t (*)();
using PFN_Begin = int (*)();
using PFN_Evaluate = int (*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*, ID3D12CommandQueue*, const Frame*,
                             Outcome*);
using PFN_End = void (*)();

struct PlanFrame
{
    unsigned id = 0;
    unsigned long long present = 0;
    double timeMs = 0.0;
    double deltaMs = 0.0; // the frame time the game passed, recorded
    int reset = 0;
    float jitterX = 0.0f, jitterY = 0.0f, preExposure = 1.0f;
    std::string color, depth, motion;
};

struct Plan
{
    unsigned width = 0, height = 0, createFlags = 0;
    std::vector<PlanFrame> frames;
};

static Plan LoadPlan(const char* path)
{
    FILE* file = nullptr;
    Require(fopen_s(&file, path, "r") == 0 && file, "replay plan open");
    Plan plan;
    unsigned version = 0, count = 0;
    Require(fscanf_s(file, "replay-plan %u %u %u %u %u", &version, &plan.width, &plan.height, &plan.createFlags,
                     &count) == 5,
            "replay plan header");
    Require(version == 2 && plan.width >= 64 && plan.height >= 64 && plan.width <= 16384 && plan.height <= 16384 &&
                count > 0 && count <= 100000,
            "replay plan header values");
    for (unsigned i = 0; i < count; ++i)
    {
        PlanFrame f;
        char color[1024] {}, depth[1024] {}, motion[1024] {};
        Require(fscanf_s(file, "%u %llu %lf %lf %d %f %f %f %1023s %1023s %1023s", &f.id, &f.present, &f.timeMs,
                         &f.deltaMs, &f.reset, &f.jitterX, &f.jitterY, &f.preExposure, color, (unsigned) sizeof(color),
                         depth, (unsigned) sizeof(depth), motion, (unsigned) sizeof(motion)) == 11,
                "replay plan frame line");
        Require(f.deltaMs > 0.0 && f.deltaMs < 1000.0, "replay plan frame time out of range");
        Require(plan.frames.empty() || (f.present > plan.frames.back().present && f.timeMs > plan.frames.back().timeMs),
                "replay plan presents and times must strictly increase");
        f.color = color;
        f.depth = depth;
        f.motion = motion;
        plan.frames.push_back(std::move(f));
    }
    fclose(file);
    return plan;
}

struct Host
{
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    ID3D12CommandAllocator* alloc;
    ID3D12GraphicsCommandList* list;
    ID3D12Fence* fence;
    HANDLE event;
    UINT64 serial = 0, hz = 0;
    Ngx ngx;
    NVSDK_NGX_Parameter* params = nullptr;
    PFN_Begin begin = nullptr;
    PFN_Evaluate evaluate = nullptr;
    PFN_End end = nullptr;
    bool session = false;
    bool anyRecorded = false; // whether any evaluation recorded the pass (teardown, below)
    const char* stage = "setup";
    unsigned evaluations = 0;

    // Planes, in the order the plan names them, then the output the pass writes in place.
    enum Plane
    {
        ColorIn,
        DepthIn,
        MotionIn,
        Output,
        PlaneCount
    };
    struct Surface
    {
        Com<ID3D12Resource> texture, upload;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT64 bytes = 0;
        unsigned pixelBytes = 0;
        D3D12_RESOURCE_STATES rest {};
    };
    std::array<Surface, PlaneCount> surfaces;
    Com<ID3D12Resource> readback, timing;
    Com<ID3D12QueryHeap> queries;

    void Begin()
    {
        Check(alloc->Reset(), "replay allocator Reset");
        Check(list->Reset(alloc, nullptr), "replay list Reset");
    }
    void Submit() { ColdNr::Submit(device, queue, list, fence, event, serial); }

    void Buffer(Com<ID3D12Resource>& resource, UINT64 bytes, D3D12_HEAP_TYPE heapType)
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = heapType;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                              heapType == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                                 : D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&resource)),
              "replay buffer");
    }

    void Make(Plane plane, DXGI_FORMAT format, unsigned pixelBytes, D3D12_RESOURCE_FLAGS flags,
              D3D12_RESOURCE_STATES rest, unsigned width, unsigned height)
    {
        auto& s = surfaces[plane];
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = flags;
        Check(
            device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, rest, nullptr, IID_PPV_ARGS(&s.texture)),
            "replay texture");
        device->GetCopyableFootprints(&desc, 0, 1, 0, &s.footprint, nullptr, nullptr, &s.bytes);
        s.pixelBytes = pixelBytes;
        s.rest = rest;
        Buffer(s.upload, s.bytes, D3D12_HEAP_TYPE_UPLOAD);
    }

    // A raw plane file: tight rows, exactly height * width * pixelBytes bytes (run_replay.py checked the digest).
    void Load(Plane plane, const std::string& path, unsigned width, unsigned height)
    {
        auto& s = surfaces[plane];
        FILE* file = nullptr;
        Require(fopen_s(&file, path.c_str(), "rb") == 0 && file, "replay plane open");
        void* mapped = nullptr;
        const D3D12_RANGE noRead { 0, 0 };
        Check(s.upload->Map(0, &noRead, &mapped), "replay upload Map");
        bool ok = true;
        for (unsigned y = 0; y < height && ok; ++y)
            ok = fread(static_cast<char*>(mapped) + s.footprint.Offset + y * s.footprint.Footprint.RowPitch,
                       s.pixelBytes, width, file) == width;
        ok = ok && fgetc(file) == EOF;
        s.upload->Unmap(0, nullptr);
        fclose(file);
        Require(ok, "replay plane size does not match the plan");
    }

    void Upload(ID3D12Resource* target, const Surface& from, D3D12_RESOURCE_STATES rest)
    {
        ColdNr::Transition(list, target, rest, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
        src.pResource = from.upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = from.footprint;
        dst.pResource = target;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        ColdNr::Transition(list, target, D3D12_RESOURCE_STATE_COPY_DEST, rest);
    }

    void Init(const char* dllPath, const Plan& plan)
    {
        ngx = LoadNgx(dllPath);
        const auto version = reinterpret_cast<PFN_Version>(GetProcAddress(ngx.dll, "RetoqueReplay_Version"));
        begin = reinterpret_cast<PFN_Begin>(GetProcAddress(ngx.dll, "RetoqueReplay_Begin"));
        evaluate = reinterpret_cast<PFN_Evaluate>(GetProcAddress(ngx.dll, "RetoqueReplay_Evaluate"));
        end = reinterpret_cast<PFN_End>(GetProcAddress(ngx.dll, "RetoqueReplay_End"));
        Require(version && begin && evaluate && end, "this OptiScaler build has no replay adapter exports");
        Require(version() == 1, "replay adapter ABI version is not 1");
        ColdNr::NgxResult("replay production Init", ngx.Init(0x1337, L".", device, nullptr, NVSDK_NGX_Version_API));
        ColdNr::NgxResult("replay AllocateParameters", ngx.AllocateParameters(&params));
        params->Set(NVSDK_NGX_Parameter_Width, plan.width);
        params->Set(NVSDK_NGX_Parameter_Height, plan.height);
        params->Set(NVSDK_NGX_Parameter_OutWidth, plan.width);
        params->Set(NVSDK_NGX_Parameter_OutHeight, plan.height);
        params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, plan.createFlags);
        params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        params->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f); // v1 planes hold output pixels
        params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
        params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, plan.width);
        params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, plan.height);

        constexpr auto Srv =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        Make(ColorIn, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, D3D12_RESOURCE_FLAG_NONE, Srv, plan.width, plan.height);
        Make(DepthIn, DXGI_FORMAT_R32_FLOAT, 4, D3D12_RESOURCE_FLAG_NONE, Srv, plan.width, plan.height);
        Make(MotionIn, DXGI_FORMAT_R16G16_FLOAT, 4, D3D12_RESOURCE_FLAG_NONE, Srv, plan.width, plan.height);
        // The pass reads and writes the upscaler's output in place; it arrives in UNORDERED_ACCESS, as DLSS leaves it.
        Make(Output, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, plan.width, plan.height);
        Buffer(readback, surfaces[Output].bytes, D3D12_HEAP_TYPE_READBACK);
        D3D12_QUERY_HEAP_DESC qd {};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = 2;
        Check(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&queries)), "replay timestamp heap");
        Buffer(timing, sizeof(UINT64) * 2, D3D12_HEAP_TYPE_READBACK);
        Check(queue->GetTimestampFrequency(&hz), "replay timestamp frequency");
        Require(hz != 0, "zero GPU timestamp frequency");

        Require(begin() == 1, "RetoqueReplay_Begin refused: the process needs RETOQUE_REPLAY=1");
        session = true;
    }

    void Step(const Plan& plan, size_t index)
    {
        const auto& f = plan.frames[index];
        stage = "upload/evaluate/readback";
        Load(ColorIn, f.color, plan.width, plan.height);
        Load(DepthIn, f.depth, plan.width, plan.height);
        Load(MotionIn, f.motion, plan.width, plan.height);
        Begin();
        Upload(surfaces[ColorIn].texture, surfaces[ColorIn], surfaces[ColorIn].rest);
        Upload(surfaces[DepthIn].texture, surfaces[DepthIn], surfaces[DepthIn].rest);
        Upload(surfaces[MotionIn].texture, surfaces[MotionIn], surfaces[MotionIn].rest);
        // The colour as the upscaler wrote it: the same bytes go to Output, which the pass edits in place.
        Upload(surfaces[Output].texture, surfaces[ColorIn], surfaces[Output].rest);

        params->Set(NVSDK_NGX_Parameter_Color, surfaces[ColorIn].texture.p);
        params->Set(NVSDK_NGX_Parameter_Output, surfaces[Output].texture.p);
        params->Set(NVSDK_NGX_Parameter_Depth, surfaces[DepthIn].texture.p);
        params->Set(NVSDK_NGX_Parameter_MotionVectors, surfaces[MotionIn].texture.p);
        params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, f.jitterX);
        params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, f.jitterY);
        params->Set(NVSDK_NGX_Parameter_Reset, f.reset);
        params->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, f.preExposure);
        params->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, static_cast<float>(f.deltaMs));

        Frame frame;
        frame.present = f.present;
        frame.timeMs = f.timeMs;
        Outcome outcome;
        list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        const int entered = evaluate(list, params, queue, &frame, &outcome);
        list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, timing, 0);
        auto& out = surfaces[Output];
        ColdNr::Transition(list, out.texture, out.rest, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
        src.pResource = out.texture;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = out.footprint;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        ColdNr::Transition(list, out.texture, D3D12_RESOURCE_STATE_COPY_SOURCE, out.rest);
        Submit();
        Require(entered == 1, "RetoqueReplay_Evaluate refused the frame");
        ++evaluations;
        anyRecorded = anyRecorded || outcome.recorded != 0;
        Check(device->GetDeviceRemovedReason(), "replay device health");

        UINT64* ticks = nullptr;
        const D3D12_RANGE read { 0, sizeof(UINT64) * 2 };
        Check(timing->Map(0, &read, reinterpret_cast<void**>(&ticks)), "replay timing Map");
        const double ms =
            ticks[1] >= ticks[0] ? static_cast<double>(ticks[1] - ticks[0]) * 1000.0 / static_cast<double>(hz) : -1.0;
        const D3D12_RANGE noWrite { 0, 0 };
        timing->Unmap(0, &noWrite);

        void* mapped = nullptr;
        const D3D12_RANGE all { 0, static_cast<SIZE_T>(out.bytes) };
        Check(readback->Map(0, &all, &mapped), "replay readback Map");
        FILE* file = nullptr;
        const auto path = "f" + std::to_string(f.id) + ".output";
        Require(fopen_s(&file, path.c_str(), "wb") == 0 && file, "replay output open");
        bool ok = true;
        for (unsigned y = 0; y < plan.height; ++y)
            ok = fwrite(static_cast<char*>(mapped) + out.footprint.Offset + y * out.footprint.Footprint.RowPitch,
                        out.pixelBytes, plan.width, file) == plan.width &&
                 ok;
        readback->Unmap(0, &noWrite);
        ok = fclose(file) == 0 && ok;
        Require(ok, "replay output write");
        Say("REPLAY frame=%u present=%llu gpu_ms=%.6f recorded=%u carried=%u interval=%u requested=%u completed=%u "
            "result=%d skip=%s\n",
            f.id, f.present, ms, outcome.recorded, outcome.carried, outcome.interval, outcome.requestedPasses,
            outcome.completedPasses, outcome.modelResult, outcome.skip[0] ? outcome.skip : "-");
        PumpMessages();
    }

    void Finish()
    {
        stage = "teardown after fence";
        if (session)
            end();
        session = false;
        // One known exception, as observed: in a run where the pass never recorded (the NR-off control),
        // OptiScaler's DestroyParameters answers 0xBAD00000 for this block. Its cause is not established; it is
        // accepted only there, reported, and every other result must be success.
        const auto destroyed = ngx.DestroyParameters(params);
        params = nullptr;
        const bool known = !anyRecorded && static_cast<unsigned>(destroyed) == 0xBAD00000u;
        Say("  replay DestroyParameters -> 0x%08X%s\n", static_cast<unsigned>(destroyed),
            known ? " (known: no evaluation recorded the pass)" : "");
        Require(destroyed == NVSDK_NGX_Result_Success || known, "replay DestroyParameters");
        ColdNr::NgxResult("replay Shutdown", ngx.Shutdown(device));
    }
};

static void Run(const char* dllPath, const char* planPath, ID3D12Device* device, ID3D12CommandQueue* queue,
                ID3D12CommandAllocator* alloc, ID3D12GraphicsCommandList* list, ID3D12Fence* fence, HANDLE event)
{
    // Keep all GPU-owned objects outside the try block. A failed/unknown fence cannot unwind them.
    Host host { device, queue, alloc, list, fence, event };
    try
    {
        const Plan plan = LoadPlan(planPath);
        host.Init(dllPath, plan);
        for (size_t i = 0; i < plan.frames.size(); ++i)
            host.Step(plan, i);
        host.Finish();
        Say("REPLAY PASS: frames=%u\n", host.evaluations);
    }
    catch (const std::exception& e)
    {
        Say("REPLAY FAIL stage=%s evaluations=%u serial=%llu: %s\n", host.stage, host.evaluations, host.serial,
            e.what());
        ExitProcess(1);
    }
}
} // namespace ReplayNr
