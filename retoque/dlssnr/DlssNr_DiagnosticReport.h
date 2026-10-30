#pragma once

// The NR panel's "Copy NR diagnostics" text (docs/dlssnr/design/diagnostic-export.md).
//
// A user asked what NR is doing has to read half a dozen lines spread over the panel, some of them only in the full
// view, and then retype them into a report. The button copies them instead, as plain text, assembled when it is
// pressed and at no other time. The menu fills Inputs from getters the panel already shows and calls Build; this
// header only arranges and redacts, standard library only, so tests/nr-diagnostic-report compiles it as it is.
//
// The text keeps two things apart that the panel's lines run together:
//
//   Detected  what the game and the machine offer: API, upscaler, D3D11 route, model and NGX loader, the game's
//             exposure, an HDR swapchain
//   Used      what the pass actually ran on: whether it runs, the hook method set and the one in effect, the guides,
//             the HDR conversion
//
// then the settings (the timing contract's settings fields), the last fence-confirmed GPU sample, which is one
// historical sample and is labelled so, and LSFG. With NR off the report is the build and one line.
//
// Every absolute path is replaced by <path> (Redact), whatever field it arrived in: a failure reason or LSFG's fallback
// can name a file under a user's home or Steam library, and the text is meant to be pasted into a public issue.
//
// Not the release sidecar: tools/ writes that from the repository at packaging time; this is the running game's
// state, in the user's hands.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace DlssNr::DiagnosticReport
{
// A confirmed sample younger than this, while measurement is on, reads "recent"; anything else "historical".
inline constexpr uint64_t kRecentSampleMs = 2000;

// The pass as the panel's status line has it.
enum class Pass : uint8_t
{
    NotRunning,    // failureReason says why, or empty: waiting for the upscaler
    RunningD3D12,  // the D3D12 pass, its bridges and present hosts
    RunningVulkan, // the native Vulkan pass
};

// GpuTiming::Snapshot, the fields the export names.
struct GpuSample
{
    bool instrumented = false; // the route is timed at all: not native Vulkan, not the driver proxy
    bool measuring = false;    // [DlssNr] GpuTiming on
    bool valid = false;        // a confirmed sample exists
    double totalMs = 0.0, modelMs = 0.0, otherMs = 0.0;
    uint64_t sampleId = 0;
    uint64_t ageMs = 0; // since the sample was recorded
    uint64_t settingsGeneration = 0;
    uint64_t contractHash = 0;
    std::string stage;
    uint32_t modelWidth = 0, modelHeight = 0;
    uint32_t passes = 0;
    uint32_t cadence = 1;
    bool carried = false;
};

// Lsfg::Status.
struct LsfgState
{
    bool enabled = false;
    bool active = false;
    uint32_t width = 0, height = 0;
    double gpuMs = 0.0;
    std::string fallbackReason;
};

// Already-fetched values. Strings are copies; an empty string means the getter had nothing to say.
struct Inputs
{
    std::string build; // BuildInfo::ProductName()
    bool wine = false; // State::isRunningOnLinux
    bool nrEnabled = false;

    // Detected. Filled only with NR on.
    std::string api;       // "D3D11", "D3D12", "Vulkan"
    std::string upscaler;  // the current feature's display name
    std::string dx11Route; // Dx11Route::ReportText, the panel's amber route line
    bool modelDescribed = false;
    std::string modelVersion;
    unsigned long long modelBytes = 0;
    std::string loaderVersion;
    std::string loaderBuild;    // NgxInfo::DriverFromLoaderVersion
    std::string feature18Route; // PeScan::Describe of the loader's feature-18 entry ("routes feature 18 itself ...")
    unsigned long long exposureFrames = 0;
    bool exposureOfferedNow = false;
    bool exposureEverOffered = false;
    bool exposureLatched = false;
    bool hdrSeen = false;
    std::string hdrProfile;

    // Used. Filled only with NR on.
    bool applyModel = true;
    uint32_t hookMethod = 1;
    Pass pass = Pass::NotRunning;
    unsigned long long vulkanFrames = 0;
    std::string failureReason;
    std::string hookStatus;
    std::string guidesLine; // PresentGuidesLine
    bool hdrConverted = false;
    std::string hdrReason;
    std::string settingsContract; // SettingsContractText: TimingContract over the current Config
    GpuSample gpu;
    LsfgState lsfg;
};

namespace Detail
{
// TimingContract's fields that describe one frame rather than the configuration. The settings-contract wrapper passes
// placeholders for them, and the last confirmed sample says what a real frame had.
inline constexpr std::string_view kFrameFields[] = { "stage",   "render",     "compose",           "model",
                                                     "scale",   "linear_hdr", "compose_format",    "effective_transfer",
                                                     "enlarge", "cadence",    "highlight_recovery" };

inline bool IsAlnum(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
inline bool IsAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// Where a path may begin: the start of the text, after whitespace, after an assignment or a colon, or inside quotes
// and brackets. Not after a letter or digit, so "ms/frame" and "D3D11/D3D12" are not paths.
inline bool AtBoundary(std::string_view s, size_t i)
{
    if (i == 0)
        return true;
    switch (s[i - 1])
    {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '=':
    case ':':
    case '(':
    case '[':
    case '{':
    case '"':
    case '\'':
    case '`':
    case ',':
    case ';':
        return true;
    default:
        return false;
    }
}

// An absolute path starting at i: a drive ("C:\", "z:/", which covers Wine's Z:\home\...), a UNC or "\\?\" prefix, or
// a POSIX root followed by a name ("/home/...", "/mnt/..."; not "//" of a URL, not a lone "/").
inline bool PathStartsAt(std::string_view s, size_t i)
{
    if (i >= s.size() || !AtBoundary(s, i))
        return false;
    const auto at = [&](size_t k) { return i + k < s.size() ? s[i + k] : '\0'; };
    if (IsAlpha(at(0)) && at(1) == ':' && (at(2) == '\\' || at(2) == '/'))
        return true;
    if (at(0) == '\\' && at(1) == '\\' && (IsAlnum(at(2)) || at(2) == '?' || at(2) == '.'))
        return true;
    return at(0) == '/' && (IsAlnum(at(1)) || at(1) == '_' || at(1) == '.' || at(1) == '~');
}

// " key=" at i: a following key=value token, where a path that ran on would have ended.
inline bool KeyValueAt(std::string_view s, size_t i)
{
    if (i >= s.size() || !(IsAlpha(s[i]) || s[i] == '_'))
        return false;
    while (i < s.size() && (IsAlnum(s[i]) || s[i] == '_' || s[i] == '.'))
        ++i;
    return i < s.size() && s[i] == '=';
}

// One past a path that starts at start. A path may hold spaces ("Program Files (x86)"), so it runs to the last
// separator before the end of its stretch of text, then over that last name up to a space or closing punctuation.
// The stretch ends at a newline, a quote or a character Windows forbids in a path, at ", " or "; ", and at a space
// that begins another path or a key=value token.
inline size_t PathEnd(std::string_view s, size_t start)
{
    size_t stretch = start;
    for (; stretch < s.size(); ++stretch)
    {
        const char c = s[stretch];
        if (c == '\n' || c == '\r' || c == '\t' || c == '"' || c == '\'' || c == '`' || c == '<' || c == '>' ||
            c == '|' || c == '*')
            break;
        if ((c == ',' || c == ';') && stretch + 1 < s.size() && s[stretch + 1] == ' ')
            break;
        if (c == ' ' && (PathStartsAt(s, stretch + 1) || KeyValueAt(s, stretch + 1)))
            break;
    }
    size_t lastSeparator = start;
    for (size_t i = start; i < stretch; ++i)
        if (s[i] == '\\' || s[i] == '/')
            lastSeparator = i;
    size_t end = lastSeparator + 1;
    while (end < stretch && s[end] != ' ' && s[end] != ')' && s[end] != ']' && s[end] != '}' && s[end] != ',' &&
           s[end] != ';' && s[end] != ':')
        ++end;
    // A last name with no extension before its first space may be a folder whose name has spaces (a user's folder):
    // privacy over precision, so it runs on to the end of the stretch or closing punctuation. A file name with an
    // extension ends at the space, so the words after it ("x.dll was not found") stay.
    if (s.substr(lastSeparator + 1, end - lastSeparator - 1).find('.') == std::string_view::npos)
        while (end < stretch && s[end] != ')' && s[end] != ']' && s[end] != '}' && s[end] != ':')
            ++end;
    return end;
}

inline std::string OrText(const std::string& value, const char* fallback) { return value.empty() ? fallback : value; }

inline const char* HookMethodName(uint32_t method)
{
    switch (method)
    {
    case 0:
        return "Auto";
    case 1:
        return "Upscaled";
    case 2:
        return "Present";
    default:
        return nullptr;
    }
}
} // namespace Detail

// Every absolute path in text becomes <path>; everything else is kept byte for byte.
inline std::string Redact(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();)
    {
        if (Detail::PathStartsAt(text, i))
        {
            out += "<path>";
            i = Detail::PathEnd(text, i);
            continue;
        }
        out += text[i++];
    }
    return out;
}

// The settings tokens of a timing contract, in order, single-spaced: every key=value token whose key is not one of
// Detail::kFrameFields.
inline std::string StripFrameFields(std::string_view contract)
{
    std::string out;
    size_t i = 0;
    while (i < contract.size())
    {
        while (i < contract.size() && contract[i] == ' ')
            ++i;
        const size_t start = i;
        while (i < contract.size() && contract[i] != ' ')
            ++i;
        if (start == i)
            break;
        const auto token = contract.substr(start, i - start);
        const auto key = token.substr(0, token.find('='));
        bool frame = false;
        for (const auto field : Detail::kFrameFields)
            frame = frame || key == field;
        if (frame)
            continue;
        if (!out.empty())
            out += ' ';
        out += token;
    }
    return out;
}

// The report. Deterministic: the same Inputs give the same text, with no clock or address in it.
inline std::string Build(const Inputs& in)
{
    using Detail::OrText;
    std::string t;
    const auto line = [&t](const std::string& text) { t += text + "\n"; };

    line("Retoque NR diagnostics");
    line("Absolute paths in this text are redacted.");
    line("");
    line("== Build ==");
    line("Build: " + OrText(in.build, "unknown"));
    line(std::string("Platform: ") + (in.wine ? "Wine/Proton" : "native Windows"));
    if (!in.nrEnabled)
    {
        line("Neural Rendering: off");
        return Redact(t);
    }
    line("Neural Rendering: on");

    line("");
    line("== Detected ==");
    line("Graphics API: " + OrText(in.api, "unknown"));
    line("Upscaler feature: " + OrText(in.upscaler, "none"));
    line("D3D11 route: " + OrText(in.dx11Route, "nothing to report"));
    if (in.modelDescribed)
        line("Model file: version " + OrText(in.modelVersion, "unknown") + ", " + std::to_string(in.modelBytes) +
             " bytes");
    else
        line("Model file: not described this session (the D3D12 pass describes it when it builds)");
    if (!in.modelDescribed)
        line("NGX loader: not described this session");
    else if (in.loaderVersion.empty())
        line("NGX loader: none loaded");
    else
    {
        std::string loader = "NGX loader: version " + in.loaderVersion;
        if (!in.loaderBuild.empty())
            loader += in.wine ? " (Windows-branch loader build " + in.loaderBuild + ", not the Linux driver version)"
                              : " (driver " + in.loaderBuild + ")";
        line(loader + "; it " + OrText(in.feature18Route, "was not scanned"));
    }
    if (in.exposureFrames == 0)
        line("Game exposure: nothing observed yet");
    else
        line(std::string("Game exposure: ") +
             (in.exposureOfferedNow    ? "offered on the last frame"
              : in.exposureEverOffered ? "offered earlier, not on the last frame"
                                       : "never offered") +
             " (" + std::to_string(in.exposureFrames) + " evaluates observed)" +
             (in.exposureLatched ? "; latched as unusable" : ""));
    line(in.hdrSeen ? "HDR swapchain: seen (" + OrText(in.hdrProfile, "declined") + ")"
                    : "HDR swapchain: none seen in the last two seconds");

    line("");
    line("== Used ==");
    line(in.applyModel ? "Apply the model: yes" : "Apply the model: no (the pass runs, its edit is hidden)");
    switch (in.pass)
    {
    case Pass::RunningD3D12:
        line("Pass: running on D3D12");
        break;
    case Pass::RunningVulkan:
        line("Pass: running natively on Vulkan (" + std::to_string(in.vulkanFrames) + " frames)");
        break;
    default:
        line(in.failureReason.empty() ? "Pass: waiting for the upscaler to run"
                                      : "Pass: off for this session: " + in.failureReason);
        break;
    }
    if (const char* name = Detail::HookMethodName(in.hookMethod))
        line(std::string("Hook method (setting): ") + name);
    else
        line("Hook method (setting): " + std::to_string(in.hookMethod) + " (out of range; read as Upscaled)");
    line("Hook status: " + OrText(in.hookStatus, "nothing has run yet"));
    // PresentGuidesLine starts with "Guides:" itself.
    line(in.guidesLine.empty() ? "Guides: no present host decided in the last two seconds" : in.guidesLine);
    if (in.hdrConverted)
        line("HDR conversion: converting");
    else if (!in.hdrReason.empty())
        line("HDR conversion: not converting: " + in.hdrReason);
    else
        line(in.hdrSeen ? "HDR conversion: off" : "HDR conversion: none");

    line("");
    line("== Settings ==");
    const auto settings = StripFrameFields(in.settingsContract);
    if (settings.empty())
        line("unavailable");
    else
    {
        line("The timing contract's settings, from the current configuration:");
        for (size_t at = 0; at < settings.size();)
        {
            const auto next = settings.find(' ', at);
            line("  " + settings.substr(at, next == std::string::npos ? std::string::npos : next - at));
            at = next == std::string::npos ? settings.size() : next + 1;
        }
    }

    line("");
    line("== GPU timing ==");
    const auto& g = in.gpu;
    if (!g.instrumented)
        line("Measurement: not instrumented on this route (native Vulkan or the driver proxy)");
    else
        line(g.measuring ? "Measurement: on" : "Measurement: off");
    if (!g.valid)
        line("Last confirmed sample: none");
    else
    {
        const bool recent = g.measuring && g.ageMs <= kRecentSampleMs;
        char sample[160] {};
        std::snprintf(sample, sizeof(sample), ", %.1f s old: %.3f ms dispatch = %.3f ms model + %.3f ms outside model",
                      g.ageMs / 1000.0, g.totalMs, g.modelMs, g.otherMs);
        line(std::string("Last confirmed sample: ") + (recent ? "recent" : "historical") + sample);
        line("Sample " + std::to_string(g.sampleId) + ", stage " + OrText(g.stage, "unknown") + ", model " +
             std::to_string(g.modelWidth) + "x" + std::to_string(g.modelHeight) + ", " + std::to_string(g.passes) +
             " passes, cadence " + std::to_string(g.cadence) + (g.carried ? ", carried frame (no model call)" : ""));
        char hash[24] {};
        std::snprintf(hash, sizeof(hash), "0x%016llx", static_cast<unsigned long long>(g.contractHash));
        line("Contract generation " + std::to_string(g.settingsGeneration) + ", hash " + hash +
             " (the INFO log's contract line for it has the full text)");
        line("One sample, not the current frame's cost or an average.");
    }

    line("");
    line("== LSFG ==");
    const auto& l = in.lsfg;
    if (!l.enabled)
        line("LSFG: off");
    else if (l.active)
    {
        char cost[48] {};
        if (l.gpuMs > 0.0)
            std::snprintf(cost, sizeof(cost), " (%.2f ms/frame)", l.gpuMs);
        line("LSFG: active at " + std::to_string(l.width) + "x" + std::to_string(l.height) + cost);
    }
    else if (!l.fallbackReason.empty())
        line("LSFG: enabled, not active: " + l.fallbackReason);
    else
        line("LSFG: enabled, waiting for frames");

    return Redact(t);
}
} // namespace DlssNr::DiagnosticReport
