# Copy NR diagnostics — the panel's state as plain text

Status: built 2026-10-30 on `feat/nr-diagnostics-export-20261030`; host-tested by `tests/nr-diagnostic-report`.
**Not yet built with MSVC or pressed in a game**, so nothing here says the clipboard text is right in a running
session. Roadmap candidate 2 ([roadmap](../roadmap.md)): a user-facing diagnostic export that separates detected inputs
from inputs actually used. It is not the release sidecar that `tools/` writes at packaging time; that describes the
package, this describes the running game.

## The problem

What NR is doing in a game is spread over the panel: the status line, the D3D11 route line, the hook status, the
present guides line, the HDR line, the fence-confirmed GPU sample (only in the full view), and LSFG's status under
another tab. A report asked of a user arrives as a screenshot of whichever of those were on screen, or retyped. The
build, the NGX loader's version and whether the game runs under Proton are in the log, which users rarely attach.

## The design

One button, **Copy NR diagnostics**, under the status lines in both the simple and the full view
(`RenderDiagnosticExport` in `retoque/dlssnr/DlssNr_Menu.cpp`). Pressing it reads the getters the panel already
reads, once, arranges them into plain text and puts the text on the clipboard with `ImGui::SetClipboardText`, the
pattern upstream's "Copy Code" uses for overlay settings (`menu_common.cpp`). A dimmed "Copied to the clipboard."
follows the button for three seconds.

The split:

- `CollectDiagnosticReport` (menu) fills `DiagnosticReport::Inputs`, a plain struct of copies. It runs only inside the
  button's click branch.
- `DiagnosticReport::Build` (`retoque/dlssnr/DlssNr_DiagnosticReport.h`, standard library only) arranges the text and
  redacts it. It reads no clock and no address, so the same inputs give the same bytes.

### Sections

| Section | Lines | Source |
|---|---|---|
| Build | build, platform (Wine/Proton or native Windows), Neural Rendering on/off | `BuildInfo::ProductName`, `State::isRunningOnLinux` |
| Detected | graphics API; upscaler feature; D3D11 route line; model file version and size; NGX loader version, its build (named the driver on Windows, the Windows-branch loader build under Proton) and how it routes feature 18; whether the game offers exposure; whether an HDR swapchain was seen | `State`, `Dx11RouteReport`, `NgxInfo::Described`, `GameExposureStatus`, `GetPresentHdrStatus` |
| Used | Apply the model; whether the pass runs (D3D12, native Vulkan with its frame count, or the failure reason / waiting); the hook method set and the hook status in effect; the present guides line; the HDR conversion | `IsRunning`, `IsRunningVk`, `FramesVk`, `FailureReason`, `HookStatus`, `PresentGuidesLine`, `GetPresentHdrStatus` |
| Funnel | per counted route: eligible, ran the pass (model called), declined, unaccounted, then every decline with its count; added 2026-10-30 ([engagement-funnel](engagement-funnel.md)) | `Funnel::Read` |
| Settings | the timing contract's settings fields, one per line | `SettingsContractText` |
| GPU timing | measurement on/off or not instrumented; the last fence-confirmed sample marked **recent** (measurement on and younger than 2 s) or **historical**, its sample, stage, model size, passes, cadence and contract generation/hash; "One sample, not the current frame's cost or an average." | `GpuTiming::GetSnapshot` |
| LSFG | off; active at a size, with its cost when measured; enabled with its fallback reason; or waiting | `Lsfg::GetStatus` |

Detected is what the game and the machine offer; Used is what the pass ran on. The panel's guides line mixes the two
(the guides used, and how many the identity rules refused); it is reported whole under Used, because that is what it
answers. Lines are always present: a value that is not known reads `unknown`, `none` or `not described this session`,
so two reports diff line for line.

With NR off the report is the build, the platform and `Neural Rendering: off`, and nothing past those is read.

### The settings

`TimingContract` is the one text of every setting that shapes the picture (the timing contract, and a comparison
capture's provenance). `SettingsContractText` (`DlssNr_Dx12_Diagnostics.cpp`) calls it over the current `Config`
with placeholder frame arguments; `StripFrameFields` removes the fields that describe one frame rather than the
configuration (`stage`, `render`, `compose`, `model`, `scale`, `linear_hdr`, `compose_format`, `effective_transfer`,
`enlarge`, `cadence`, `highlight_recovery`). The last confirmed sample says what a real frame had, and its contract
generation and hash find the full contract line in the INFO log. A setting `TimingContract` gains appears in the
export without a change here; a new frame field has to be added to `kFrameFields`, and `tests/nr-diagnostic-report`
fails until it is.

The settings come from `Config` at the click, not from the frame that last ran: a setting changed since is reported as
set. That is why the GPU sample carries its contract generation.

### NGX facts without describing

`NgxInfo::Describe` keeps its first answer per model path, and `LoaderRouteKnownToFault` (the `UseProxy` refusal for
the 616.64 pairing, [ngx-driver-616](ngx-driver-616.md)) decides from it. If the export were the first caller, before
the NGX proxy loaded the loader, it would freeze a snapshot without one and change that decision. The export uses
`NgxInfo::Described`, a lookup under the same lock that adds and logs nothing. Before the D3D12 pass builds (and on
native Vulkan, which does not describe) the model reads `not described this session`.

### Redaction

The text is meant for a public issue. `Redact` replaces every absolute path with `<path>`: a drive path (`C:\`,
`c:/`, Wine's `Z:\home\...`), a UNC or `\\?\` path, and a POSIX path (`/home/...`, `/mnt/...`) at a word boundary.
A path may hold spaces (`Program Files (x86)`), so it runs to the last separator before the end of its stretch, then
over that last name; the stretch ends at a newline, a quote or a character Windows forbids in paths, at `, ` and `; `,
and at a space that begins another path or a `key=value` token. `ms/frame`, `1/2`, `D3D11/D3D12`, relative paths and
URLs are left alone. The redaction runs over the whole text, so a path inside a failure reason or LSFG's fallback is
caught whichever field carried it. A last name with no extension before its first space may be a folder whose name has
spaces (`C:\Users\Jane Doe`), so it runs on to the end of the stretch or to `)`, `]`, `}` or `:`: privacy over
precision, at the cost of an occasional trailing word after an extensionless path. A file name with an extension still
ends at the space, so `x.dll was not found` keeps its words.

### Language

The button, its tooltip and the acknowledgement go through `Localization` and have pt-BR entries. The report itself
stays English: it is read by whoever answers the issue, and it quotes getters whose texts are English in the log.

## Invariants

- NR-disabled path: no per-frame work and no render state. The button is drawn with the rest of the panel; its only
  state is the menu-local time of the last copy. With NR off a press reads the build and the platform only.
- Read-only: the export never calls `GpuTiming::SetEnabled`, `RetryAfterFailure` or `NgxInfo::Describe`
  (`tests/nr-diagnostic-report` guards each).
- Rule 2 (no dead control): the button always has something to copy; with NR off it is the short report.
- No config key, shader, constant buffer or resource lifetime changes.

## What needs an in-game check

1. Build with MSVC (`build-local.sh`) and confirm the panel draws the button in the simple and the full view.
2. Press it in a D3D12 title with NR running, under Proton and on native Windows: paste the text, confirm each line
   matches the panel (status, hook status, guides line, GPU sample) and that Platform names the right one.
3. With GPU timing on, confirm the sample reads recent; switch timing off, press again, and confirm it reads historical.
4. Press it with NR off: the three-line Build section and nothing else.
5. Native Vulkan: `Pass: running natively on Vulkan`, GPU timing not instrumented, model not described.
6. A game installed under a path with the user's name (or a failure naming the model path): confirm the pasted text
   holds `<path>` and no user name.
7. With the optional pt-BR pack active, the button and tooltip are translated and the report is English.
