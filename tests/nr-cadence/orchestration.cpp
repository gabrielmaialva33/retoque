// Execute the shipped D3D12 Record/Carry/Fade/ModelMotion/Probe bodies and HLSL PixelModes on a host sink.
// The sink replaces GPU commands, not the production method decisions. It is not a D3D12/GPU oracle.
#define main ExistingRuleCases
#include "rule.cpp"
#undef main
#include "legacy_carry.h"
#include <array>
#include <cstdlib>
#include <memory>
#include <string>
#include <dlssnr/DlssNr_CadenceAdaptive.h>

struct uint2
{
    int x, y;
    uint2(int a, int b) : x(a), y(b) {}
};
struct uint3
{
    int x, y, z;
    uint2 xy;
    uint3(int a, int b) : x(a), y(b), z(0), xy(a, b) {}
};
struct int3
{
    int x, y;
    int3(uint2 p, int) : x(p.x), y(p.y) {}
};
struct float4
{
    float x, y, z, w;
    float3 rgb;
    float a;
    float4(float v = 0) : float4(v, v, v, v) {}
    float4(float xx, float yy, float zz, float ww) : x(xx), y(yy), z(zz), w(ww), rgb(xx, yy, zz), a(ww) {}
    float4(float3 c, float aa) : float4(c.x, c.y, c.z, aa) {}
    float4(float2 v, float zz, float ww) : float4(v.x, v.y, zz, ww) {}
};
float length(float2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
using UINT = unsigned;
using UINT64 = uint64_t;
constexpr int kRest = 1, kWrite = 2;
constexpr int D3D12_RESOURCE_STATE_COPY_SOURCE = 3, D3D12_RESOURCE_STATE_COPY_DEST = 4;
constexpr int D3D12_RESOURCE_BARRIER_TYPE_UAV = 1;
constexpr int D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX = 1, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT = 2;
constexpr unsigned kThreads = 8;
struct ID3D12Resource
{
    std::string name;
    Image<float4> pixels;
    bool half = false;
    int state = kRest;
    float4 Load(int3 p) const { return pixels.At(p.x, p.y); }
    Image<float3> Rgb() const
    {
        Image<float3> out;
        out.Resize(pixels.w, pixels.h);
        for (size_t i = 0; i < pixels.px.size(); ++i)
            out.px[i] = pixels.px[i].rgb;
        return out;
    }
    Image<float> Scalar() const
    {
        Image<float> out;
        out.Resize(pixels.w, pixels.h);
        for (size_t i = 0; i < pixels.px.size(); ++i)
            out.px[i] = pixels.px[i].x;
        return out;
    }
    Image<float2> Vectors() const
    {
        Image<float2> out;
        out.Resize(pixels.w, pixels.h);
        for (size_t i = 0; i < pixels.px.size(); ++i)
            out.px[i] = { pixels.px[i].x, pixels.px[i].y };
        return out;
    }
};
struct TextureView
{
    ID3D12Resource* resource = nullptr;
    float4 Load(int3 p) const
    {
        assert(resource);
        return resource->Load(p);
    }
    struct Store
    {
        ID3D12Resource* resource;
        uint2 p;
        void operator=(float4 v)
        {
            assert(resource && resource->state == kWrite);
            if (resource->half)
                v = float4(float(_Float16(v.x)), float(_Float16(v.y)), float(_Float16(v.z)), float(_Float16(v.w)));
            resource->pixels.Ref(p.x, p.y) = v;
        }
        operator float() const { return resource->pixels.At(p.x, p.y).x; }
    };
    Store operator[](uint2 p) const { return { resource, p }; }
};
TextureView gProxy, gAnswer, gOut0, gOut1, gOut2;
int gMode, gWidth, gHeight, gLowWidth, gLowHeight;
unsigned gHaveUi, gChainStart, gProbeHistory, gPassthrough;
float gFadeWeight;
CadenceParams g_params;
CadenceParams LoadParams() { return g_params; }
float DepthNowAt(int x, int y) { return g_depthNow.At(x, y); }
float2 MotionAt(int x, int y) { return g_motion.At(x, y); }
float UiAt(int x, int y) { return g_ui.At(x, y); }
float3 ProxyNowAt(int x, int y) { return g_proxyNow.At(x, y); }
float2 ChainAt(int x, int y) { return g_chain.At(x, y); }
#include "legacy_pixel_modes.h"
#include "pixel_modes.h"

struct D3D12_RESOURCE_BARRIER
{
    int Type;
    struct
    {
        ID3D12Resource* pResource;
    } UAV;
};
struct D3D12_PLACED_SUBRESOURCE_FOOTPRINT
{
};
struct D3D12_TEXTURE_COPY_LOCATION
{
    ID3D12Resource* pResource;
    int Type, SubresourceIndex;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT PlacedFootprint;
};
struct ID3D12GraphicsCommandList
{
    std::vector<unsigned> modes;
    unsigned copies = 0;
    void ResourceBarrier(int, const D3D12_RESOURCE_BARRIER* barrier)
    {
        assert(barrier->UAV.pResource->state == kWrite);
    }
    void CopyTextureRegion(D3D12_TEXTURE_COPY_LOCATION* to, int, int, int, D3D12_TEXTURE_COPY_LOCATION* from, void*)
    {
        assert(from->pResource->state == D3D12_RESOURCE_STATE_COPY_SOURCE);
        to->pResource->pixels = from->pResource->pixels;
        ++copies;
    }
};
void Transition(ID3D12GraphicsCommandList*, ID3D12Resource* r, int before, int after)
{
    assert(r && r->state == before);
    r->state = after;
}
namespace DlssNr::Submission
{
struct Usage
{
};
bool Track(ID3D12GraphicsCommandList*, Usage&) { return true; }
} // namespace DlssNr::Submission

class DlssNr_Cadence_Dx12
{
  public:
    // PRODUCTION_TYPES
    static constexpr unsigned kSrvCount = 11, kUavCount = 3;
    struct ProbeSlot
    {
        ID3D12Resource* grid = nullptr;
        ID3D12Resource* readback = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        DlssNr::Submission::Usage usage;
        DlssNr::Cadence::Adaptive::Sample sample;
        bool pending = false, ignored = false;
    };
    std::array<ProbeSlot, 4> _probes;
    bool _init = true, _smoothing = true;
    bool failShow = false;
    unsigned showCalls = 0;
    void* _device = this;
    unsigned _width, _height, _lowWidth, _lowHeight;
    int _current = 0;
    ID3D12Resource *_residual, *_proxyThen, *_depthThen, *_residualLow, *_chain[2], *_shown, *_fade;
    std::vector<std::unique_ptr<ID3D12Resource>> resources;
    ID3D12Resource* Make(const char* name, unsigned w, unsigned h, bool half = false, int state = kRest)
    {
        auto r = std::make_unique<ID3D12Resource>();
        r->name = name;
        r->half = half;
        r->state = state;
        r->pixels.Resize(w, h);
        auto* result = r.get();
        resources.push_back(std::move(r));
        return result;
    }
    DlssNr_Cadence_Dx12(unsigned w, unsigned h)
        : _width(w), _height(h), _lowWidth(DlssNr::Cadence::LowExtent(w)), _lowHeight(DlssNr::Cadence::LowExtent(h))
    {
        _residual = Make("residual", w, h, true);
        _proxyThen = Make("previous proxy", w, h, true);
        _depthThen = Make("previous depth", w, h);
        _residualLow = Make("coarse", _lowWidth, _lowHeight, true);
        _chain[0] = Make("chain0", w, h);
        _chain[1] = Make("chain1", w, h);
        _shown = Make("shown", w, h, true);
        _fade = Make("fade", w, h, true);
    }
    Constants Base(uint32_t, const Frame&) const;
    bool Record(ID3D12GraphicsCommandList*, const Frame&, ID3D12Resource*);
    bool Carry(ID3D12GraphicsCommandList*, const Frame&, bool, ID3D12Resource*, const Smooth&, ID3D12Resource*);
    ID3D12Resource* ModelMotion(ID3D12GraphicsCommandList*, const Frame&);
    bool Fade(ID3D12GraphicsCommandList*, const Frame&, ID3D12Resource*, ID3D12Resource*, const Smooth&);
    void Probe(ID3D12GraphicsCommandList*, const Frame&, ID3D12Resource*, bool, bool, uint64_t, int64_t);
    bool Show(ID3D12GraphicsCommandList*, const Frame&, ID3D12Resource*, const Smooth&, ID3D12Resource*)
    {
        ++showCalls;
        if (failShow)
            return false; // downstream failure after the production Carry already wrote chain/shown
        assert(false && "light limiter covered separately, this fixture tests the history/fade boundary");
        return false;
    }
    bool InitProbe(ProbeSlot& p)
    {
        if (!p.grid)
        {
            p.grid = Make("probe", 16, 16);
            p.readback = Make("readback", 16, 16);
        }
        return true;
    }
    bool Run(ID3D12GraphicsCommandList* cmd, const Constants& c, ID3D12Resource* const (&s)[kSrvCount],
             ID3D12Resource* const (&u)[kUavCount], UINT gx, UINT gy, ProbeSlot* probe = nullptr)
    {
        (void) probe;
        cmd->modes.push_back(c.Mode);
        g_params = Shipped();
        g_params.width = c.Width;
        g_params.height = c.Height;
        g_params.lowWidth = c.LowWidth;
        g_params.lowHeight = c.LowHeight;
        g_params.lowBlock = c.LowBlock;
        g_params.haveUi = c.HaveUi;
        g_params.depthTol = c.DepthTol;
        g_params.colourTol = c.ColourTol;
        g_params.scale = c.Scale;
        g_params.depthStepX = c.DepthStepX;
        g_params.depthStepY = c.DepthStepY;
        gMode = c.Mode;
        gWidth = c.Width;
        gHeight = c.Height;
        gLowWidth = c.LowWidth;
        gLowHeight = c.LowHeight;
        gHaveUi = c.HaveUi;
        gChainStart = c.ChainStart;
        gProbeHistory = c.ProbeHistory;
        gPassthrough = c.Passthrough;
        gFadeWeight = c.FadeWeight;
        gProxy.resource = s[1];
        gAnswer.resource = s[0];
        if (s[1])
            g_proxyNow = s[1]->Rgb();
        if (s[2])
            g_depthNow = s[2]->Scalar();
        if (s[3])
            g_motion = s[3]->Vectors();
        if (s[4])
            g_chain = s[4]->Vectors();
        if (s[5])
            g_depthThen = s[5]->Scalar();
        if (s[6])
            g_proxyThen = s[6]->Rgb();
        if (s[7])
            g_residual = s[7]->Rgb();
        if (s[8])
            g_low = s[8]->Rgb();
        if (s[9])
            g_ui = s[9]->Scalar();
        if (s[10])
            g_fade = s[10]->Rgb();
        gOut0.resource = u[0];
        gOut1.resource = u[1];
        gOut2.resource = u[2];
        ID3D12Resource expected;
        if (c.Mode == 9)
        {
            // The probe's result must be independent of residual/coarse contents, including nonfinite data.
            if (!s[7])
                g_residual.Resize(c.Width, c.Height, { NAN, NAN, NAN });
            if (!s[8])
                g_low.Resize(c.LowWidth, c.LowHeight, { NAN, NAN, NAN });
            expected.state = kWrite;
            expected.pixels.Resize(16, 16);
            gOut0.resource = &expected;
            for (unsigned y = 0; y < 16; ++y)
                for (unsigned x = 0; x < 16; ++x)
                    LegacyPixelModes(uint3(x, y));
            gOut0.resource = u[0];
        }
        const size_t before = g_residualReads + g_lowReads;
        for (unsigned y = 0; y < gy * 8; ++y)
            for (unsigned x = 0; x < gx * 8; ++x)
                PixelModes(uint3(x, y));
        if (c.Mode == 9)
        {
            for (unsigned y = 0; y < 16; ++y)
                for (unsigned x = 0; x < 16; ++x)
                {
                    const auto actual = u[0]->pixels.At(x, y), reference = expected.pixels.At(x, y);
                    assert(actual.x == reference.x && actual.y == reference.y && actual.z == reference.z &&
                           actual.w == reference.w && "probe differs from independent baseline shader");
                }
            assert(s[7] == nullptr && s[8] == nullptr && "probe must not bind residual or coarse history");
            assert(g_residualReads + g_lowReads == before && "probe must not read residual or coarse history");
        }
        return true;
    }
};
// PRODUCTION_METHODS

namespace
{
void Require(bool value, const char* message)
{
    if (!value)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
float Half(float x) { return float(_Float16(x)); }
float3 Half(float3 v) { return { Half(v.x), Half(v.y), Half(v.z) }; }
void Equal(float3 a, float3 b)
{
    Require(a.x == b.x && a.y == b.y && a.z == b.z, "pixel differs from frozen 73fad086 carry");
}

void ProbeWithoutResidualReads()
{
    DlssNr_Cadence_Dx12 pass(19, 17);
    ID3D12GraphicsCommandList cmd;
    auto* proxy = pass.Make("proxy", 19, 17);
    auto* depth = pass.Make("depth", 19, 17);
    auto* motion = pass.Make("motion", 19, 17);
    for (int y = 0; y < 17; ++y)
        for (int x = 0; x < 19; ++x)
        {
            proxy->pixels.Ref(x, y) = float4(0.25f);
            pass._proxyThen->pixels.Ref(x, y) = float4(0.25f);
            depth->pixels.Ref(x, y) = float4(0.4f);
            pass._depthThen->pixels.Ref(x, y) = float4(0.4f);
        }
    DlssNr_Cadence_Dx12::Frame frame;
    frame.proxy = proxy;
    frame.depth = depth;
    frame.motion = motion;
    frame.depthRegion = { 0, 0, 19, 17 };
    frame.motionRegion = frame.depthRegion;
    pass.Probe(&cmd, frame, nullptr, true, true, 2, 32);
    Require(cmd.copies == 1, "acceptance-only probe retains its readback");
}

// Uneven extents exercise partial 16x16 coarse blocks. Each Record has a distinct history.
void Sequence(bool failedPartial, bool failedCarry = false)
{
    constexpr unsigned width = 19, height = 17;
    DlssNr_Cadence_Dx12 pass(width, height);
    ID3D12GraphicsCommandList cmd;
    auto* proxy = pass.Make("proxy", width, height, true);
    auto* depth = pass.Make("depth", width, height);
    auto* motion = pass.Make("motion", width, height);
    auto* ui = pass.Make("interface", width, height);
    auto* answer = pass.Make("answer", width, height, true);
    auto* output = pass.Make("output", width, height, true, kWrite);
    DlssNr_Cadence_Dx12::Frame frame;
    frame.proxy = proxy;
    frame.depth = depth;
    frame.motion = motion;
    frame.depthRegion = { 0, 0, width, height };
    frame.motionRegion = frame.depthRegion;
    const auto scene = [&](int generation)
    {
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x)
            {
                const float light = Half(0.22f + float(x + y + generation) * 0.003f);
                proxy->pixels.Ref(x, y) = float4(Half(float3(light, light * 0.75f, light * 0.5f)), 1);
                depth->pixels.Ref(x, y) = float4(0.4f + float(x > 9 || (generation > 3 && x == 8)) * 0.2f);
                motion->pixels.Ref(x, y) = float4(generation == 2 && x == 4 ? NAN : 0.0f, 0, 0, 0);
                ui->pixels.Ref(x, y) = float4(x < 3 ? 1.0f : 0.0f);
                answer->pixels.Ref(x, y) = float4(Half(proxy->pixels.At(x, y).rgb * (1.0f + generation * 0.04f)), 1);
            }
    };
    DlssNr::Cadence::Scheduler scheduler;
    DlssNr::Cadence::Inputs in;
    in.requested = 4;
    in.adaptiveInterval = 1;
    in.smoothing = true;
    DlssNr::Cadence::Signature signature;
    for (unsigned i = 1; i <= 3; ++i)
    {
        scene(i);
        const auto d = scheduler.Decide(in, signature, i, i * 16, false);
        Require(d.runModel && d.record, "interval one still records the latest history");
        if (i == 2 && failedPartial)
        {
            scheduler.Drop(false, true);
            continue;
        }
        Require(pass.Record(&cmd, frame, answer), "production Record succeeds");
        scheduler.Recorded();
        Require(std::count(cmd.modes.begin(), cmd.modes.end(), 1u) == 0,
                "Record must not reduce a history before its first consumer");
        pass.Probe(&cmd, frame, i == 2 ? ui : nullptr, true, true, i, i * 16);
    }
    auto params = Shipped();
    params.width = width;
    params.height = height;
    params.lowWidth = pass._lowWidth;
    params.lowHeight = pass._lowHeight;
    const auto eagerCoarse = [&]()
    {
        // Eager baseline from the newest stored/quantized residual, not the current model answer.
        g_residual = pass._residual->Rgb();
        g_proxyThen = pass._proxyThen->Rgb();
        g_depthThen = pass._depthThen->Scalar();
        g_low.Resize(params.lowWidth, params.lowHeight);
        for (int y = 0; y < params.lowHeight; ++y)
            for (int x = 0; x < params.lowWidth; ++x)
                g_low.Ref(x, y) = Half(CadenceCoarse(x, y, params));
        return g_low;
    };
    auto eagerLow = eagerCoarse();
    in.adaptiveInterval = 4;
    scene(4); // return to carry using the most recent Record, not this different model answer
    auto decision = scheduler.Decide(in, signature, 4, 64, false);
    Require(!decision.runModel && decision.chainStart, "recovery starts a new carry chain");
    DlssNr_Cadence_Dx12::Smooth smooth;
    uint64_t present = 4;
    if (failedCarry)
    {
        smooth.limit = true;
        pass.failShow = true;
        Require(!pass.Carry(&cmd, frame, decision.chainStart, nullptr, smooth, output),
                "partial Carry propagates Show failure");
        Require(pass.showCalls == 1 && cmd.modes[cmd.modes.size() - 2] == 2 && cmd.modes.back() == 3,
                "the failed Carry already recorded chain and shown-answer work");
        scheduler.Drop(true);
        Require(!scheduler.HasEdit() && scheduler.CarriedSinceModel() == 0 && scheduler.FramesSinceModel() == 1,
                "partial Carry Drop invalidates reuse without losing model-history age");
        scene(5);
        auto restart = scheduler.Decide(in, signature, 5, 80, false);
        Require(restart.runModel && restart.record && restart.staleHistory && !restart.fade,
                "after partial Carry failure the next model resets and records a fresh history");
        Require(pass.Record(&cmd, frame, answer), "fresh model after Drop records its latest history");
        scheduler.Recorded();
        Require(std::count(cmd.modes.begin(), cmd.modes.end(), 1u) == 1, "recovery Record does not eagerly reduce");
        eagerLow = eagerCoarse();
        present = 6;
        restart = scheduler.Decide(in, signature, present, present * 16, false);
        Require(!restart.runModel && restart.chainStart, "first Carry after Drop/new Record must restart its chain");
        pass.failShow = false;
        smooth.limit = false;
        decision = restart;
    }
    Require(pass.Carry(&cmd, frame, decision.chainStart, nullptr, smooth, output), "first production Carry succeeds");
    scheduler.Carried();
    const auto coarseCount = failedCarry ? 2 : 1;
    Require(std::count(cmd.modes.begin(), cmd.modes.end(), 1u) == coarseCount,
            "first Carry prepares coarse exactly once");
    Require(std::find(cmd.modes.begin(), cmd.modes.end(), 1u) < std::find(cmd.modes.begin(), cmd.modes.end(), 3u),
            "coarse must precede the first synthesis read");
    const auto observedLow = pass._residualLow->Rgb();
    for (size_t i = 0; i < eagerLow.px.size(); ++i)
        Equal(eagerLow.px[i], observedLow.px[i]);
    g_low = eagerLow;
    g_residual = pass._residual->Rgb();
    g_proxyThen = pass._proxyThen->Rgb();
    g_depthThen = pass._depthThen->Scalar();
    g_proxyNow = proxy->Rgb();
    g_depthNow = depth->Scalar();
    const auto chain = pass._chain[pass._current]->Vectors();
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
        {
            const auto legacy = LegacyCarryEdit(x, y, chain.At(x, y), params, false);
            Equal(output->pixels.At(x, y).rgb,
                  Half(CadenceClampAnswer(g_proxyNow.At(x, y), g_proxyNow.At(x, y) + legacy.add)));
        }
    decision = scheduler.Decide(in, signature, present + 1, (present + 1) * 16, false);
    Require(!decision.chainStart, "later carry reuses the same history");
    Require(pass.Carry(&cmd, frame, decision.chainStart, nullptr, smooth, output), "later production Carry succeeds");
    Require(std::count(cmd.modes.begin(), cmd.modes.end(), 1u) == coarseCount, "later Carry must not reduce again");
    scheduler.Carried();
    in.adaptiveInterval = 3;
    scene(6);
    decision = scheduler.Decide(in, signature, present + 2, (present + 2) * 16, false);
    Require(decision.runModel && decision.handChain && decision.fade && decision.record,
            "scheduled model after carries must fade old history before the new Record");
    Require(pass.ModelMotion(&cmd, frame) != nullptr, "production model motion succeeds");
    smooth.fadeWeight = 0.5f;
    Require(pass.Fade(&cmd, frame, nullptr, answer, smooth), "production Fade succeeds");
    g_residual = pass._residual->Rgb();
    g_low = eagerLow;
    g_proxyThen = pass._proxyThen->Rgb();
    g_depthThen = pass._depthThen->Scalar();
    g_proxyNow = proxy->Rgb();
    g_depthNow = depth->Scalar();
    g_motion = motion->Vectors();
    g_chain = pass._chain[pass._current]->Vectors();
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
        {
            const auto legacy = LegacyCarryEdit(x, y, CadenceChainStep(x, y, false, params), params, false);
            auto keep = legacy.accept;
            const auto now = g_proxyNow.At(x, y);
            auto fade = (CadenceClampAnswer(now, now + legacy.add) - answer->pixels.At(x, y).rgb) * keep;
            if (!CadenceFinite3(fade))
                fade = { 0, 0, 0 };
            Equal(pass._shown->pixels.At(x, y).rgb,
                  Half(CadenceFadeShown(answer->pixels.At(x, y).rgb, fade, smooth.fadeWeight)));
        }
    Require(std::count(cmd.modes.begin(), cmd.modes.end(), 1u) == coarseCount, "Fade reuses old history's coarse");
    Require(pass.Record(&cmd, frame, answer), "Record replaces history only after Fade");
    scheduler.Recorded(true);
    Require(std::count(cmd.modes.begin(), cmd.modes.end(), 1u) == coarseCount, "new Record does not eagerly reduce");
    decision = scheduler.Decide(in, signature, present + 3, (present + 3) * 16, false);
    Require(!decision.runModel && decision.chainStart, "new Record restarts first-carry relation");
    Require(pass.Carry(&cmd, frame, decision.chainStart, nullptr, smooth, output),
            "first Carry of new history succeeds");
    Require(std::count(cmd.modes.begin(), cmd.modes.end(), 1u) == coarseCount + 1,
            "new history gets a new coarse exactly once");
    // Failure returns must not prepare a coarse or write any commands.
    const auto count = cmd.modes.size();
    frame.depth = nullptr;
    Require(!pass.Carry(&cmd, frame, true, nullptr, smooth, output), "invalid Carry refuses");
    Require(cmd.modes.size() == count, "refusal records no coarse dispatch");
}
} // namespace
int main()
{
    ProbeWithoutResidualReads();
    Sequence(false);
    Sequence(true);
    Sequence(false, true);
    std::puts("PASS production cadence orchestration: Record/Probe/recovery and partial failure/Drop/recovery, odd "
              "extents, latest half-rounded history, zero unused coarse work");
}
