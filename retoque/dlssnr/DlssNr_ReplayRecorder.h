// Recording a game's NR-boundary frames for offline replay: the pure half
// (docs/dlssnr/design/offline-replay-recorder.md).
//
// One request records a short clip: every evaluation's colour, depth and motion vectors as the pass receives them,
// copied raw on the game's own command list, read back only once a certificate proves the GPU past the copies, and
// written by a thread. This header holds what runs without a GPU (tests/nr-replay-recorder): the session's state
// machine over an abstract backend, the refusals, the ring's backpressure, the contiguous-prefix rule and the
// capture manifest (schema v2). DlssNr_ReplayRecorder.cpp is the D3D12 backend and the writer.

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace DlssNr::ReplayRecorder
{
// --- Bounds -------------------------------------------------------------------------------------------------------

constexpr size_t kSlots = 6;         // readback slots in flight; a full ring ends the clip, never skips a frame
constexpr uint32_t kMaxFrames = 360; // frames per clip, whatever the frame rate
constexpr double kClipMs = 6000.0;   // steady-clock length of a clip
constexpr double kWarmupMs = 1500.0; // frames before this are replay warmup: settle gates and model creation
constexpr double kFreeSpaceFactor = 1.25;

// --- What is recorded ---------------------------------------------------------------------------------------------

// One plane as the game handed it: its DXGI format (made typed for the copy) and extent. Rows are written tight.
struct Plane
{
    uint32_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pixelBytes = 0;
    uint64_t RowBytes() const { return uint64_t(width) * pixelBytes; }
    uint64_t Bytes() const { return RowBytes() * height; }
};

// The frame as EvaluateAfterUpscale received it: the parameter block GatherFrame reads, the clocks and the planes.
struct FrameMeta
{
    uint32_t id = 0;      // 1-based within the clip
    uint64_t present = 0; // State::frameCount
    double timeMs = 0.0;  // steady clock since the clip's first frame
    bool rayReconstruction = false;
    uint32_t createFlags = 0;
    bool reset = false;
    float jitterX = 0.0f, jitterY = 0.0f;
    float mvScaleX = 1.0f, mvScaleY = 1.0f;
    float preExposure = 1.0f;
    float frameTimeMs = 0.0f;
    uint32_t renderWidth = 0, renderHeight = 0;
    uint32_t depthBaseX = 0, depthBaseY = 0, motionBaseX = 0, motionBaseY = 0;
    Plane color, depth, motion;
};

// What the writer did with each frame, for the manifest: only fully written frames are listed.
struct WrittenFrame
{
    FrameMeta meta;
    bool colorWritten = false, depthWritten = false, motionWritten = false; // the writer put the plane on disk
};

struct Provenance
{
    std::string build;    // BuildInfo
    std::string exe;      // the game executable's stem
    std::string model;    // NgxInfo: model file identity
    std::string gpu;      // adapter name
    std::string settings; // TimingContract
};

// --- Refusals -----------------------------------------------------------------------------------------------------

struct RefusalInputs
{
    bool afterUpscaleD3D12 = true; // the route the seam replays
    bool exposureTextureBound = false;
    bool planesUsable = true; // single-sample 2D textures in formats the copy accepts
    uint64_t freeBytes = 0;
    uint64_t estimateBytes = 0;
};

// Null when a clip may start. The first condition the user can do least about is named.
inline const char* RefusalFor(const RefusalInputs& in)
{
    if (!in.afterUpscaleD3D12)
        return "recording needs the D3D12 after-upscale route (the one offline replay reproduces)";
    if (!in.planesUsable)
        return "the colour, depth or motion texture is not a single-sample 2D texture the copy can take";
    if (in.exposureTextureBound)
        return "the game binds an exposure texture, which replay v1 does not transport";
    if (double(in.freeBytes) < double(in.estimateBytes) * kFreeSpaceFactor)
        return "not enough free disk space beside the DLL for a clip";
    return nullptr;
}

// --- The session --------------------------------------------------------------------------------------------------

enum class Phase : uint8_t
{
    Idle,
    Requested, // the next eligible evaluation starts the clip
    Recording,
    Finishing, // no more copies; waiting for the ones in flight
    Done,
    Failed,
};

enum class Certificate : uint8_t
{
    Pending,   // not yet answerable
    Accepted,  // Reset, one execution, completed fence: the bytes are the frame's
    Discarded, // settled without proof (cancelled, orphaned): free the slot, never read it
    Failed,    // device lost or unknown execution: the slot may still be in use; the clip ends
};

// Backend contract (D3D12 in the DLL, fakes in the host test):
//   bool Available(size_t slot)            the writer is done with the slot's buffers (disk keeps up)
//   bool Copy(size_t slot, Recording&)       record this frame's copies into the slot on the game's list; false: none
//   Certificate Certify(size_t slot)
//   void Hand(size_t slot, const FrameMeta&) read the slot and pass the frame to the writer; the slot is free after
//   void Release(size_t slot)                free a slot without reading it (only after Accepted or Discarded)
//   void Finish(uint32_t frames, const char* stopped)  close the clip: frames is the contiguous count
template <class Backend> class Session
{
  public:
    Phase Current() const { return _phase; }
    const char* Reason() const { return _reason; }
    uint32_t Recorded() const { return _recorded; }
    uint32_t Handed() const { return _handed; }

    // From the menu or the trigger file. A clip in progress is not restarted.
    bool Request()
    {
        if (_phase == Phase::Requested || _phase == Phase::Recording || _phase == Phase::Finishing)
            return false;
        *this = Session {};
        _phase = Phase::Requested;
        return true;
    }

    // At the NR boundary, before the pass writes. refusal comes from RefusalFor, evaluated by the caller only while a
    // clip is wanted. meta is completed here (id, clock, reset of the first frame).
    template <class Recording>
    void AtBoundary(Backend& backend, Recording& recording, FrameMeta meta, double nowMs, const char* refusal)
    {
        if (_phase == Phase::Requested)
        {
            if (refusal != nullptr)
            {
                Fail(refusal);
                return;
            }
            _phase = Phase::Recording;
            _startMs = nowMs;
        }
        if (_phase != Phase::Recording)
            return;
        if (refusal != nullptr)
            return Stop(refusal);
        if (_recorded >= kMaxFrames || nowMs - _startMs >= kClipMs)
            return Stop("the clip reached its length");
        size_t slot = kSlots;
        for (size_t i = 0; i < kSlots; ++i)
            if (!_busy[i] && backend.Available(i))
            {
                slot = i;
                break;
            }
        if (slot == kSlots)
            return Stop("every readback slot was still in flight (the GPU or the writer fell behind)");
        meta.id = _recorded + 1;
        meta.timeMs = nowMs - _startMs;
        meta.reset = meta.reset || _recorded == 0; // a replay starts from an empty history
        if (!backend.Copy(slot, recording))
            return Stop("recording the copies failed");
        _busy[slot] = true;
        _queue.push_back({ slot, meta });
        ++_recorded;
    }

    // Once per evaluation, before anything can return. Hands certified frames to the writer in order; the first
    // frame that cannot be certified ends the clip at the frames before it.
    void Poll(Backend& backend)
    {
        while (!_queue.empty())
        {
            auto& [slot, meta] = _queue.front();
            const Certificate c = backend.Certify(slot);
            if (c == Certificate::Pending)
                break;
            if (c == Certificate::Failed)
            {
                // The slot may still be in use: keep it busy forever, end the clip.
                _queue.pop_front();
                _broken = true;
                Stop("a recording's completion could not be proven");
                continue;
            }
            if (c == Certificate::Accepted && !_broken)
            {
                backend.Hand(slot, meta);
                ++_handed;
            }
            else
            {
                if (c == Certificate::Discarded && !_broken)
                {
                    _broken = true;
                    Stop("a recording was cancelled or released before it could be read");
                }
                backend.Release(slot);
            }
            _busy[slot] = false;
            _queue.pop_front();
        }
        if (_phase == Phase::Finishing && _queue.empty())
        {
            backend.Finish(_handed, _reason);
            if (_handed == 0)
                Fail(_reason != nullptr ? _reason : "no frame was recorded");
            else
                _phase = Phase::Done;
        }
    }

  private:
    void Stop(const char* why)
    {
        if (_phase == Phase::Recording)
        {
            _phase = Phase::Finishing;
            _reason = why;
        }
    }
    void Fail(const char* why)
    {
        _phase = Phase::Failed;
        _reason = why;
    }

    Phase _phase = Phase::Idle;
    const char* _reason = nullptr;
    double _startMs = 0.0;
    uint32_t _recorded = 0;
    uint32_t _handed = 0;
    bool _broken = false;
    std::array<bool, kSlots> _busy {};
    std::deque<std::pair<size_t, FrameMeta>> _queue;
};

// --- The manifest (offline-replay.md, capture schema v2) ------------------------------------------------------------

inline std::string Escape(const std::string& text)
{
    std::string out;
    for (const char c : text)
    {
        if (c == '"' || c == '\\')
            out += '\\', out += c;
        else if (static_cast<unsigned char>(c) < 0x20)
        {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += buffer;
        }
        else
            out += c;
    }
    return out;
}

// JSON numbers must be finite; a nonfinite value is written as 0 and the manifest is refused by the caller first.
inline std::string Number(double value)
{
    if (!std::isfinite(value))
        return "0";
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.9g", value);
    return buffer;
}

inline bool Finite(const FrameMeta& m)
{
    for (const double v : { m.timeMs, double(m.jitterX), double(m.jitterY), double(m.mvScaleX), double(m.mvScaleY),
                            double(m.preExposure), double(m.frameTimeMs) })
        if (!std::isfinite(v))
            return false;
    return m.preExposure > 0.0f;
}

constexpr uint32_t kFlagMvLowRes = 1u << 1, kFlagMvJittered = 1u << 2, kFlagDepthInverted = 1u << 3;

// The recorder writes no hashes (its writer must keep pace with the game): sha256 is null and the capture says
// "finalized": false until tools/replay/finalize.py hashes every plane.
inline std::string PlaneJson(const Plane& p, const std::string& path)
{
    return "{\"path\":\"" + Escape(path) + "\",\"encoding\":\"raw-dxgi\",\"dxgi_format\":" + std::to_string(p.format) +
           ",\"width\":" + std::to_string(p.width) + ",\"height\":" + std::to_string(p.height) +
           ",\"pixel_bytes\":" + std::to_string(p.pixelBytes) + ",\"row_pitch\":" + std::to_string(p.RowBytes()) +
           ",\"sha256\":null}";
}

// The clip's manifest. Frames are the contiguous certified prefix, in order. stopped says why the clip ended.
inline std::string CaptureJson(const std::vector<WrittenFrame>& frames, const Provenance& prov, const char* stopped)
{
    const uint32_t flags = frames.empty() ? 0u : frames.front().meta.createFlags;
    std::string s = "{\n\"schema\":\"retoque-replay-capture\",\"version\":2,\"complete\":true,\"finalized\":false,"
                    "\"mode\":\"nr-boundary\",";
    s += "\"source\":\"game:" + Escape(prov.exe) + "\",\"clock\":\"recorded-evaluation\",";
    s += "\"color_space\":\"" + std::string(frames.empty() || (flags & 1u) ? "linear-rec709" : "srgb-rec709") + "\",";
    s += "\"color_jitter\":\"removed\",";
    s += "\"depth\":{\"kind\":\"device\",\"inverted\":" + std::string(flags & kFlagDepthInverted ? "true" : "false") +
         ",\"threshold\":0.02},";
    s += "\"motion\":{\"direction\":\"current-to-previous\",\"units\":\"pixels\",\"scale\":\"per-frame\","
         "\"includes_jitter\":" +
         std::string(flags & kFlagMvJittered ? "true" : "false") + "},";
    s += "\"regions\":{},";
    s += "\"provenance\":{\"producer\":\"" + Escape(prov.build) + "\",\"shader_sha256\":\"" + std::string(64, '0') +
         "\",\"model\":\"" + Escape(prov.model) + "\",\"gpu\":\"" + Escape(prov.gpu) + "\",\"settings\":\"" +
         Escape(prov.settings) + "\",\"stopped\":\"" + Escape(stopped != nullptr ? stopped : "") + "\"},\n";
    s += "\"frames\":[";
    for (size_t i = 0; i < frames.size(); ++i)
    {
        const auto& f = frames[i];
        const auto& m = f.meta;
        const std::string stem = "f" + std::to_string(m.id);
        s += i ? ",\n" : "\n";
        s += "{\"id\":" + std::to_string(m.id) + ",\"present\":" + std::to_string(m.present) +
             ",\"time_ms\":" + Number(m.timeMs) + ",\"reset\":" + (m.reset ? "true" : "false") +
             ",\"score\":" + (m.timeMs >= kWarmupMs ? "true" : "false") + ",\"jitter\":[" + Number(m.jitterX) + "," +
             Number(m.jitterY) + "],\"pre_exposure\":" + Number(m.preExposure) +
             ",\"exposure\":null,\"exposure_value\":1,\"completion\":\"fence-complete\",";
        s += "\"params\":{\"create_flags\":" + std::to_string(m.createFlags) +
             ",\"ray_reconstruction\":" + (m.rayReconstruction ? "true" : "false") + ",\"mv_scale\":[" +
             Number(m.mvScaleX) + "," + Number(m.mvScaleY) + "],\"frame_time_ms\":" + Number(m.frameTimeMs) +
             ",\"render_subrect\":[" + std::to_string(m.renderWidth) + "," + std::to_string(m.renderHeight) +
             "],\"depth_base\":[" + std::to_string(m.depthBaseX) + "," + std::to_string(m.depthBaseY) +
             "],\"motion_base\":[" + std::to_string(m.motionBaseX) + "," + std::to_string(m.motionBaseY) + "]},";
        s += "\"color\":" + PlaneJson(m.color, stem + ".color") + ",";
        s += "\"depth\":" + PlaneJson(m.depth, stem + ".depth") + ",";
        s += "\"motion\":" + PlaneJson(m.motion, stem + ".motion") + "}";
    }
    s += "\n]\n}\n";
    return s;
}
} // namespace DlssNr::ReplayRecorder
