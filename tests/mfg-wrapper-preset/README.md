# MFG wrapper and DRS preset

Run `python3 tests/mfg-wrapper-preset/run.py`. Compiles production policies with g++ C++20,
ASan/UBSan and warnings as errors. Synthetic buffers test both register shapes, zero/one/two
matches across sections, ignored data, truncated buffers, 1/3/5 maxima, unsupported DWORDs,
same-address reload, new addresses and two independent wrapper copies. The latch reads old
sites only inside the new image's executable bounds. Failed attempts are latched, not retried.

Preset tests cover Auto, invalid config, unlock-off, A/B, the C version boundary, unknown versions,
other setting IDs, driver errors, absent keys, wrong struct versions/types, null output, and preserving
predefined metadata on an existing-key answer. The UI availability gate is production code too.

The actual wrapper orchestration and planner are compiled as source slices with explicit fake
modules. Both new DRS/menu adapter headers are compiled unchanged against Config/NVAPI/ImGui fakes:
Auto/A/C session selection, disabled unlock, absent driver/provider, replacement route, C unknown/old/new,
and a menu edit are exercised. Source guards keep load/private-loader/hook/config wiring attached. A generated fixture compiles the
real `CustomOptional`, INI reader and preset read/save statements against SimpleIni and verifies
read/save/read for missing, auto, mixed-case A/B/C, invalid text, menu assignment and reset.

No NVIDIA binary, GPU, game, Wine, Windows API or MSVC build is exercised. These tests do not
qualify actual module unload ordering, machine-code semantics, rendered frames, pacing or presets.

Optional adversarial check: `python3 tests/mfg-wrapper-preset/run.py --mutations` must reject
eight individually compiled regressions by assertion (first-match admission, arbitrary ceiling,
sticky reload, global latch, data-section match, early C, Auto override and unlock-off override).
Compilation errors do not count as rejection; mutants are created only in temporary test directories.
