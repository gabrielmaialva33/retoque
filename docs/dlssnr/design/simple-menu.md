# Simple Neural Rendering menu

The default panel keeps Enable, the session-guard notice, Apply the model, status/route diagnostics and the
startup game-profile summary. Below those, `Show all settings` is a persistent menu preference:
`[DlssNr] ShowAllSettings=auto` means false. It does not participate in renderer settings or feature rebuilds.

With the checkbox off, show only:

- the existing model preset selector and active weight-set report; one known weight set hides the selector;
- requested pass count and last-evaluation status on D3D12 routes, including bridges; native Vulkan and the
  driver proxy have no pass control. Individual tuning stays in the full view and saved overrides remain active;
- the existing Detail strength slider, reset button and help;
- the existing fence-confirmed timing readout when measurement is enabled on its supported backend, without
  measurement or sampling controls. Historical samples retain their age and caveats.

Checking the box shows the previous panel in its original order. Preset, strength and timing presentation
are extracted into shared functions. The pass editor gains a presentation-only argument to stop after its
count/status block in simple view. Both views write exactly the same Config members/transactions, with the
same labels and ImGui identities. Switching views never clears or resets hidden settings.

Visibility review: DEVELOPMENT.md rule 2 still hides a one-weight-set preset and unsupported pass controls.
The full view retains the white-point, D3D11 route, capture and guide conditions of DEVELOPMENT.md section 2.
No render, shader, model or timing configuration is changed by the view preference itself.

Validation: scripted production menu helpers, source placement guards, config round-trip, localization,
full host tier and a Release build. These do not validate pixel layout or game behavior; no game is deployed.
