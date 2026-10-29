# Retoque architecture and ownership map

Navigation reviewed against source snapshot `9778aeb5` on 2026-10-29. This map describes responsibilities,
not universal compatibility or performance; those are bounded by the [capability evidence](capabilities.md).

## Repository boundaries

| Area | Responsibility | Boundary to preserve |
|---|---|---|
| `OptiScaler/` | Upstream proxy/loading, hooks, config, input/output providers and overlay integration | Keep upstream paths and minimize fork-owned changes |
| `retoque/dlssnr/` | NR public API, runtime/model integration, routing, guides, capture, status and menu | External call sites remain narrow; NR off is inert |
| `retoque/shaders/dlssnr/` | GPU pass orchestration, D3D12 execution units and embedded shader sources/bytecode | CPU/HLSL layout and submission/lifetime contracts |
| `retoque/shaders/synth_motion/` | Shared motion estimation used by NR and synthesized FG | No duplicate estimation when the consumers share a frame |
| `retoque/lsfg/` | LSFG dispatch graph, library loading, UI and MFG presenter | User supplies licensed shader/model data; no redistribution |
| `retoque/inputs/FG/`, `retoque/framegen/` | Synthesized FG inputs and fork-owned provider additions | Input, interpolation and presentation remain distinct |
| `tests/` | Registry, host contracts, runtime fixtures and evidence processing | Stable suite IDs, explicit skips, no host-to-GPU inference |
| `tools/` | Build/deploy/release orchestration, diagnostics and replay analysis | Explicit inputs, owned processes, retained evidence |
| `docs/` | Current guidance, decisions, measurements and historical records | One authority per question; dated proof is not current status |

## Runtime flow

The game loads a proxy DLL. Upstream exports forward ordinary calls and hook supported graphics/upscaler
entrypoints. Upscaler inputs normalize into NGX parameters; provider selection chooses the implementation.
The NR integration enters either at an eligible upscaler boundary or on a supported present route. Those are
alternative placements, not an instruction to apply NR twice.

NR prepares the model input, evaluates the selected runtime route and composes its contribution into the original
image. When frame generation is involved, the integration must establish the correct pre-FG boundary or decline;
not every present is a new real frame. Present routes may have different guide availability than upscaler routes.
The Vulkan present host is separately opt-in and has its own FG stand-down policy.

Read the [NR module index](../retoque/dlssnr/README.md), [synthesized-motion module](../retoque/shaders/synth_motion/README.md)
and [LSFG module](../retoque/lsfg/README.md) for file-level entrypoints.

## Renderer boundaries

The D3D12 split already has an explicit [shared-state contract](dlssnr/design/dx12-internal.md).
The following responsibility units are implemented; this organizational change does not move their code.

| Unit | Owns |
|---|---|
| `DlssNr_Dx12.cpp` | Remaining orchestration, shared storage, route/present integration and shutdown |
| `DlssNr_Dx12_Internal.h` | Private declarations, never duplicate storage or header-local caches |
| `DlssNr_Dx12_Model.cpp` | Model loader/capability/fault integration |
| `DlssNr_Dx12_Resources.cpp` | Resource/format helpers and scoped state restoration |
| `DlssNr_Dx12_Inputs.cpp` | Frame/guide input interpretation |
| `DlssNr_Dx12_Enlarge.cpp` | Private reconstruction of the NR contribution |
| `DlssNr_Dx12_Lifetime.cpp` | Admission, retirement, parking and related decisions |
| `DlssNr_Dx12_Diagnostics.cpp` | Reporting and diagnostics |

All these files remain under `retoque/shaders/dlssnr/`. Further present/guide/synth-motion extraction is a future
change, not part of the documentation/traceability work. Do not replace proven synchronization with generic
ownership abstractions during a move. Preserve source-slice tests while gradually making smaller real units
compilable; [test support](../tests/_support/nr_sources.py) documents the existing bridge.

## Configuration, state and ownership

`Config::Instance()` owns INI settings; `State::Instance()` exposes runtime facts inherited from OptiScaler.
Use the existing scoped hook/state guards rather than toggling shared flags by hand. A setting's default,
requested value and effective runtime decision are different facts; diagnostics must not conflate them.

GPU objects outlive CPU calls. Recorded, submitted, completed and abandoned work are distinct states. See
[submission tracking](dlssnr/design/submission-tracking.md), [D3D12 ownership](dlssnr/design/dx12-resource-ownership.md)
and [Vulkan recording lifetime](dlssnr/design/vulkan-recording-lifetime.md). These contracts are stronger than
an assumption that a resource becomes unused after a fixed number of frames.

## Research versus product

`tools/replay/` and `tools/lsfg-study/` contain reproducible research tooling, not automatically supported runtime
features. Design notes retain rejected results. Temporary reference clones and raw captures remain local/ignored;
record source revisions, commands and bounded summaries so a conclusion does not depend on a mutable clone.

## Validation and distribution

The [test registry](../tests/suites.toml) owns suite identity/requirements. The [catalog](../tests/CATALOG.md)
is generated. The [capability index](capabilities.md) connects behavior to scoped evidence; the
[release manifest](releases/VALIDATION.md) binds an archive to its member hashes and source-test evidence.
Neither is a substitute for running the exact distributed binaries on the target environment.
