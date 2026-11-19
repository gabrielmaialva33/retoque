#include <cassert>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "DlssNr_PassSettings.h"
#include "DlssNr_PresentCpuTiming.h"
#define SI_NO_CONVERSION
#include <SimpleIni.h>

static int lstrcmpiA(const char* a, const char* b)
{
    if (a == b)
        return 0;
    if (!a)
        return -1;
    if (!b)
        return 1;
    while (*a && *b)
    {
        auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? int(c + ('a' - 'A')) : int(c); };
        const int diff = lower(static_cast<unsigned char>(*a)) - lower(static_cast<unsigned char>(*b));
        if (diff != 0)
            return diff;
        ++a;
        ++b;
    }
    return static_cast<int>(static_cast<unsigned char>(*a)) - static_cast<int>(static_cast<unsigned char>(*b));
}

static CSimpleIniA ini;

// @@CUSTOM_OPTIONAL@@

// @@CONFIG_HELPERS@@

class Config
{
  public:
    CustomOptional<float, NoDefault> DlssNrWorkingScale;
    CustomOptional<float> DlssNrIntensity { 1.0f };
    std::vector<std::string> _log;

    std::optional<std::string> readString(std::string section, std::string key, bool lowercase = false);
    std::optional<float> readFloat(std::string section, std::string key);
};

// @@CONFIG_METHODS@@

namespace AnchorHarness
{
struct AnchorPoint
{
    float scan;
    float white;
};

constexpr float kFloor = 1e-6f;
constexpr float kCeiling = 1e4f;
std::mutex g_scanMutex;
std::vector<AnchorPoint> g_anchors;

void SortAnchorsLocked()
{
    std::sort(g_anchors.begin(), g_anchors.end(),
              [](const AnchorPoint& a, const AnchorPoint& b) { return a.scan < b.scan; });
}

// @@ANCHOR_METHODS@@
} // namespace AnchorHarness

namespace RatioHarness
{
namespace DlssNr::ModelFile
{
inline std::optional<std::filesystem::path> Find(const std::filesystem::path&)
{
    return std::filesystem::path("nvngx_dlssnr.dll");
}
} // namespace DlssNr::ModelFile

struct FakeNr
{
    int (*queryRatio)(const wchar_t*, void*, unsigned int, float*) = nullptr;
    void* capabilityParams = nullptr;
    int* lastRatioStage = nullptr;
};

static FakeNr g_nr;
static std::filesystem::path g_dllDir;
static std::string g_loggedLine;

#define LOG_INFO(fmt, ...)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        g_loggedLine = std::format(fmt, ##__VA_ARGS__);                                                                \
    } while (false)

// @@REPORT_SCALING_RATIOS@@

#undef LOG_INFO
} // namespace RatioHarness

static unsigned g_cases = 0;
#define CASE(name)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        ++g_cases;                                                                                                     \
        std::printf("PASS: %s\n", name);                                                                               \
    } while (false)

static void RunAllAssertions(const char* tag)
{
    assert(GetFloatValue(std::nullopt) == "auto");
    assert(GetFloatValue(std::numeric_limits<float>::quiet_NaN()) == "auto");
    assert(GetFloatValue(std::numeric_limits<float>::infinity()) == "auto");
    assert(GetFloatValue(-std::numeric_limits<float>::infinity()) == "auto");
    assert(GetFloatValue(0.0f) == "0.000000");
    assert(GetFloatValue(0.25f) == "0.250000");
    assert(GetFloatValue(0.5f) == "0.500000");
    assert(GetFloatValue(1.0f) == "1.000000");
    assert(GetFloatValue(-1.0f) == "-1.000000");
    assert(GetFloatValue(1.75f) == "1.750000");
    for (float sample : { 0.0f, -0.5f, 0.33333334f, 1234.567871f, std::numeric_limits<float>::max() })
    {
        const std::string text = GetFloatValue(sample);
        assert(text != "auto" && text.find(',') == std::string::npos && text.find('.') != std::string::npos);
    }

    // Full INI round-trip for [DlssNr] WorkingScale and Intensity, saving and reloading in the active locale.
    {
        ini.Reset();
        Config writer;
        writer.DlssNrWorkingScale = 0.5f;
        writer.DlssNrIntensity = 1.25f;
        ini.SetValue("DlssNr", "WorkingScale", GetFloatValue(writer.DlssNrWorkingScale.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Intensity", GetFloatValue(writer.DlssNrIntensity.value_for_config()).c_str());
        DlssNr::PassConfig::Overrides overrides;
        DlssNrPassSettings pass1;
        pass1.LocalTone = 0.75f;
        DlssNr::PassConfig::Set(overrides, 0, pass1);
        DlssNr::PassConfig::Save(ini, overrides);

        std::string savedText;
        assert(ini.Save(savedText) >= 0);
        assert(savedText.find("0.500000") != std::string::npos);
        assert(savedText.find("1.250000") != std::string::npos);
        assert(savedText.find("0,500000") == std::string::npos);
        assert(savedText.find("1,250000") == std::string::npos);

        ini.Reset();
        assert(ini.LoadData(savedText.c_str()) >= 0);
        Config reader;
        reader.DlssNrWorkingScale.set_from_config(reader.readFloat("DlssNr", "WorkingScale"));
        reader.DlssNrIntensity.set_from_config(
            DlssNr::PassConfig::Float(ini.GetValue("DlssNr", "Intensity"), 0.0f, 2.0f));
        const auto reloadedOverrides = DlssNr::PassConfig::Load(ini);
        assert(reader.DlssNrWorkingScale.has_value() && *reader.DlssNrWorkingScale == 0.5f);
        assert(reader.DlssNrIntensity.value_or_default() == 1.25f);
        assert(DlssNr::PassConfig::Get(reloadedOverrides, 0).LocalTone == 0.75f);
    }

    // Recovery of an INI already poisoned by an older build under a comma locale, then canonicalized on save.
    {
        ini.Reset();
        assert(ini.LoadData("[DlssNr]\n"
                            "WorkingScale = 0,500000\n"
                            "Intensity = 1,250000\n"
                            "SkinStructure = -0,500000\n"
                            "[DlssNr.Pass1]\n"
                            "Intensity = 0,600000\n"
                            "LocalStructure = +1,500000\n") >= 0);
        Config recovered;
        recovered.DlssNrWorkingScale.set_from_config(recovered.readFloat("DlssNr", "WorkingScale"));
        assert(recovered.DlssNrWorkingScale == 0.5f);
        CustomOptional<float> intensity { 1.0f };
        CustomOptional<float> skin { -1.0f };
        intensity.set_from_config(DlssNr::PassConfig::Float(ini.GetValue("DlssNr", "Intensity"), 0.0f, 2.0f));
        skin.set_from_config(DlssNr::PassConfig::Float(ini.GetValue("DlssNr", "SkinStructure"), -1.0f, 2.0f));
        assert(intensity.value_or_default() == 1.25f);
        assert(skin.value_or_default() == -0.5f);
        const auto overrides = DlssNr::PassConfig::Load(ini);
        assert(DlssNr::PassConfig::Get(overrides, 0).Intensity == 0.6f);
        assert(DlssNr::PassConfig::Get(overrides, 0).LocalStructure == 1.5f);

        ini.SetValue("DlssNr", "WorkingScale", GetFloatValue(recovered.DlssNrWorkingScale.value_for_config()).c_str());
        ini.SetValue("DlssNr", "Intensity", GetFloatValue(intensity.value_for_config()).c_str());
        ini.SetValue("DlssNr", "SkinStructure", GetFloatValue(skin.value_for_config()).c_str());
        DlssNr::PassConfig::Save(ini, overrides);
        std::string canonical;
        assert(ini.Save(canonical) >= 0);
        assert(canonical.find("WorkingScale = 0.500000") != std::string::npos);
        assert(canonical.find("Intensity = 1.250000") != std::string::npos);
        assert(canonical.find("SkinStructure = -0.500000") != std::string::npos);
        assert(canonical.find("Intensity = 0.6") != std::string::npos);
        assert(canonical.find("LocalStructure = 1.5") != std::string::npos);
        assert(canonical.find(',') == std::string::npos);
    }

    // Rejection of ambiguous, malformed, overflowing, underflowing and non-finite strings.
    for (const char* bad :
         { "",      "   ",  "auto", "AUTO", ",",   ".",   "1,000,000", "1.000,5", "1,000.5", "+-0.5", "+-0,5", "++0.5",
           "+ 0.5", "0.5x", "0,5x", "0.5 ", "nan", "NaN", "inf",       "-inf",    "+inf",    "1e99",  "1e-999" })
    {
        float out = -42.0f;
        if (std::string_view(bad) != "auto" && std::string_view(bad) != "AUTO")
            assert(!isFloat(bad, out) && out == -42.0f);
        ini.Reset();
        ini.SetValue("DlssNr", "WorkingScale", bad);
        Config c;
        assert(!c.readFloat("DlssNr", "WorkingScale").has_value());
        assert(!DlssNr::PassConfig::Float(bad, -1.0f, 2.0f).has_value());
    }

    // Overlay settings pack/unpack keeps exact float values for both dot and comma INI strings.
    {
        for (const char* text : { "1.500000", "1,500000" })
        {
            std::string packed;
            packed += static_cast<char>(11); // FpsScale in overlayIniKeys
            PackOverlayValue(packed, OverlayValueType::Float, text);
            const auto unpacked = UnpackOverlayValues(packed);
            assert(unpacked.has_value() && *unpacked == "FpsScale=1.5;");
        }
    }

    // ExposureScan anchor table round-trips dot decimals and recovers comma-serialized tables.
    {
        AnchorHarness::LoadAnchors("0,125:2,5;0,5:1,25;1,0,2:3.0;bad:1.0;");
        assert(AnchorHarness::g_anchors.size() == 2);
        assert(std::fabs(AnchorHarness::g_anchors[0].scan - 0.125f) < 1e-6f);
        assert(std::fabs(AnchorHarness::g_anchors[0].white - 2.5f) < 1e-6f);
        assert(std::fabs(AnchorHarness::g_anchors[1].scan - 0.5f) < 1e-6f);
        assert(std::fabs(AnchorHarness::g_anchors[1].white - 1.25f) < 1e-6f);
        const std::string serialized = AnchorHarness::SerializeAnchors();
        assert(serialized == "0.125:2.5;0.5:1.25;");
        AnchorHarness::LoadAnchors(serialized);
        assert(AnchorHarness::SerializeAnchors() == serialized);
    }

    // Present CPU timing window log uses dot decimals.
    {
        DlssNr::PresentCpu::Window w;
        w.presents = 3;
        w.seconds = 5.0;
        w.record.Add(0.4);
        w.record.Add(2.4);
        w.submit.Add(0.25);
        w.waits[static_cast<size_t>(DlssNr::PresentCpu::Wait::Allocator)].waited.Add(0.5);
        const std::string line = DlssNr::PresentCpu::Format(w, 2);
        assert(line.find("seconds=5.0 record_ms mean=1.400 max=2.400 submit_ms mean=0.250 max=0.250") !=
               std::string::npos);
        assert(line.find("allocator_wait attempted=1 mean_ms=0.500 max_ms=0.500 failures=0") != std::string::npos);
        assert(line.find(',') == std::string::npos);
    }

    // ReportScalingRatios logs four-decimal dot ratios and refused entries without commas.
    {
        int fakeParams = 1;
        RatioHarness::g_nr.capabilityParams = &fakeParams;
        RatioHarness::g_nr.queryRatio = [](const wchar_t*, void*, unsigned int q, float* out) -> int
        {
            static constexpr float kRatios[6] = { 0.5f, 0.58f, 0.6667f, 0.3333f, -1.0f, 1.0f };
            if (q == 4)
                return -1;
            *out = kRatios[q];
            return 1;
        };
        RatioHarness::g_loggedLine.clear();
        RatioHarness::ReportScalingRatios();
        assert(RatioHarness::g_loggedLine ==
               "DLSS-NR the model's own scaling ratios: MaxPerf=0.5000 Balanced=0.5800 MaxQuality=0.6667 "
               "UltraPerformance=0.3333 UltraQuality=refused DLAA=1.0000 ");
        assert(RatioHarness::g_loggedLine.find(',') == std::string::npos);
    }

    std::printf("PASS: numeric serialization, comma recovery and log lines verified (%s)\n", tag);
    ++g_cases;
}

int main()
{
    RunAllAssertions("classic C locale");

    const char* priorPtr = std::setlocale(LC_NUMERIC, nullptr);
    const std::string prior = priorPtr != nullptr ? priorPtr : "C";
    const char* applied = nullptr;
    for (const char* candidate :
         { "pt_BR.UTF-8", "pt_BR.utf8", "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8" })
    {
        if (std::setlocale(LC_NUMERIC, candidate) != nullptr && std::localeconv()->decimal_point != nullptr &&
            std::localeconv()->decimal_point[0] == ',')
        {
            applied = candidate;
            break;
        }
        std::setlocale(LC_NUMERIC, prior.c_str());
    }

    if (applied != nullptr)
    {
        char label[96];
        std::snprintf(label, sizeof(label), "live comma LC_NUMERIC=%s", applied);
        RunAllAssertions(label);
        std::setlocale(LC_NUMERIC, prior.c_str());
        CASE("live decimal-comma LC_NUMERIC leaves float serialization, parsing and log lines unchanged");
    }
    else
    {
        std::printf("NOTE: no decimal-comma C locale (pt_BR/de_DE/fr_FR UTF-8) installed on this host; "
                    "live setlocale pass skipped while parser/serializer cases ran\n");
    }

    assert(g_cases >= 1);
    return 0;
}
