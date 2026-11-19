#pragma once

// One Config-only admission policy shared by the menu, heartbeat and Dispatch.
// Templates keep the same policy executable against host Config fakes, without Windows includes.
namespace DlssNr::AdvisorPolicy
{
enum class Gate
{
    Hidden,
    Stage,
    Adaptive,
    Periphery,
    Hold,
    TimingDisabled,
    TimingSparse,
    Ready
};
template <class Config> Gate CheckConfig(const Config& config, bool nativeVulkan)
{
    if (nativeVulkan || !config.DlssNrEnabled.value_or_default() || config.DlssNrUseProxy.value_or_default() ||
        config.DlssNrDx11SceneStageAtStart())
        return Gate::Hidden;
    if (config.DlssNrStage.value_or_default() != 0)
        return Gate::Stage;
    if (config.DlssNrCadenceAdaptive.value_or_default())
        return Gate::Adaptive;
    if (config.DlssNrPeripheryCompression.value_or_default())
        return Gate::Periphery;
    if (config.DlssNrHoldFrame.value_or_default())
        return Gate::Hold;
    const auto timing = config.GetDlssNrGpuTimingSettings();
    if (!timing.Enabled)
        return Gate::TimingDisabled;
    if (timing.Interval != 1)
        return Gate::TimingSparse;
    return Gate::Ready;
}
} // namespace DlssNr::AdvisorPolicy
