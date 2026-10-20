#include "pch.h"

// Recording a game's NR-boundary frames for offline replay: the D3D12 backend and the writer
// (docs/dlssnr/design/offline-replay-recorder.md). The state machine is in DlssNr_ReplayRecorder.h.

#include "DlssNr_ReplayRecorderDx12.h"
#include "DlssNr_Readback.h"
#include "DlssNr_Submission.h"

#include <BuildInfo.h>
#include <Logger.h>
#include <Util.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace DlssNr::ReplayRecorder
{
namespace
{
constexpr const wchar_t* kDirectoryName = L"dlssnr-replay";

double NowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Formats a copy to a buffer takes as they are, with their pixel size. Typeless ones become typed for the copy
// (capture::TypedForCopy, and R32 / R16G16 / R32G32 here). Two-plane depth-stencil is refused: copying it as one
// plane is the invalid copy that removes the device.
uint32_t PixelBytes(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 16;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
        return 8;
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R16G16_FLOAT:
        return 4;
    default:
        return 0;
    }
}

DXGI_FORMAT Typed(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    default:
        return capture::TypedForCopy(f);
    }
}

Plane PlaneOf(ID3D12Resource* resource, bool& usable)
{
    Plane plane;
    if (resource == nullptr)
    {
        usable = false;
        return plane;
    }
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    const DXGI_FORMAT typed = Typed(desc.Format);
    plane.format = static_cast<uint32_t>(typed);
    plane.sourceFormat = static_cast<uint32_t>(desc.Format);
    plane.sourceFlags = static_cast<uint32_t>(desc.Flags);
    plane.width = static_cast<uint32_t>(desc.Width);
    plane.height = desc.Height;
    plane.pixelBytes = PixelBytes(typed);
    usable = usable && desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1 &&
             desc.DepthOrArraySize == 1 && plane.pixelBytes != 0;
    return plane;
}

struct Slot
{
    capture::Shot shots[3];
    Plane planes[3];
    Submission::Usage usage;
};

// Each job carries its own folder (and the finish its provenance): a clip asked for while the writer is still on the
// last one must not move the old frames into the new folder.
struct Job
{
    bool finish = false;
    size_t slot = 0;
    FrameMeta meta;
    uint32_t frames = 0;
    std::string stopped;
    std::filesystem::path folder;
    Provenance provenance;
};

std::mutex g_mutex; // the session, the slots and the status: menu thread, render thread
std::array<Slot, kSlots> g_slots;
std::array<std::atomic<bool>, kSlots> g_writing {}; // the writer holds a slot's buffers
std::atomic<bool> g_used { false };
std::atomic<uint32_t> g_written { 0 };
std::filesystem::path g_folder; // the current clip's, under g_mutex
Provenance g_provenance;        // the current clip's, under g_mutex
Plane g_shape[3];               // the current clip's planes: a clip keeps the shape it started with

void Write();

class Writer
{
  public:
    bool Push(Job&& job)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_)
        {
            try
            {
                // Detached and never joined, as the pair capture's writer: it outlives static destruction.
                std::thread([this] { Run(); }).detach();
            }
            catch (...)
            {
                return false;
            }
            started_ = true;
        }
        jobs_.push_back(std::move(job));
        wake_.notify_one();
        return true;
    }

  private:
    void Run();
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    bool started_ = false;
    std::vector<WrittenFrame> frames_;
};

Writer& TheWriter()
{
    static Writer* writer = new Writer();
    return *writer;
}

// Tight rows of one readback, written straight from the mapped buffer: no copy and no hash on the writer, which must
// keep up with the game (about 57 MB a frame at 3440x1440). tools/replay/finalize.py hashes the planes afterwards.
// False when the plane could not be written whole.
bool WritePlane(const std::filesystem::path& path, const capture::Shot& shot, const Plane& plane)
{
    void* mapped = nullptr;
    const D3D12_RANGE read { 0, static_cast<SIZE_T>(shot.bytes) };
    if (shot.readback == nullptr || FAILED(shot.readback->Map(0, &read, &mapped)) || mapped == nullptr)
        return false;
    const D3D12_RANGE none { 0, 0 };
    std::FILE* file = _wfopen(path.c_str(), L"wb");
    bool ok = file != nullptr;
    const auto row = static_cast<size_t>(plane.RowBytes());
    const auto* base = static_cast<const uint8_t*>(mapped) + shot.layout.Offset;
    if (ok && shot.layout.Footprint.RowPitch == row)
        ok = std::fwrite(base, 1, row * plane.height, file) == row * plane.height;
    else
        for (uint32_t y = 0; ok && y < plane.height; ++y)
            ok = std::fwrite(base + size_t(y) * shot.layout.Footprint.RowPitch, 1, row, file) == row;
    shot.readback->Unmap(0, &none);
    if (file != nullptr && std::fclose(file) != 0)
        ok = false;
    return ok;
}

void Writer::Run()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return !jobs_.empty(); });
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        std::error_code error;
        if (job.finish)
        {
            // Only the frames written in order, and only once all of them are on disk: the manifest names no file
            // that does not exist, and a crash before this leaves a folder no reader accepts.
            std::vector<WrittenFrame> frames;
            for (auto& f : frames_)
            {
                if (frames.size() + 1 != f.meta.id || !f.colorWritten || !f.depthWritten || !f.motionWritten ||
                    !Finite(f.meta))
                    break;
                frames.push_back(f);
            }
            const std::string json = CaptureJson(frames, job.provenance, job.stopped.c_str());
            const auto tmp = job.folder / L"capture.json.tmp";
            std::FILE* file = _wfopen(tmp.c_str(), L"wb");
            const bool ok = file != nullptr && std::fwrite(json.data(), 1, json.size(), file) == json.size();
            if (file != nullptr)
                std::fclose(file);
            if (ok)
                std::filesystem::rename(tmp, job.folder / L"capture.json", error);
            if (!ok || error)
                LOG_ERROR("DLSS-NR replay recorder: the clip's manifest could not be written in {}",
                          job.folder.string());
            else
                LOG_INFO("DLSS-NR replay recorder: clip of {} frames written to {} ({})", frames.size(),
                         job.folder.string(), job.stopped);
            frames_.clear();
            continue;
        }
        Slot& slot = g_slots[job.slot];
        WrittenFrame written;
        written.meta = job.meta;
        const std::string stem = "f" + std::to_string(job.meta.id);
        written.colorWritten = WritePlane(job.folder / (stem + ".color"), slot.shots[0], slot.planes[0]);
        written.depthWritten = WritePlane(job.folder / (stem + ".depth"), slot.shots[1], slot.planes[1]);
        written.motionWritten = WritePlane(job.folder / (stem + ".motion"), slot.shots[2], slot.planes[2]);
        frames_.push_back(std::move(written));
        g_written.fetch_add(1, std::memory_order_relaxed);
        g_writing[job.slot].store(false, std::memory_order_release);
    }
}

struct Recording
{
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12Resource* surface[3] = {};
    D3D12_RESOURCE_STATES state[3] = {};
    Plane planes[3];
};

struct Dx12Backend
{
    bool Available(size_t slot) { return !g_writing[slot].load(std::memory_order_acquire); }

    bool Copy(size_t index, Recording& recording)
    {
        Slot& slot = g_slots[index];
        ID3D12Device* device = nullptr;
        if (FAILED(recording.cmd->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
            return false;
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i)
        {
            const Plane& want = recording.planes[i];
            const Plane& have = slot.planes[i];
            if (slot.shots[i].readback == nullptr || have.format != want.format || have.width != want.width ||
                have.height != want.height)
            {
                // First use of the slot in this process, or a clip of another shape: a buffer sized for this plane.
                // The old one, if any, is settled (the slot is free) and released here.
                if (slot.shots[i].readback != nullptr)
                    slot.shots[i].readback->Release();
                slot.shots[i] = {};
                ok = capture::AllocReadback(device, recording.surface[i]->GetDesc(), slot.shots[i]);
                slot.planes[i] = want;
            }
        }
        device->Release();
        // Before any copy is recorded, as Submission::Track requires; a list it cannot follow ends the clip.
        if (!ok || !Submission::Track(recording.cmd, slot.usage))
            return false;
        for (int i = 0; i < 3; ++i)
            capture::CopyToReadback(recording.cmd, recording.surface[i], recording.state[i], slot.shots[i]);
        return true;
    }

    Certificate Certify(size_t index)
    {
        Slot& slot = g_slots[index];
        const auto certificate = Submission::TimingCertificate(slot.usage);
        if (certificate.accepted)
            return Certificate::Accepted;
        if (Submission::Settled(slot.usage) || (certificate.terminal && certificate.reusable))
            return Certificate::Discarded;
        return certificate.terminal ? Certificate::Failed : Certificate::Pending;
    }

    void Hand(size_t index, const FrameMeta& meta)
    {
        g_slots[index].usage = {};
        g_writing[index].store(true, std::memory_order_release);
        Job job;
        job.slot = index;
        job.meta = meta;
        job.folder = g_folder;
        if (!TheWriter().Push(std::move(job)))
            g_writing[index].store(false, std::memory_order_release); // the frame is lost; the manifest stops there
    }

    void Release(size_t index) { g_slots[index].usage = {}; }

    void Finish(uint32_t frames, const char* stopped)
    {
        Job job;
        job.finish = true;
        job.frames = frames;
        job.stopped = stopped != nullptr ? stopped : "";
        job.folder = g_folder;
        job.provenance = g_provenance;
        TheWriter().Push(std::move(job));
    }
};

Session<Dx12Backend>& TheSession()
{
    static Session<Dx12Backend>* session = new Session<Dx12Backend>();
    return *session;
}

uint64_t FreeBytes(const std::filesystem::path& folder)
{
    std::error_code error;
    const auto space = std::filesystem::space(folder, error);
    return error ? 0 : space.available;
}

std::string UtcStamp()
{
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc {};
    gmtime_s(&utc, &now);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &utc);
    return buffer;
}
} // namespace

bool Request(const char** why)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_used.store(true, std::memory_order_release);
    if (!TheSession().Request())
    {
        if (why != nullptr)
            *why = "a clip is already being recorded";
        return false;
    }
    g_written.store(0, std::memory_order_relaxed);
    g_folder.clear();
    LOG_INFO("DLSS-NR replay recorder: a clip was asked for; the next after-upscale evaluation starts it");
    return true;
}

Status GetStatus()
{
    Status status;
    if (!g_used.load(std::memory_order_acquire))
        return status;
    std::lock_guard<std::mutex> lock(g_mutex);
    status.phase = TheSession().Current();
    status.reason = TheSession().Reason();
    status.recorded = TheSession().Recorded();
    status.written = g_written.load(std::memory_order_relaxed);
    status.folder = g_folder.string();
    return status;
}

bool Wanted()
{
    if (!g_used.load(std::memory_order_relaxed))
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    const Phase phase = TheSession().Current();
    return phase == Phase::Requested || phase == Phase::Recording;
}

void Poll()
{
    if (!g_used.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Dx12Backend backend;
    TheSession().Poll(backend);
}

void AtBoundary(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* color, D3D12_RESOURCE_STATES colorState,
                ID3D12Resource* depth, D3D12_RESOURCE_STATES depthState, ID3D12Resource* motion,
                D3D12_RESOURCE_STATES motionState, const BoundaryFacts& facts)
{
    if (cmdList == nullptr)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& session = TheSession();
    const bool starting = session.Current() == Phase::Requested;

    Recording recording;
    recording.cmd = cmdList;
    recording.surface[0] = color;
    recording.surface[1] = depth;
    recording.surface[2] = motion;
    recording.state[0] = colorState;
    recording.state[1] = depthState;
    recording.state[2] = motionState;
    bool usable = true;
    for (int i = 0; i < 3; ++i)
        recording.planes[i] = PlaneOf(recording.surface[i], usable);

    FrameMeta meta = facts.meta;
    meta.color = recording.planes[0];
    meta.depth = recording.planes[1];
    meta.motion = recording.planes[2];

    if (starting)
    {
        const auto base = Util::DllPath().remove_filename() / kDirectoryName;
        g_folder = base / UtcStamp();
        std::error_code error;
        std::filesystem::create_directories(g_folder, error);
        RefusalInputs inputs;
        inputs.afterUpscaleD3D12 = facts.afterUpscaleD3D12;
        inputs.exposureTextureBound = facts.exposureTextureBound;
        inputs.planesUsable = usable && !error;
        const uint64_t perFrame = meta.color.Bytes() + meta.depth.Bytes() + meta.motion.Bytes();
        inputs.estimateBytes = perFrame * kMaxFrames;
        inputs.freeBytes = error ? 0 : FreeBytes(g_folder);
        if (const char* refusal = RefusalFor(inputs))
        {
            Dx12Backend backend;
            session.AtBoundary(backend, recording, meta, NowMs(), refusal);
            LOG_INFO("DLSS-NR replay recorder: not started: {}", refusal);
            return;
        }
        g_provenance = {};
        g_provenance.build = BuildInfo::ProductVersion();
        g_provenance.exe = Util::ExePath().stem().string();
        g_provenance.settings = facts.settings;
        for (int i = 0; i < 3; ++i)
            g_shape[i] = recording.planes[i];
        LOG_INFO("DLSS-NR replay recorder: recording up to {} frames / {:.0f} s into {} (colour {}x{} format {}, "
                 "depth {}x{} format {}, motion {}x{} format {})",
                 kMaxFrames, kClipMs / 1000.0, g_folder.string(), meta.color.width, meta.color.height,
                 meta.color.format, meta.depth.width, meta.depth.height, meta.depth.format, meta.motion.width,
                 meta.motion.height, meta.motion.format);
    }

    // A clip keeps the shape it started with; a resize mid-clip ends it at the frames before.
    const char* refusal = nullptr;
    const auto same = [](const Plane& a, const Plane& b)
    {
        return a.format == b.format && a.sourceFormat == b.sourceFormat && a.sourceFlags == b.sourceFlags &&
               a.width == b.width && a.height == b.height;
    };
    if (!usable)
        refusal = "a plane changed to a format or shape the recorder cannot copy";
    else if (!same(recording.planes[0], g_shape[0]) || !same(recording.planes[1], g_shape[1]) ||
             !same(recording.planes[2], g_shape[2]))
        refusal = "the frame changed size or format mid-clip";
    else if (facts.exposureTextureBound)
        refusal = "the game bound an exposure texture mid-clip";
    Dx12Backend backend;
    session.AtBoundary(backend, recording, meta, NowMs(), refusal);
}
} // namespace DlssNr::ReplayRecorder
