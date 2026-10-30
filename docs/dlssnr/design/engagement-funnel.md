# NR engagement funnel and crash note

Status: built 2026-10-30 on `feat/nr-funnel-crash-report-20261030`; host-tested by `tests/nr-funnel`,
`tests/nr-crash-note` and `tests/nr-diagnostic-report`. **Not yet built with MSVC, run in a game, or crashed on
purpose** under Proton or native Windows, so nothing here says the counts or the note are right in a running session.

Ideas after RenoDX-DLSS5-Generic's `funnel.hpp`, `declines.hpp` and `lastgasp.hpp` (PEQHUB, MIT); no code was copied.

## The problem

Whether NR is doing anything in a game had several partial answers: the status line (did the pass run lately), the
coverage log line (calls per stage every five seconds) and `ReportSkipOnce` (each early return said once, then
silence). A session where the pass ran on one evaluate in a hundred read, from all of them, like one where it ran on
every evaluate. And when a game with NR on died, OptiScaler's log ended at whatever was last logged, saying nothing of
the death.

## The funnel

`retoque/dlssnr/DlssNr_Funnel.h`, standard library only. Each route has a `Ladder`: every evaluate that reaches it
with NR on is **eligible** and ends exactly one way, **ran** (the pass recorded; `modelCalls` counts those where the
model was called, which a carried cadence frame is not) or one named **decline**:

    eligible - ran - sum(declines) = unaccounted

`Funnel::Evaluation` is the scope: it enters at construction and counts the first outcome named; one that ends with
none counts nothing, so the ladder stays unbalanced to say so. A snapshot reads outcomes (acquire) before eligible, so
it never sees an outcome without its entry: `unaccounted` is the evaluates in flight plus any silent way out, never
negative.

Counted route: **D3D12 after the upscaler** (`EvaluateAfterUpscale`, which the D3D11- and Vulkan-on-D3D12 bridges'
after-upscale passes call too). Declines, in the order the pass meets them: ray reconstruction with `ApplyAfterRR`
off; no command list or parameters; stage 1 (the model ran before the upscaler); no output, depth or motion; deferred
to the present-time pass; several views in one colour allocation; no D3D12 device; the pass could not be built; the
pass recorded nothing (its own last reason kept, from the coverage sample). Not counted yet: the D3D12 and D3D11-bridge
present hosts and the Vulkan routes; `Funnel::Route` is where they go.

Cost, NR on only: two atomic increments and three relaxed stores per evaluate, no lock, no allocation. The scope opens
after the NR-off return, so the off path is unchanged. No config key.

### In the diagnostic report

"Copy NR diagnostics" ([diagnostic-export.md](diagnostic-export.md)) has a **Funnel** section between Used and
Settings: per route, `eligible, ran the pass (model called), declined, unaccounted`, then every decline with its count,
zero included so two reports diff line for line. A positive unaccounted is explained in the line; the menu reads each
ladder once, on the click.

## The crash note

`retoque/dlssnr/DlssNr_CrashNote.h` (pure formatting) and `DlssNr_CrashNote.cpp` (Win32). At the first evaluate with
NR on (D3D12 after-upscale or present), `CrashNote::InstallOnce` opens a second handle on OptiScaler's existing log
(`FILE_APPEND_DATA`, `OPEN_EXISTING`, shared read/write/delete so neither spdlog nor `PrepareLogger`'s rotation is
refused) and puts an unhandled-exception filter in place, keeping the one it replaces. Nothing with `LogToFile` off,
nothing with NR off. Later calls cost one relaxed load.

When the process dies of an unhandled exception the filter writes three lines in the log's own shape:

    [hh:mm:ss.ffffff] [E] DLSS-NR crash note: unhandled exception 0xC0000005 (access violation, write at 0x...) at nvngx_dlssnr.dll+0x1a2b3c, thread N
    [hh:mm:ss.ffffff] [E] DLSS-NR crash note: last NR stage: D3D12 after the upscaler, recording the pass, on this thread
    [hh:mm:ss.ffffff] [E] DLSS-NR crash note: funnel d3d12_after_upscale eligible=... ran=... model=... declined=... unaccounted=... top=<tag>:<count>

then hands the exception to the previous filter, or continues the search when there was none. It never handles the
crash. Constraints, each guarded by `tests/nr-crash-note`:

- Formatting by hand into a stack buffer: no allocation, no printf, no locale, no logger, no `Config`.
- The module is found with `VirtualQuery` and named from its own PE export directory, read inside `__try`; the
  executable's base is taken at install. No `GetModuleHandle*`/`GetModuleFileName` in the filter (they may take the
  loader lock; Wine's do). Export names are sanitised to `[A-Za-z0-9._+-]`: no path reaches the log.
- A stack overflow writes one fixed line and formats nothing.
- Re-entry (a fault in the note, a chained filter calling back) continues the search at once; a fault inside the note
  is contained and the chain still runs.
- Last-chance only, no vectored handler: a vectored handler sees every first-chance exception a game raises and
  handles. Upstream installs neither; this is the only handler in the tree.
- On an unload (`DLL_PROCESS_DETACH` with `lpReserved == NULL`) the slot is given back, only while it is still ours;
  at process exit nothing is undone.

Known gaps: an engine that sets its own filter after ours without chaining takes the slot, and then nothing is
written; a `__fastfail` never reaches a filter; with a debugger attached the filter is not called; with `LogAsync`
queued lines may follow or be lost, the note does not wait for them; after a log rotation from the panel the note
lands in the `.previous.log` the handle followed.

## What needs an in-game or native Windows check

1. Build with MSVC (`build-local.sh`): the new unit compiles, `__try` in `WriteNote` is accepted (no unwinding
   objects), and the funnel header compiles in `DlssNr_Dx12.cpp` and the menu.
2. A D3D12 title with NR on: "Copy NR diagnostics" shows the Funnel with `unaccounted 0` (or a small in-flight count),
   eligible rising, and declines matching the panel (HookMethod=2 shows "deferred to the present-time pass").
3. The log shows "DLSS-NR crash note: in place" once, with whether a filter was there before it.
4. A deliberate crash with NR on (a test build or a debugger-free fault injection), under Proton and on native
   Windows: the three lines appear at the end of the log, the module and offset are right, and the game's own crash
   reporter (or WER) still runs afterwards. Under Proton, confirm Wine reaches the top-level filter at all.
5. NR off for the whole session: no "in place" line and no filter change.
