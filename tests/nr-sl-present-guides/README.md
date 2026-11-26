# Streamline Present guides (b1)

Host policy and production adapter tests for [the approved b1 slice](../../docs/dlssnr/design/present-depth-without-upscaler.md#streamline-tag-option-b1-review-2026-11-26).
Run `python3 tests/nr-sl-present-guides/run.py`. No Wine, D3D12 runtime, GPU, game or shader compiler.
The owning clock advances before optional NR work, with no capture; default/bridge decisions retain the old clock.
Complete token/viewport-matched pairs, explicit constants, bounded identity, upscaler priority and refusal are covered.
Only `eOnlyValidNow` is implemented. An open DIRECT list, observed successful Reset and absence of a render pass
are mandatory. Owned source references and clone destruction/reuse follow the existing recording-based retirement contract.
HUD-less inputs are unused: no same-frame GPU guard has been specified. H1 remains unchanged.
Host tests do not qualify detour coexistence, runtime states, visual quality, native Windows or Proton.
