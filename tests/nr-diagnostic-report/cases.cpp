// tests/nr-diagnostic-report: the NR panel's "Copy NR diagnostics" text (retoque/dlssnr/DlssNr_DiagnosticReport.h,
// docs/dlssnr/design/diagnostic-export.md). The header is standard library only and compiled as it is.

#include <dlssnr/DlssNr_DiagnosticReport.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace
{
int g_failures = 0;

void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

bool Contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

size_t Count(std::string_view text, std::string_view part)
{
    size_t n = 0;
    for (size_t at = text.find(part); at != std::string_view::npos; at = text.find(part, at + part.size()))
        ++n;
    return n;
}

namespace DR = DlssNr::DiagnosticReport;

// A plausible D3D12 session under Proton, with every field filled.
DR::Inputs FullInputs()
{
    DR::Inputs in;
    in.build = "OptiScaler v10.0.0-dev (abc1234) (2026-10-30)";
    in.wine = true;
    in.nrEnabled = true;
    in.api = "D3D12";
    in.upscaler = "DLSS 310.4.0";
    in.modelDescribed = true;
    in.modelVersion = "310.8.0.0";
    in.modelBytes = 165000000;
    in.modelVramKnown = true;
    in.modelVramBytes = 851443712; // 812 MiB
    in.loaderVersion = "32.0.16.1691";
    in.loaderBuild = "616.91";
    in.feature18Route = "routes feature 18 itself";
    in.exposureFrames = 1200;
    in.exposureOfferedNow = true;
    in.exposureEverOffered = true;
    in.hdrSeen = true;
    in.hdrProfile = "HDR10";
    in.applyModel = true;
    in.hookMethod = 2;
    in.pass = DR::Pass::RunningD3D12;
    in.hookStatus = "present: active on the swapchain";
    in.guidesLine = "Guides: the game's depth and motion, on every frame";
    in.hdrConverted = true;
    in.settingsContract = "stage=after render=1920x1080 compose=3840x2160 model=1920x1080 scale=0.5 passes=1 "
                          "individual=false apply=true hold=false white_source=0 detail=1 linear_hdr=false "
                          "compose_format=28 effective_transfer=1 enlarge=matched pass1=[preset=0,style=1] "
                          "cadence=2 highlight_recovery=1";
    in.gpu.instrumented = true;
    in.gpu.measuring = true;
    in.gpu.valid = true;
    in.gpu.totalMs = 3.25;
    in.gpu.modelMs = 2.5;
    in.gpu.otherMs = 0.75;
    in.gpu.sampleId = 42;
    in.gpu.ageMs = 400;
    in.gpu.settingsGeneration = 3;
    in.gpu.contractHash = 0x1234abcdULL;
    in.gpu.stage = "after";
    in.gpu.modelWidth = 1920;
    in.gpu.modelHeight = 1080;
    in.gpu.passes = 1;
    in.gpu.cadence = 1;
    in.lsfg.enabled = true;
    in.lsfg.active = true;
    in.lsfg.width = 3840;
    in.lsfg.height = 2160;
    in.lsfg.gpuMs = 1.5;
    return in;
}

const std::vector<std::string_view> kSections = { "== Build ==",    "== Detected ==",   "== Used ==",
                                                  "== Settings ==", "== GPU timing ==", "== LSFG ==" };

void Deterministic()
{
    const auto a = DR::Build(FullInputs());
    const auto b = DR::Build(FullInputs());
    Require(!a.empty(), "the report is not empty");
    Require(a == b, "the same inputs give the same text, byte for byte");
    Require(!Contains(a, "<path>"), "a report with no path in its inputs has nothing redacted: " + a);
    Require(a.back() == '\n', "the report ends with a newline");
    Require(!Contains(a, "\r"), "the report uses \\n only");

    auto other = FullInputs();
    other.gpu.totalMs = 3.5;
    Require(DR::Build(other) != a, "a different input changes the text");
}

void NoSectionMissing()
{
    // Every section once, in order, for a full report and for one whose fields were all left at their defaults.
    DR::Inputs bare;
    bare.nrEnabled = true;
    for (const auto& report : { DR::Build(FullInputs()), DR::Build(bare) })
    {
        size_t last = 0;
        for (const auto section : kSections)
        {
            Require(Count(report, section) == 1, std::string("section once: ") + std::string(section));
            const auto at = report.find(section);
            Require(at != std::string::npos && at >= last, std::string("section in order: ") + std::string(section));
            last = at == std::string::npos ? last : at;
        }
        for (const auto label : { "Build:", "Platform:", "Neural Rendering: on", "Graphics API:", "Upscaler feature:",
                                  "D3D11 route:", "Model file:", "NGX loader:", "Game exposure:", "HDR swapchain:",
                                  "Apply the model:", "Pass:", "Hook method (setting):", "Hook status:", "Guides:",
                                  "HDR conversion:", "Measurement:", "Last confirmed sample:", "LSFG:" })
            Require(Contains(report, std::string("\n") + label), std::string("line present: ") + label);
        Require(Contains(report, "\nModel VRAM:"), "line present: Model VRAM:");
    }

    // Unknown values say so instead of dropping the line.
    const auto bareText = DR::Build(bare);
    Require(Contains(bareText, "Graphics API: unknown"), "an unset API reads unknown");
    Require(Contains(bareText, "Model file: not described this session"), "an undescribed model says so");
    Require(Contains(bareText, "Model VRAM: not reported (no running D3D12 build the model gave a figure for)"),
            "no figure says so");
    Require(Contains(bareText, "Last confirmed sample: none"), "no sample reads none");
}

void DetectedBeforeUsed()
{
    const auto text = DR::Build(FullInputs());
    const auto detected = text.find("== Detected ==");
    const auto used = text.find("== Used ==");
    Require(detected < used, "Detected comes before Used");
    // What the game offers is detected; what the pass ran on is used.
    for (const auto line : { "Graphics API: D3D12", "Upscaler feature: DLSS 310.4.0", "HDR swapchain: seen (HDR10)",
                             "Model file: version 310.8.0.0, 165000000 bytes",
                             "Model VRAM: 812.0 MiB (851443712 bytes), the model's own count at its last build" })
    {
        const auto at = text.find(line);
        Require(at != std::string::npos && detected < at && at < used, std::string("in Detected: ") + line);
    }
    for (const auto line : { "Pass: running on D3D12", "Hook method (setting): Present",
                             "Guides: the game's depth and motion, on every frame", "HDR conversion: converting" })
    {
        const auto at = text.find(line);
        Require(at != std::string::npos && used < at && at < text.find("== Settings =="),
                std::string("in Used: ") + line);
    }
    Require(Contains(text, "Platform: Wine/Proton"), "Wine named");
    auto native = FullInputs();
    native.wine = false;
    Require(Contains(DR::Build(native), "Platform: native Windows"), "native Windows named");
    Require(Contains(text, "NGX loader: version 32.0.16.1691 (Windows-branch loader build 616.91, not the Linux driver "
                           "version); it routes feature 18 itself"),
            "under Wine the loader build is not called the driver");
    Require(Contains(DR::Build(native), "NGX loader: version 32.0.16.1691 (driver 616.91)"),
            "on Windows the loader build is the driver");
}

void PassStates()
{
    auto in = FullInputs();
    in.pass = DR::Pass::NotRunning;
    in.failureReason = "the model refused feature 18";
    Require(Contains(DR::Build(in), "Pass: off for this session: the model refused feature 18"), "failure reason");
    in.failureReason.clear();
    Require(Contains(DR::Build(in), "Pass: waiting for the upscaler to run"), "waiting");
    in.pass = DR::Pass::RunningVulkan;
    in.vulkanFrames = 77;
    Require(Contains(DR::Build(in), "Pass: running natively on Vulkan (77 frames)"), "native Vulkan");
    in.applyModel = false;
    Require(Contains(DR::Build(in), "Apply the model: no (the pass runs, its edit is hidden)"), "apply off");
    for (const auto& [method, name] : std::vector<std::pair<unsigned, const char*>> {
             { 0, "Auto" }, { 1, "Upscaled" }, { 2, "Present" }, { 7, "7 (out of range; read as Upscaled)" } })
    {
        in.hookMethod = method;
        Require(Contains(DR::Build(in), std::string("Hook method (setting): ") + name), name);
    }
    in.hdrConverted = false;
    in.hdrReason = "the HDR output switch is off";
    Require(Contains(DR::Build(in), "HDR conversion: not converting: the HDR output switch is off"), "hdr reason");
    in.hdrSeen = false;
    in.hdrReason.clear();
    Require(Contains(DR::Build(in), "HDR swapchain: none seen in the last two seconds"), "no hdr swapchain");
    Require(Contains(DR::Build(in), "HDR conversion: none"), "nothing to convert");
    in.dx11Route = "the pass cannot run after native D3D11 DLSS; dlss_12 carries it";
    Require(Contains(DR::Build(in), "D3D11 route: the pass cannot run after native D3D11 DLSS"), "route line");
}

void SettingsAreSettings()
{
    // The contract's frame fields are a frame's, not the configuration's: the export strips them.
    const auto stripped = DR::StripFrameFields("stage=after render=1920x1080 compose=3840x2160 model=1x1 scale=0.5 "
                                               "passes=2 individual=true apply=true linear_hdr=true compose_format=10 "
                                               "max_ratio=4 effective_transfer=2 enlarge=private min_ratio=0.25 "
                                               "pass1=[preset=0,style=1] cadence=3 periphery=[centre=0.5,work=0.5] "
                                               "highlight_recovery=1");
    Require(stripped == "passes=2 individual=true apply=true max_ratio=4 min_ratio=0.25 pass1=[preset=0,style=1] "
                        "periphery=[centre=0.5,work=0.5]",
            "frame fields stripped, settings kept in order: [" + stripped + "]");
    Require(DR::StripFrameFields("") == "", "empty contract");
    Require(DR::StripFrameFields("  apply=true   detail=1 ") == "apply=true detail=1", "spacing normalised");

    const auto text = DR::Build(FullInputs());
    const auto from = text.find("== Settings ==");
    const auto to = text.find("== GPU timing ==");
    Require(from != std::string::npos && to != std::string::npos && from < to, "Settings precedes GPU timing");
    const auto settings = from < to && to != std::string::npos ? text.substr(from, to - from) : std::string();
    Require(Contains(settings, "\n  apply=true\n"), "one setting per line");
    Require(Contains(settings, "\n  pass1=[preset=0,style=1]\n"), "per-pass settings kept");
    for (const auto frameField : { "render=", "compose=", "stage=", "linear_hdr=", "effective_transfer=", "enlarge=",
                                   "cadence=", "highlight_recovery=" })
        Require(!Contains(settings, std::string("  ") + frameField), std::string("not in Settings: ") + frameField);

    DR::Inputs none;
    none.nrEnabled = true;
    Require(Contains(DR::Build(none), "== Settings ==\nunavailable\n"), "no contract text says unavailable");
}

void GpuSampleMarking()
{
    auto in = FullInputs();
    const auto recent = DR::Build(in);
    Require(Contains(recent, "Last confirmed sample: recent, 0.4 s old: 3.250 ms dispatch = 2.500 ms model + 0.750 ms "
                             "outside model"),
            "a fresh sample while measuring is recent: " + recent);
    Require(Contains(recent, "One sample, not the current frame's cost or an average."), "the caveat is always there");
    Require(Contains(recent, "Sample 42, stage after, model 1920x1080, 1 passes, cadence 1"), "sample identity");
    Require(Contains(recent, "Contract generation 3, hash 0x000000001234abcd"), "contract identity for the log");

    in.gpu.ageMs = DR::kRecentSampleMs + 1;
    Require(Contains(DR::Build(in), "Last confirmed sample: historical, 2.0 s old"), "an old sample is historical");
    in.gpu.ageMs = 100;
    in.gpu.measuring = false;
    const auto off = DR::Build(in);
    Require(Contains(off, "Measurement: off") && Contains(off, "Last confirmed sample: historical, 0.1 s old"),
            "measurement off makes any sample historical");
    in.gpu.carried = true;
    in.gpu.cadence = 2;
    Require(Contains(DR::Build(in), ", carried frame (no model call)"), "carried frame named");
    in.gpu.valid = false;
    Require(Contains(DR::Build(in), "Last confirmed sample: none"), "no sample");
    in.gpu.instrumented = false;
    const auto vk = DR::Build(in);
    Require(Contains(vk, "Measurement: not instrumented on this route (native Vulkan or the driver proxy)"),
            "uninstrumented route");
}

void LsfgStates()
{
    auto in = FullInputs();
    Require(Contains(DR::Build(in), "LSFG: active at 3840x2160 (1.50 ms/frame)"), "lsfg active");
    in.lsfg.gpuMs = 0.0;
    Require(Contains(DR::Build(in), "LSFG: active at 3840x2160\n"), "lsfg active, untimed");
    in.lsfg.active = false;
    in.lsfg.fallbackReason = "Lossless.dll not found";
    Require(Contains(DR::Build(in), "LSFG: enabled, not active: Lossless.dll not found"), "lsfg fallback");
    in.lsfg.fallbackReason.clear();
    Require(Contains(DR::Build(in), "LSFG: enabled, waiting for frames"), "lsfg waiting");
    in.lsfg.enabled = false;
    Require(Contains(DR::Build(in), "LSFG: off"), "lsfg off");
}

void NrOffIsShort()
{
    DR::Inputs off = FullInputs();
    off.nrEnabled = false;
    const auto text = DR::Build(off);
    Require(Contains(text, "== Build ==") && Contains(text, "Build: OptiScaler v10.0.0-dev"),
            "NR off still names the build");
    Require(Contains(text, "Platform: Wine/Proton"), "NR off still names the platform");
    Require(Contains(text, "Neural Rendering: off"), "NR off says so");
    for (const auto section : kSections)
        if (section != "== Build ==")
            Require(!Contains(text, section), std::string("NR off has no ") + std::string(section));
    Require(Count(text, "\n") <= 8, "NR off is short: " + text);
    // Whatever else was filled in is not reported with NR off.
    Require(!Contains(text, "DLSS 310.4.0") && !Contains(text, "apply=true"), "NR off reports nothing of the pass");
}

void Redaction()
{
    const std::vector<std::pair<std::string, std::string>> cases = {
        { "C:\\Users\\Jane Doe\\Games\\x\\nvngx_dlssnr.dll", "<path>" },
        { "c:\\games\\x.dll", "<path>" },
        { "Z:\\home\\jane\\.steam\\steam\\steamapps\\common\\Game\\OptiScaler.dll was not found",
          "<path> was not found" },
        { "/home/jane/.local/share/Steam/steamapps/common/Game/x.dll", "<path>" },
        { "C:/Program Files (x86)/Steam/steamapps/common/Game/bin/x.dll", "<path>" },
        { "C:\\Program Files (x86)\\Steam\\x.dll was not found", "<path> was not found" },
        { "\\\\?\\C:\\Users\\me\\x.dll", "<path>" },
        { "\\\\server\\share\\dir\\file.txt", "<path>" },
        { "failed to open 'D:\\Games\\x.ini' (code 5)", "failed to open '<path>' (code 5)" },
        { "failed to open \"D:\\Games\\x.ini\"", "failed to open \"<path>\"" },
        { "(see C:\\x\\log.txt)", "(see <path>)" },
        { "model=C:\\a\\b.dll route=present", "model=<path> route=present" },
        { "C:\\a\\b.dll and Z:\\c\\d.dll", "<path> and <path>" },
        { "C:\\a\\b.dll: access denied", "<path>: access denied" },
        { "Error:/home/u/x", "Error:<path>" },
        { "one /home/u/a\ntwo /mnt/games/b", "one <path>\ntwo <path>" },
        { "C:\\a\\b.dll, then /tmp/c", "<path>, then <path>" },
        // A last name without an extension may hold spaces (a user folder): none of it survives.
        { "C:\\Users\\Jane Doe", "<path>" },
        { "Z:\\home\\jane doe", "<path>" },
        { "(see C:\\Users\\Jane Doe)", "(see <path>)" },
        { "C:\\Users\\Jane Doe: access denied", "<path>: access denied" },
        { "C:\\Users\\Jane Doe.txt", "<path>" },
        { "C:\\Users\\Jane Doe\\x.dll was not found", "<path> was not found" },
        // Not paths: left byte for byte.
        { "ms/frame 1/2 U of P D3D11/D3D12 on/off", "ms/frame 1/2 U of P D3D11/D3D12 on/off" },
        { "dlssnr-replay\\clip relative/dir", "dlssnr-replay\\clip relative/dir" },
        { "version=310.8.0.0 render=1920x1080 time 12:30 ratio 3:2", "version=310.8.0.0 render=1920x1080 time 12:30 "
                                                                     "ratio 3:2" },
        { "https://github.com/x/y", "https://github.com/x/y" },
        { "a / b", "a / b" },
        { "", "" },
    };
    for (const auto& [input, expected] : cases)
    {
        const auto got = DR::Redact(input);
        Require(got == expected, "Redact(" + input + ") = [" + got + "], expected [" + expected + "]");
    }

    // Redaction runs over the whole report, whichever field a path arrived in.
    auto in = FullInputs();
    in.failureReason = "nvngx_dlssnr.dll at C:\\Users\\someone\\Game\\nvngx_dlssnr.dll refused";
    in.pass = DR::Pass::NotRunning;
    in.lsfg.active = false;
    in.lsfg.fallbackReason = "shaders not found in /home/someone/.local/share/lsfg";
    in.hookStatus = "Z:\\home\\someone\\x";
    in.build = "OptiScaler v10 built in C:\\build\\someone";
    const auto text = DR::Build(in);
    Require(!Contains(text, "someone"), "no user name survives anywhere in the report: " + text);
    Require(Count(text, "<path>") == 4, "each path became <path>");
    Require(Contains(text, "Pass: off for this session: nvngx_dlssnr.dll at <path> refused"),
            "the text around a path is kept");
    DR::Inputs off;
    off.build = "built in /home/someone/src";
    Require(!Contains(DR::Build(off), "someone"), "the NR-off report is redacted too");
}
void ModelVram()
{
    // The model's own figure, 64-bit, and a known zero kept apart from no figure at all.
    auto in = FullInputs();
    in.modelVramBytes = 5ull * 1024 * 1024 * 1024 + 3;
    Require(Contains(DR::Build(in), "Model VRAM: 5120.0 MiB (5368709123 bytes)"), "a figure above 4 GiB is whole");
    in.modelVramBytes = 0;
    Require(Contains(DR::Build(in), "Model VRAM: 0.0 MiB (0 bytes)"), "a reported zero is a figure");
    in.modelVramKnown = false;
    in.modelVramBytes = 123;
    Require(Contains(DR::Build(in), "Model VRAM: not reported") && !Contains(DR::Build(in), "123 bytes"),
            "an unknown figure prints no stale number");
    DR::Inputs off;
    off.modelVramKnown = true;
    Require(!Contains(DR::Build(off), "Model VRAM"), "the NR-off report stays short");
}
} // namespace

int main()
{
    Deterministic();
    NoSectionMissing();
    DetectedBeforeUsed();
    PassStates();
    SettingsAreSettings();
    GpuSampleMarking();
    LsfgStates();
    NrOffIsShort();
    Redaction();
    ModelVram();

    if (g_failures != 0)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS: diagnostic report cases\n");
    return 0;
}
