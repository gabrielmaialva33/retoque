# NR pass menu verification

Run `python tests/nr-menu/run.py` from the repository root. Requires Python and GCC
with C++20, ASan and UBSan. Outputs are temporary and excluded from release packaging.

The suite extracts the actual deferred slider and pass-editor functions from
`DlssNr_Menu.cpp`, then compiles them against scripted ImGui events and a Config facade
using the production sparse settings types. Fifteen frames cover unsupported backends,
one/master defaults, every optional field, live inheritance, explicit overrides,
field/pass clearing, active-pass selection, retained inactive settings and disabling then
reenabling individual tuning. Four more cover the two controls the model itself makes inert: the
per-pass preset is not drawn once the model reports one weight set (`DlssNr::ModelLog`,
faked here), and skin structure is drawn disabled, taking no click, while the auto mask is off. Twelve
more cover model cadence's control (`docs/dlssnr/design/model-cadence.md`): nothing on native Vulkan; with
the driver proxy or a build without its shader, no control and one line only when a cadence is
configured; the combo writes 1..4; the frame-generation opt-in and the status line only above a
cadence of 1; a hand-edited out-of-range value reads as off. Source guards reject direct master writes and check both
new runtime translation units are registered exactly once in the project and filters.
This complements the real Config transaction/codec and real ImGui hash suites. It does
not render pixels or exercise the actual Windows Config, renderer, NGX or GPU.

## Menu state and adversarial review

| State | Controls and meaning |
| --- | --- |
| Native Vulkan (including before first NR evaluation) or driver proxy | Single/master limitation text; no count or individual editors |
| D3D12, individual off | Requested count 1..10 (1..3 before 2026-11-11), from two passes a cost line ("Cost: Nx the model's GPU time and N model instances in video memory."), last-evaluation count/status, individual toggle; no per-pass editor |
| D3D12, individual on, one requested pass | Pass 1 fields; no meaningless pass-selection slider |
| D3D12, individual on, multiple requested passes | One selector limited to requested passes (up to ten), not one block per pass; seven optional overrides |
| Field inherited | One attribute row with Inherited state and current master value; checkbox creates an explicit value |
| Field explicit | Same row with Explicit state and editor; unchecking clears the sparse value |
| No explicit fields | No clear-pass button |
| At least one explicit field | Inherit-all button removes only the selected pass's overrides |
| Lowered count or individual off | Inactive overrides retained, never silently deleted |
| Renderer restart/refusal/waiting | Actual renderer status shown; requested count is never described as completed work |
| Model cadence, native Vulkan | Nothing |
| Model cadence, driver proxy or no shader | No control; one line saying why, only when a cadence above 1 is configured |
| Model cadence 1 (off) | The combo only |
| Model cadence 2..4 | The combo, the frame-generation opt-in, the renderer's cadence status |

The Model section continues to edit master values through short Config setters. The
renderer may rebuild after changes; no Config lock is held over retry or renderer calls.
Deferred values commit on release; pass scopes also separate pending ImGui IDs. The
white-point source and anchor block is untouched by this integration: source 0 has
paper white; source 1 has exposure trim; source 2 without anchors has paper white and no
trim; source 2 with anchors has scan trim, with paper white only while editing a point;
the direction checkbox remains limited to exactly one anchor. The added controls change
neither the white-point settings nor exposure availability conditions.

The optional Portuguese pack translates visible attribute names and inherited/explicit states. The row layout
adds no ID scope: the underlying editors retain their original IDs, including the legacy per-pass choice IDs.
Unknown dynamic diagnostic reasons still fall back to English. No in-game layout qualification is claimed.

Backend visibility additionally checks the known input API and whether the current
feature uses the D3D12 bridge, because `IsRunningVk()` alone is false before the first
native NR evaluation. Unknown Vulkan features hide the editor conservatively. This
selection is inspected in source; the scripted ImGui harness receives the resulting
backend flag rather than constructing the real State/feature graph.

## Simple view

The harness compiles the production simple-view gate, shared appearance and comparison controls, the active
weight-set report and timing readout. Scripted frames check retained hidden presets/passes/overrides, one view
per frame, look recipes, detail strength, comparison mode/zoom/split writes and balanced item widths.
Source guards put the simple return before Processing route and require a single shared comparison body.

The simple player view now keeps Look, Detail strength and capture, then Show all settings.
Preset, passes and timing are full-view controls. Scripted frames assert retained hidden
settings and that a toggle never draws both views in one frame.
