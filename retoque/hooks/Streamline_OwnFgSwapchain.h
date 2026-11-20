#pragma once

// When OptiScaler makes the frame generation swapchain itself, a game's own Streamline DLSS-G must not hook it too.
//
// FGHooks makes that swapchain when the FG Input is neither None nor NvngxFG and there is an FG Output (the first rule
// of CheckForFGStatus, OptiScaler/hooks/FG_Hooks.cpp). sl.dlss_g's swapchain hooks then wrap it a second time: the
// game's Streamline makes another frame generation swapchain and crashes in sl.interposer at the first present (Forza
// Horizon 6 with OptiFG; janblade/OptiScaler-F5-DLSSNR-Multipass PR #347, GPL-3.0). Upstream clears those hooks only
// for FG Input or Output DLSSG.
//
// D3D12 only. On Vulkan, clearing the plugin's JSON would also zero sl.dlss_g's queue requests, and that path is not
// checked; there the game's DLSS-G and OptiScaler's FG can still both run, as before.
namespace StreamlineOwnFg
{
// Input and Output are FGInput and FGOutput (State.h); templated so this header does not need State.h.
template <typename Input, typename Output> constexpr bool MakesFgSwapchainD3D12(bool vulkan, Input input, Output output)
{
    return !vulkan && input != Input::NoFG && input != Input::NvngxFG && output != Output::NoFG;
}

// The game's DLSS-G is replaced (not merely fed, as with FG Input DLSSG) when its hooks are cleared for any other
// input.
template <typename Input> constexpr bool ReplacesGameDlssg(bool hooksCleared, Input input)
{
    return hooksCleared && input != Input::DLSSG;
}
} // namespace StreamlineOwnFg
