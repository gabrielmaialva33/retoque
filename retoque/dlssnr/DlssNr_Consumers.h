#pragma once

// Is another DLSS 5 neural-rendering consumer in this process?
//
// Every other add-on in the scene reaches the neural model by detouring NVIDIA's core
// (_nvngx.dll). We do not: we ARE the NGX implementation the process talks to, and our LoadLibrary
// hooks hand our own module to anything that asks for nvngx.dll or _nvngx.dll. So when both are
// installed, the other add-on's module is handed ours, our dlss backend then calls the real core,
// and its detours fire there -- a second neural pass over a frame that already had one. That is not
// a quality note, it is a wrong image, and nothing about it is visible without being told.
//
// Detection is by module name, because the mechanism itself cannot be observed from here: a detour
// on the core looks like the core. The list is therefore what is known today, not a guarantee.

// Coexistence: ReShade add-ons are loaded by ReShade when the game creates its first device, which is
// before our first NR feature, so a producer that is going to be here is already here when our model
// loader runs. HandOverIfCompeting() is called there, BEFORE anything of the model is loaded: with
// [DlssNr] YieldToAddons (default true) it turns NR off for this session exactly as the toggle key does
// (Enabled set volatile, the ini is not rewritten), so every NR path behaves as with Enabled=false and
// the other producer runs alone over our upscaler's output. The decision is made once per process.

namespace DlssNr::Consumers
{
// True when NR must stand down for another producer in this process (said once on the log, NR is
// then off for the session); false when there is none, or YieldToAddons=false keeps ours running.
bool HandOverIfCompeting();

// Warn once per process, on the log. Call after the neural forwarder has loaded -- by then anything
// that was going to hook the core has been in the process for a long time, and we know the neural
// pass is actually live, so the warning is never printed at someone who is not running it.
void WarnIfCompeting();
} // namespace DlssNr::Consumers
