// Scenarios for the production ReleaseSurfacesIfFormatChanged (docs/dlssnr/design/format-swap-history.md).
// Included by run.py after the real functions, extracted from DlssNr_Dx12_Lifetime.cpp, and the fakes they need.
// Nothing here runs a GPU, NGX or a shader: the resources are fakes that only answer GetDesc.

namespace DlssNr::Dx12Internal
{
static int g_failures = 0;
static std::vector<ID3D12Resource*> g_owned;

static void Check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

static ID3D12Resource* Make(DXGI_FORMAT format, unsigned w, unsigned h)
{
    auto* r = new ID3D12Resource { { format, w, h } };
    g_owned.push_back(r);
    return r;
}

struct Extent
{
    unsigned w = 1920, h = 1080, ww = 1920, wh = 1080;
};

using Transfer = DlssNr::FormatTwin::Transfer;

// What Dispatch does around the call: the format check first, then the surfaces it finds missing.
static void Frame(DXGI_FORMAT format, Extent e = {}, Transfer t = {})
{
    RELEASE_SURFACES(format, e.w, e.h, e.ww, e.wh, t);
    if (g_nr.output == nullptr)
    {
        g_nr.output = Make(format, e.ww, e.wh);
        g_nr.colorCopy = Make(format, e.w, e.h);
        g_nr.hdrCopy = Make(format, e.w, e.h);
    }
}

// A session that has run on `format` for a while: surfaces built, the model's history settled (no reset pending),
// a usable calibration.
static void Start(DXGI_FORMAT format, Extent e = {}, Transfer t = {})
{
    g_nr = {};
    g_parked = 0;
    Frame(format, e, t);
    g_nr.reset = false;
    for (bool& r : g_nr.passReset)
        r = false;
    g_nr.calibCount = 42;
    g_nr.calibSuggestion = 1.5f;
    g_nr.calibUsable = true;
}

static bool AnyReset() { return g_nr.reset || g_nr.passReset[0] || g_nr.passReset[1] || g_nr.passReset[2]; }
static bool AllReset() { return g_nr.reset && g_nr.passReset[0] && g_nr.passReset[1] && g_nr.passReset[2]; }
static bool CalibrationKept() { return g_nr.calibCount == 42 && g_nr.calibUsable && g_nr.calibSuggestion == 1.5f; }
static bool CalibrationForgotten() { return g_nr.calibCount == 0 && !g_nr.calibUsable; }

static void KeepsAcross(DXGI_FORMAT from, DXGI_FORMAT to, const char* what)
{
    Start(from);
    ID3D12Resource* const first = g_nr.output;
    Frame(to);
    std::string label = std::string(what) + ": no history reset";
    Check(!AnyReset(), label.c_str());
    label = std::string(what) + ": calibration kept";
    Check(CalibrationKept(), label.c_str());
    label = std::string(what) + ": surfaces in the new format, the old set kept aside";
    Check(g_nr.output != nullptr && g_nr.output->GetDesc().Format == to && g_nr.alt.output == first &&
              g_nr.alt.format == from,
          label.c_str());

    Frame(from);
    label = std::string(what) + ": and back: the kept set is swapped back in, still no reset";
    Check(g_nr.output == first && !AnyReset() && CalibrationKept(), label.c_str());
}

static void ResetsAcross(DXGI_FORMAT from, DXGI_FORMAT to, const char* what, Extent e = {}, Extent e2 = {},
                         Transfer t = {}, Transfer t2 = {})
{
    Start(from, e, t);
    Frame(to, e2, t2);
    std::string label = std::string(what) + ": history reset, every pass";
    Check(AllReset(), label.c_str());
    label = std::string(what) + ": calibration forgotten";
    Check(CalibrationForgotten(), label.c_str());
}

static int Run()
{
    // The equivalent swaps: the same bytes with red and blue swapped, read through views of each resource's own format.
    KeepsAcross(kRgba8Unorm, kBgra8Unorm, "R8G8B8A8_UNORM -> B8G8R8A8_UNORM");
    KeepsAcross(kBgra8Unorm, kRgba8Unorm, "B8G8R8A8_UNORM -> R8G8B8A8_UNORM");
    KeepsAcross(kRgba8Typeless, kBgra8Typeless, "R8G8B8A8_TYPELESS -> B8G8R8A8_TYPELESS (Watch Dogs: Legion)");
    KeepsAcross(kRgba8Srgb, kBgra8Srgb, "R8G8B8A8_UNORM_SRGB -> B8G8R8A8_UNORM_SRGB");

    // Alternating every frame for a while: never a reset, the calibration never restarts, nothing new is built.
    {
        Start(kRgba8Typeless);
        Frame(kBgra8Typeless);
        const size_t built = g_owned.size();
        bool clean = true;
        for (int i = 0; i < 50; ++i)
        {
            Frame(i % 2 ? kBgra8Typeless : kRgba8Typeless);
            clean = clean && !AnyReset() && CalibrationKept();
        }
        Check(clean && g_owned.size() == built && g_parked == 0,
              "50 alternations 27/90: no reset, calibration kept, nothing built or parked after the first");
    }

    // A reset the game asked for this frame (a cut, or the scene-cut estimator) is set before the format check, and
    // the format check never clears it.
    {
        Start(kRgba8Unorm);
        g_nr.reset = true;
        Frame(kBgra8Unorm);
        Check(g_nr.reset, "twin swap with the game's Reset on the same frame: the reset stands");
    }

    // A twin swap is only equivalent at the extent the surfaces were built for.
    {
        Extent big;
        Extent small { 1280, 720, 1280, 720 };
        ResetsAcross(kRgba8Unorm, kBgra8Unorm, "twin swap with a frame size change", big, small);
        Extent work { 1920, 1080, 960, 540 };
        ResetsAcross(kRgba8Unorm, kBgra8Unorm, "twin swap with a working size change", big, work);
    }

    // A twin swap with a different transfer is a different picture.
    {
        Transfer sdr;
        Transfer hdr;
        hdr.gameSaysHdr = true;
        ResetsAcross(kRgba8Unorm, kBgra8Unorm, "twin swap with the game's HDR claim changing", {}, {}, sdr, hdr);
        Transfer white;
        white.presentWhite = 2.5f;
        ResetsAcross(kRgba8Unorm, kBgra8Unorm, "twin swap with the present host's HDR white changing", {}, {}, sdr,
                     white);
        Transfer quant;
        quant.presentQuantRel = 1.0f / 1024.0f;
        ResetsAcross(kRgba8Unorm, kBgra8Unorm, "twin swap with the output code changing", {}, {}, sdr, quant);
        Transfer nan;
        nan.presentWhite = std::numeric_limits<float>::quiet_NaN();
        ResetsAcross(kRgba8Unorm, kBgra8Unorm, "twin swap with a NaN white on both sides", {}, {}, nan, nan);
    }

    // Every other format change resets exactly as before.
    ResetsAcross(kRgba8Unorm, kRgba8Srgb, "UNORM -> sRGB (R8G8B8A8): the view decodes on one side only");
    ResetsAcross(kBgra8Unorm, kBgra8Srgb, "UNORM -> sRGB (B8G8R8A8)");
    ResetsAcross(kRgba8Unorm, kBgra8Srgb, "R8G8B8A8_UNORM -> B8G8R8A8_UNORM_SRGB");
    ResetsAcross(kRgba8Srgb, kBgra8Unorm, "R8G8B8A8_UNORM_SRGB -> B8G8R8A8_UNORM");
    ResetsAcross(kRgba8Typeless, kRgba8Unorm, "typeless -> typed, same order");
    ResetsAcross(kRgba8Typeless, kBgra8Unorm, "typeless -> typed, swapped order");
    ResetsAcross(kRgba8Unorm, kBgrx8Unorm, "R8G8B8A8 -> B8G8R8X8: alpha reads as 1");
    ResetsAcross(kBgra8Unorm, kBgrx8Unorm, "B8G8R8A8 -> B8G8R8X8");
    ResetsAcross(kRgba8Unorm, kRgba16Float, "R8G8B8A8 -> RGBA16F (SDR to HDR)");
    ResetsAcross(kRgba16Float, kBgra8Unorm, "RGBA16F -> B8G8R8A8 (HDR to SDR)");
    ResetsAcross(kR11G11B10Float, kRgba16Float, "R11G11B10 -> RGBA16F (FF7 Rebirth)");
    ResetsAcross(kRgb10A2Unorm, kRgba8Unorm, "RGB10A2 -> R8G8B8A8");

    // FF7 Rebirth's alternation still swaps the kept set back in, and still resets each time.
    {
        Start(kR11G11B10Float);
        ID3D12Resource* const first = g_nr.output;
        Frame(kRgba16Float);
        g_nr.reset = false;
        g_nr.calibCount = 42;
        g_nr.calibUsable = true;
        Frame(kR11G11B10Float);
        Check(g_nr.output == first && g_nr.reset && CalibrationForgotten(),
              "R11G11B10 <-> RGBA16F: the kept set comes back, with a reset and a fresh calibration");
    }

    // No format change: the function leaves everything as it was, a pending reset included.
    {
        Start(kRgba8Unorm);
        Frame(kRgba8Unorm);
        Check(!AnyReset() && CalibrationKept(), "same format: nothing reset");
        g_nr.reset = true;
        Frame(kRgba8Unorm);
        Check(g_nr.reset, "same format: a pending reset is left pending");
    }

    for (ID3D12Resource* r : g_owned)
        delete r;
    g_owned.clear();

    if (g_failures != 0)
        std::printf("%d scenario check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
} // namespace DlssNr::Dx12Internal

int main() { return DlssNr::Dx12Internal::Run(); }
