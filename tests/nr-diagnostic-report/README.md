# NR diagnostic report

```bash
python3 tests/nr-diagnostic-report/run.py
```

The NR panel's "Copy NR diagnostics" button ([diagnostic-export.md](../../docs/dlssnr/design/diagnostic-export.md)).
The runner compiles `retoque/dlssnr/DlssNr_DiagnosticReport.h` as it is (standard library only) with g++ C++20,
ASan/UBSan and `-Werror`, runs `cases.cpp`, and checks the production call sites by source.

## What this covers

- **Determinism.** The same `Inputs` give the same bytes; a changed input changes them; `\n` only; nothing redacted
  in a report without paths.
- **No section missing.** Build, Detected, Used, Funnel, Settings, GPU timing and LSFG once each, in that order, for a full
  report and for one with every field at its default; every line is present, unknown values read `unknown`/`none`.
- **Detected before Used.** API, upscaler, model and HDR swapchain sit under Detected; pass, hook method, guides and
  HDR conversion under Used. Under Wine the loader build is not called the driver.
- **States.** The pass running on D3D12, natively on Vulkan, off with its reason, or waiting; Apply off; each hook
  method and an out-of-range one; HDR converting, declined or absent; the D3D11 route line; LSFG off, active, timed,
  falling back and waiting.
- **Funnel.** Between Used and Settings: each route's ladder on one line (eligible, ran, model called, declined,
  unaccounted), every decline with its count (zero included) and its last reason; a positive unaccounted is
  explained, an impossible one shown signed; no route reads `No route counted.`
- **Settings.** `StripFrameFields` keeps settings tokens in order and drops every frame field; one per line in the
  report; `unavailable` when there is no contract text.
- **GPU sample.** Recent only while measuring and under `kRecentSampleMs`; historical otherwise; carried frames named;
  none; not instrumented.
- **NR off.** Build, platform and `Neural Rendering: off`, at most eight lines, nothing of the pass even when the
  struct holds it.
- **Redaction.** Drive, Wine `Z:\`, POSIX, forward-slash Windows, `\\?\` and UNC paths, paths with spaces, quoted,
  parenthesised, followed by `key=value`, `:`, `, ` or another path; non-paths (`ms/frame`, `1/2`, relative paths,
  URLs, times, ratios) untouched; a user name in any field of a full report, or in the NR-off report, does not survive.
- **Source guards.** The header includes only standard headers. The panel assembles the text inside the button's click
  branch and passes it straight to `SetClipboardText`; the collector returns before any NR, LSFG or timing getter when
  NR is off, never calls `GpuTiming::SetEnabled`, `RetryAfterFailure` or `NgxInfo::Describe`. `SettingsContractText`
  calls the one `TimingContract`, and every frame field `TimingContract` writes is in `kFrameFields`.
  `NgxInfo::Described` looks up under the lock and never adds or logs. The collector reads every route's ladder from `DlssNr::Funnel`.

## What this does not cover

No ImGui, clipboard, Config or DLL is involved: `CollectDiagnosticReport` is checked by source, not run, and its
MSVC compile is a separate step. Whether the pasted text matches the panel in a running game, under Proton and on
native Windows, is the in-game check listed in the design note. The pt-BR entries for the button are checked by
`nr-localization`.
