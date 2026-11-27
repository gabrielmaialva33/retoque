#pragma once

// Read-only Status rows, inspired by janblade/OptiScaler-F5-DLSSNR-Multipass
// 8c7d6c72 and 0b29d5c5 (GPL-3.0). Reimplemented for Retoque's existing diagnostic getters.
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace DlssNr::StatusPage
{
enum class Backend
{
    None,
    D3D12,
    Vulkan
};

struct Inputs
{
    bool enabled = false;
    bool apply = true;
    bool hideHook = false; // native D3D11 no-pass notice supersedes HookStatus's configured active route
    Backend backend = Backend::None;
    std::string failure, hook, route, model, cadenceStatus;
    uint32_t passes = 1, cadence = 1;
    float scale = 1.0f, rrScale = 1.0f;
};

struct Row
{
    const char* label;
    std::string value;
};

inline std::string StateText(const Inputs& in)
{
    if (!in.enabled)
        return "Off";
    if (!in.failure.empty())
        return "Failed / unavailable: " + in.failure;
    if (in.backend == Backend::None)
        return "On, waiting for the model";
    // IsRunning reports a retained feature, not proof that the current frame ran the model.
    return std::string("On (") + (in.backend == Backend::Vulkan ? "native Vulkan" : "D3D12") + "; model loaded)" +
           (in.apply ? "" : "; edit hidden");
}

inline std::string ScaleText(float scale, float rrScale)
{
    char text[96];
    std::snprintf(text, sizeof(text), "%.0f%%; after RR %.0f%% (resolved settings)", scale * 100.0f, rrScale * 100.0f);
    return text;
}

inline std::string ModelText(const std::string& model)
{
    if (model.empty())
        return "Auto (nvngx_dlssnr.dll search)";
    // Screenshots must not expose the user's model folder. Both Windows and POSIX separators.
    return model.substr(model.find_last_of("/\\") + 1) + " (startup selection)";
}

inline std::vector<Row> Rows(const Inputs& in)
{
    std::vector<Row> rows { { "Neural Rendering", StateText(in) } };
    if (!in.enabled)
        return rows;
    if (!in.hideHook)
        rows.push_back({ "Route", in.hook.empty() ? "Not reported" : in.hook });
    if (!in.route.empty())
        rows.push_back({ "D3D11 route", in.route });
    rows.push_back({ "Passes requested", std::to_string(in.passes) + (in.passes == 1 ? " pass" : " passes") });
    rows.push_back({ "Working scale", ScaleText(in.scale, in.rrScale) });
    rows.push_back({ "Model file", ModelText(in.model) });
    rows.push_back(
        { "Cadence requested", in.cadence <= 1 ? "Every frame" : "One model frame in " + std::to_string(in.cadence) });
    if (in.backend == Backend::D3D12 && !in.cadenceStatus.empty())
        rows.push_back({ "Cadence status", in.cadenceStatus });
    return rows;
}
} // namespace DlssNr::StatusPage
