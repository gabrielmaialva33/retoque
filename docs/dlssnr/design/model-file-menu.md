# Model file list in the menu (designed, not built)

Date: 2026-11-03. Status: **designed, reviewed, parked**. Nothing is implemented; `[DlssNr] ModelFile` stays an
INI setting ([model-file.md](model-file.md)).

## What was designed

A combo in the NR panel listing the model files found:

- the files `nvngx_dlssnr*.dll` beside OptiScaler and beside the game;
- the files one level under `DlssNrModels/`.

The scan was to run on a worker thread, because the menu is drawn inside the game's Present. Each entry was to
save a value that v1's resolver maps back to that same file. A copy shadowed by an earlier folder was to be shown
greyed out. Separate lines were to say "this session", "next start" and "UseProxy refused".

## Why it is parked

An independent review on 2026-11-03 found 1 P1, 9 P2 and 13 P3. The main points:

- **The panel could not say which model is running.** The design took the last resolution from any caller as
  "loaded". Resolutions keep happening after the model is loaded (the chained-pass create, the pair capture,
  the Vulkan builders), so a renamed or moved file would make the panel contradict the running model.
  - v1's own identity had the same weakness. It is fixed separately: the module identity is now the first named
    file found (model-file.md).
- **The value written could differ from the one shown.** The combo preview would show raw INI text, including a
  user's folder. The "From the INI" row confused the start-up value with the edited one.
- **The control could be dead.** While the scan runs, or after it fails, the combo holds only "Automatic". For
  most users, with one `nvngx_dlssnr.dll`, its only effect would be to narrow the search and refuse UseProxy.
- **A worker thread in the DLL**, alive at unload and logging after the logger closes; the scan running with NR
  off.
- **Missing coverage.** The Vulkan present host was missing from "loaded". The native Vulkan refusal reason never
  reaches the menu. Wine on a case-sensitive disk can list two names that differ only in case.

## If it is picked up

The review's simpler shape keeps most of the user value:

- show only the frozen start-up value for this session, not a per-session state table;
- list only files that round-trip through the resolver;
- scan only when a button is pressed, on the menu thread, bounded;
- write the INI value the user can read, never a path outside the two search folders.

The diagnostics report is already fixed: it describes the named model (model-file.md).
