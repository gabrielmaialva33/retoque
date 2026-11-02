#pragma once

#include <d3d12.h>
#include <cstdint>
#include <array>

namespace DlssNr::GpuTiming
{
struct Metadata
{
    uint64_t evaluationId = 0;
    uint64_t presentId = 0;
    uint64_t settingsGeneration = 0;
    uint64_t contractHash = 0;
    char modelIdentity[128] {};
    const char* stage = "unknown"; // Canonicalized to static before/after/after-fallback/after-rr/unknown labels.
    uint32_t renderWidth = 0, renderHeight = 0;
    uint32_t outputWidth = 0, outputHeight = 0;
    uint32_t modelWidth = 0, modelHeight = 0;
    uint32_t requestedPasses = 1;
    float workingScale = 1.0f;
    bool modelReset = false;
    bool captureActive = false;
    // Model cadence (docs/dlssnr/design/model-cadence.md). A carried evaluation makes no model call by design: it
    // records no model brackets, completes zero passes, and is its own kind of sample rather than a partial chain.
    uint32_t cadence = 1;
    bool carried = false;
};

struct DiscardCount
{
    const char* reason = "unused";
    uint64_t count = 0;
};

enum class RegionKind : uint8_t
{
    Present,
    GuideCapture
};

enum class GuideSource : uint8_t
{
    Unknown,
    Game,
    SynthesizedMotion,
    Zero
};

inline const char* GuideSourceLabel(GuideSource source)
{
    switch (source)
    {
    case GuideSource::Game:
        return "game depth + motion";
    case GuideSource::SynthesizedMotion:
        return "zero depth + synthesized motion";
    case GuideSource::Zero:
        return "zero depth + motion";
    default:
        return "unknown";
    }
}

struct RegionMetadata
{
    uint64_t presentId = 0;     // Observed Present ordinal, never an engine frame ID.
    uint64_t guideSequence = 0; // Ring capture sequence; zero without a selected capture.
    uint64_t guideAgeMs = 0;    // At selection, not the age when this sample is collected.
    uint32_t guideWidth = 0, guideHeight = 0;
    uint32_t outputWidth = 0, outputHeight = 0;
    GuideSource guides = GuideSource::Unknown;
    bool boundary = false;
    bool hdr = false;
    bool nrRecorded = false; // Route reached an evaluation that recorded work; not a model-call count.
};

struct RegionSnapshot
{
    bool valid = false;
    uint64_t eligible = 0, attempted = 0, sampled = 0, accepted = 0, discarded = 0;
    uint32_t pending = 0, waitingReset = 0, waitingGpu = 0, waitingNotification = 0, retained = 0;
    uint64_t sampleId = 0, sampleAgeMs = 0;
    uint32_t sampleInterval = 1;
    double totalMs = 0.0;
    RegionMetadata metadata {};
    const char* reason = "no-sample";
};

struct Snapshot
{
    bool enabled = false;
    uint64_t runId = 0;
    uint64_t sampleAgeMs = 0;
    uint64_t collectedAgeMs = 0;
    std::array<DiscardCount, 24> reasons {};
    bool valid = false;
    uint64_t eligible = 0, attempted = 0, sampled = 0, accepted = 0, discarded = 0;
    uint32_t pending = 0;
    uint32_t waitingReset = 0, waitingGpu = 0, waitingNotification = 0, retained = 0;
    uint32_t interval = 1;
    uint32_t sampleInterval = 1;
    uint32_t actualPasses = 0;
    uint64_t sampleId = 0;
    double totalMs = 0.0, modelMs = 0.0, otherMs = 0.0;
    Metadata metadata {};
    const char* reason = "disabled";
    // The HDR present conversion's two dispatches (Conversion below): the latest confirmed sample, its own, beside the
    // evaluation's rather than inside it.
    bool conversionValid = false;
    uint64_t conversionSampleId = 0;
    uint64_t conversionAgeMs = 0;
    double conversionMs = 0.0, conversionDecodeMs = 0.0, conversionEncodeMs = 0.0;
    RegionSnapshot present {}, capture {};
};

// Collection never waits for GPU completion. Pending resources survive disable and teardown.
void Poll();
void SetEnabled(bool enabled);
Snapshot GetSnapshot();

class Evaluation
{
    void* slot = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    uint32_t finalPasses = 0;
    bool finalSuccess = false;

  public:
    Evaluation(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, const Metadata& metadata, bool enabled,
               uint32_t interval);
    ~Evaluation();
    Evaluation(const Evaluation&) = delete;
    Evaluation& operator=(const Evaluation&) = delete;
    void SetMetadata(const Metadata& metadata);
    void Reject(const char* reason);
    void FinishOnScopeExit(uint32_t actualPasses, bool success);
    void ModelBegin();
    void ModelEnd();
    void Finish(uint32_t actualPasses, bool success);
};

// The HDR present conversion's decode and encode (docs/dlssnr/design/hdr-present-conversion.md section 5.5): two
// dispatches on the present host's list, one before the NR Dispatch and one after it, so outside every Evaluation. One
// sample brackets each with its own pair of timestamps, is read back under the same proof as an evaluation, and is
// published as Snapshot::conversionMs. Sampled on its own count of eligible conversions, at the same interval.
class Conversion
{
    void* slot = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;

  public:
    Conversion(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, bool enabled, uint32_t interval);
    ~Conversion();
    Conversion(const Conversion&) = delete;
    Conversion& operator=(const Conversion&) = delete;
    void Begin();
    void End();
    // success: the decode and the encode each recorded between their brackets. A frame the pass did not run on has
    // no encode; it is neither a sample nor a discard, and its slot is freed silently. The destructor finishes
    // unsuccessfully if nothing else did.
    void Finish(bool success);
};

// One continuous GPU region. Present contains Dispatch and HDR Conversion; GuideCapture belongs to its producer
// list. Neither is added to another sample. Each kind has its own bounded pool and eligible-event sampling phase.
// Finish BEFORE closing, dropping or submitting the list. The destructor rejects an unfinished scope.
class Region
{
    void* slot = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;

  public:
    Region(ID3D12GraphicsCommandList* commandList, RegionKind kind, const RegionMetadata& metadata, bool enabled,
           uint32_t interval);
    ~Region();
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;
    void SetMetadata(const RegionMetadata& metadata);
    void Finish(bool success);
};
} // namespace DlssNr::GpuTiming
