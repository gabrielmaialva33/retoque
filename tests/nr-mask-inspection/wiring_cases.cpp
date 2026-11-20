// The statements Dispatch gained for mask inspection, as written (spliced in by run.py), run in Dispatch's order
// against the production frame, constants and pass structs. What surrounds them here is a script, not the pass.
#include "retoque/dlssnr/DlssNr_MaskInspection.h"
#include "retoque/dlssnr/DlssNr_PassSettings.h"
#include "retoque/shaders/dlssnr/DlssNr_Common.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <string>
#include <vector>

static size_t g_allocations = 0;

void* operator new(std::size_t size)
{
    ++g_allocations;
    if (void* memory = std::malloc(size ? size : 1))
        return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

#define REQUIRE(condition)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (false)

namespace
{
template <class T> struct Option : std::optional<T>
{
    T fallback {};
    T value_or_default() const { return this->has_value() ? **this : fallback; }
};

struct Config
{
    Option<bool> DlssNrEnabled;
    Option<bool> DlssNrUiProtection;
    Option<std::string> DlssNrMaskInspection;
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};

struct State
{
    uint64_t frameCount = 0;
    static State& Instance()
    {
        static State state;
        return state;
    }
};

long long g_nowMs = 0;
int g_clockReads = 0;
long long NowMs()
{
    ++g_clockReads;
    return g_nowMs;
}

std::vector<std::string> g_lines;
#define LOG_INFO(format, text) g_lines.emplace_back(text)

std::mutex g_nrMutex;
std::atomic<bool> g_resetOnReturn { false };

struct
{
    bool reset = false;
    uint64_t featureGeneration = 3;
    unsigned int cadencePasses = 0;
} g_nr;

// @@GLOBALS@@

// One Dispatch, as far as the inspector's statements can tell.
struct Script
{
    bool declineEarly = false; // a return before the reset is taken
    bool declineLate = false;  // a return after the UI decision (the model refused)
    bool uiProduced = true;
    bool useProxy = false;
    bool carried = false;
    bool wrote = true;
    unsigned workWidth = 1920;
    unsigned workHeight = 1080;
    uint32_t passes = 1;
    uint32_t completed = 1;
    bool hdr = false;
    bool highlightRecovery = false;
};

bool Dispatch(const Script& script, const DlssNrFrameInfo& frame, const void* colour, const void* output)
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    const Config& cfg = *Config::Instance();

    // @@GATE@@

    if (script.declineEarly)
        return false;

    // @@RESTART@@

    if (g_resetOnReturn.exchange(false))
        g_nr.reset = true;

    static int mask;
    const int* uiAlpha = nullptr;
    if (frame.PresentSource && cfg.DlssNrUiProtection.value_or_default() && script.uiProduced)
        uiAlpha = &mask;

    // @@UI@@

    if (script.useProxy)
    {
        // @@PROXY@@

        return false;
    }
    if (script.declineLate)
        return false;

    const bool carried = script.carried;
    const unsigned workWidth = script.workWidth;
    const unsigned workHeight = script.workHeight;
    DlssNrPassSnapshot passSnapshot;
    passSnapshot.Count = script.passes;
    passSnapshot.Settings[0].SkinStructure = -1.0f;
    for (uint32_t i = 1; i < DlssNr::PassConfig::MaxPasses; ++i)
        passSnapshot.Settings[i] = { 1.0f, 1.0f, 1.0f, 0.25f * static_cast<float>(i), 0, 0, i % 2 == 0 };
    struct
    {
        uint32_t completed;
    } chain { script.completed };
    g_nr.cadencePasses = script.completed;

    DlssNrConstants resolveParams {};
    resolveParams.TransferStrength = 1.0f;
    resolveParams.MaxRatio = 2.0f;
    resolveParams.ApplyModel = 1;
    resolveParams.Passthrough = script.hdr ? 0u : 1u;
    resolveParams.HighlightRecovery = script.hdr && script.highlightRecovery ? 1u : 0u;
    const bool wrote = script.wrote;

    // @@RESOLVE@@

    return wrote;
}

bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

DlssNrFrameInfo PresentFrame()
{
    DlssNrFrameInfo frame;
    frame.PresentSource = true;
    return frame;
}

void Configure(bool enabled, const char* inspection, bool uiProtection)
{
    Config& cfg = *Config::Instance();
    cfg.DlssNrEnabled.emplace(enabled);
    cfg.DlssNrUiProtection.emplace(uiProtection);
    cfg.DlssNrMaskInspection.reset();
    if (inspection != nullptr)
        cfg.DlssNrMaskInspection.emplace(inspection);
}

// Whether that Dispatch said a line.
bool Run(const Script& script, const DlssNrFrameInfo& frame, bool inPlace = true)
{
    static int colour, scratch;
    const size_t before = g_lines.size();
    ++State::Instance().frameCount;
    Dispatch(script, frame, &colour, inPlace ? &colour : &scratch);
    return g_lines.size() != before;
}

void OffLeavesTheObserverUntouched()
{
    const DlssNr::MaskInspection::Observer untouched = g_maskInspection;
    for (int variant = 0; variant < 4; ++variant)
    {
        Configure(variant != 3, variant == 0 ? nullptr : variant == 1 ? "off" : variant == 2 ? "overlay" : "log", true);
        const size_t before = g_allocations;
        for (int i = 0; i < 500; ++i)
        {
            g_nowMs += 5000;
            Script script;
            script.declineEarly = i % 5 == 1;
            script.useProxy = i % 5 == 2;
            script.declineLate = i % 5 == 3;
            script.carried = i % 5 == 4;
            g_resetOnReturn = i % 7 == 0;
            REQUIRE(!Run(script, PresentFrame()));
        }
        REQUIRE(g_allocations == before);
    }
    REQUIRE(g_clockReads == 0 && g_lines.empty());
    REQUIRE(std::memcmp(&g_maskInspection, &untouched, sizeof(untouched)) == 0);
    REQUIRE(!g_resetOnReturn.load()); // the pass still took its own flag
}

void OnSaysWhatTheStatementsRecorded()
{
    Configure(true, "log", true);
    g_nowMs = 100000;
    State::Instance().frameCount = 40;

    // The first frame, with the UI alpha handed over.
    REQUIRE(Run(Script {}, PresentFrame()));
    for (const char* part :
         { "route d3d12-present", "frame 41", "generation 1", "model generation 3", "working 1920x1080",
           "model call recorded (1 of 1 pass)", "UI alpha: handed to pass 1",
           "pass 1 AutoMask on, SkinStructure follows LocalStructure", "luminance guard applies (2.00x)",
           "highlight recovery does not apply (the frame arrived tone mapped)" })
        REQUIRE(Has(g_lines.back(), part));
    REQUIRE(g_clockReads == 1);

    // The mask pass recorded nothing: UiProtection is on, and the model got no UI alpha.
    Script noMask;
    noMask.uiProduced = false;
    g_nowMs += 2000;
    REQUIRE(Run(noMask, PresentFrame()));
    REQUIRE(Has(g_lines.back(), "UI alpha: not handed: the UI mask pass recorded nothing"));

    Configure(true, "log", false);
    g_nowMs += 2000;
    REQUIRE(Run(Script {}, PresentFrame()));
    REQUIRE(Has(g_lines.back(), "UI alpha: not handed: UiProtection is off"));

    // A chain, a linear HDR frame with highlight recovery, a partial chain.
    Script chain;
    chain.passes = 3;
    chain.completed = 2;
    chain.hdr = true;
    chain.highlightRecovery = true;
    g_nowMs += 2000;
    REQUIRE(Run(chain, PresentFrame()));
    for (const char* part : { "generation 2", "model call recorded (2 of 3 passes)",
                              "pass 2 AutoMask off, SkinStructure 0.25", "highlight recovery applies" })
        REQUIRE(Has(g_lines.back(), part));
    REQUIRE(!Has(g_lines.back(), "pass 3"));

    // Every way out before the resolve is "nothing recorded", never the frame before it; and one decline between
    // recordings is not said.
    Script early;
    early.declineEarly = true;
    g_nowMs += 100;
    REQUIRE(!Run(early, PresentFrame()));
    g_nowMs += 2000;
    REQUIRE(Run(early, PresentFrame()));
    REQUIRE(Has(g_lines.back(), "nothing recorded for this frame") && !Has(g_lines.back(), "UI alpha"));
    Script late;
    late.declineLate = true;
    Script unwritten;
    unwritten.wrote = false;
    for (const Script& declined : { late, unwritten })
    {
        g_nowMs += 2000;
        REQUIRE(!Run(declined, PresentFrame())); // the same status as the line above
    }

    // The driver proxy.
    Script proxy;
    proxy.useProxy = true;
    g_nowMs += 2000;
    REQUIRE(Run(proxy, PresentFrame()));
    REQUIRE(Has(g_lines.back(), "unavailable with the driver proxy"));

    // The pass was found switched off: the next frame that takes the reset starts a generation and says it again.
    g_nowMs += 2000;
    REQUIRE(Run(chain, PresentFrame()));
    const uint64_t generation = g_maskInspection.Generation();
    g_nowMs += 2000;
    REQUIRE(!Run(chain, PresentFrame()));
    g_resetOnReturn = true;
    g_nr.reset = false;
    g_nowMs += 2000;
    REQUIRE(Run(chain, PresentFrame()));
    REQUIRE(g_maskInspection.Generation() == generation + 1 && g_nr.reset && !g_resetOnReturn.load());
    g_resetOnReturn = true; // a frame that returns before taking the flag does not restart anything
    REQUIRE(!Run(early, PresentFrame()));
    REQUIRE(g_maskInspection.Generation() == generation + 1 && g_resetOnReturn.load());
    g_resetOnReturn = false;

    // A carried frame says the carry's passes.
    Script carried = chain;
    carried.carried = true;
    carried.hdr = false;
    g_nowMs += 2000;
    REQUIRE(Run(carried, PresentFrame()));
    REQUIRE(Has(g_lines.back(), "carried frame (no model call; the last edit, made by 2 of 3 passes)"));

    // Every other route of the D3D12 pass is unavailable, by name, whatever the frame did.
    DlssNrFrameInfo bridge = PresentFrame();
    bridge.BridgeHost = true;
    DlssNrFrameInfo afterRR;
    afterRR.AfterRayReconstruction = true;
    const struct
    {
        DlssNrFrameInfo frame;
        bool inPlace;
        const char* name;
    } routes[] = { { bridge, true, "route d3d11-bridge" },
                   { DlssNrFrameInfo {}, false, "route before-upscale" },
                   { afterRR, true, "route after-rr" },
                   { DlssNrFrameInfo {}, true, "route after-upscale" } };
    for (const auto& route : routes)
    {
        g_nowMs += 2000;
        REQUIRE(Run(Script {}, route.frame, route.inPlace));
        REQUIRE(Has(g_lines.back(), route.name) && Has(g_lines.back(), "unavailable on this route"));
        REQUIRE(!Has(g_lines.back(), "UI alpha"));
        g_nowMs += 2000;
        REQUIRE(!Run(Script {}, route.frame, route.inPlace));
    }

    // Switched off in the ini's terms (NR disabled): nothing more, and the clock is not read.
    const int reads = g_clockReads;
    const size_t lines = g_lines.size();
    Configure(false, "log", true);
    for (int i = 0; i < 100; ++i)
    {
        g_nowMs += 2000;
        REQUIRE(!Run(Script {}, PresentFrame()));
    }
    REQUIRE(g_clockReads == reads && g_lines.size() == lines && !g_maskInspection.Live());
}
} // namespace

int main()
{
    OffLeavesTheObserverUntouched();
    OnSaysWhatTheStatementsRecorded();
    std::puts("PASS: Dispatch's mask inspection statements, as written");
    return 0;
}
