# NR producer detection and hand-over

Run from the repository root:

```sh
python tests/nr-consumers/run.py
```

Requires Python and GCC with C++20/ASan/UBSan. Compiles `retoque/dlssnr/DlssNr_ConsumerMarkers.h` and checks
that the module-name markers find the neural add-ons (renodx-dlss/renodx-dlss5, Deep Fried Chicken,
DLSS5-Feeder) in any case and nothing else in the process: RenoDX's per-game HDR add-ons, ReShade, OptiScaler
under any proxy name, NVIDIA's core and DLSS DLLs and our forwarder. It then checks the source contract of
`[DlssNr] YieldToAddons`: declaration with default true, read, save, shipped INI and Config.md, and that both
model loaders (D3D12 `EnsureForwarder`, Vulkan `LoadForwarderShared`) call `HandOverIfCompeting()` before
loading anything of the model, that the D3D12 route entries (`EvaluateAfterUpscale`, `RunPresentPass`) hand over
before any gate of ours can decline with another reason, that standing down uses the volatile `Enabled` value like
the toggle key, and that `ResTrack_Dx12::NrRealObject` unwraps ReShade 6.7+ proxies as well as Streamline's.

Passing does not prove that a given add-on is loaded before our first NR feature in a real game, that the
add-on then runs over our upscaler's output, or anything about image quality; that needs a game run with
ReShade loaded through `[Plugins] LoadReshade`.
