# Unreal Engine DLSS search

`python3 tests/unreal-dlss-search/run.py` compiles `retoque/misc/UnrealDlssSearch.h` under ASan/UBSan and runs it on
real temporary folder trees, then slices dllmain's block that uses it (after the existing `nvngx_dlss*.dll`
searches) and runs that against a fake `State`.

Design and fork comparison:
[docs/linux-proton/unreal-dlss-search-20261030.md](../../docs/linux-proton/unreal-dlss-search-20261030.md).

Cases: engine DLSS/StreamlineCore plugins from `<Root>/<Project>/Binaries/Win64` (with and without the trailing
separator `remove_filename()` leaves); Phantom Blade Zero's `<Root>/platform/steam`; the exe's project plugin before
the engine plugin, the fixed container order (`Plugins/Runtime/Nvidia`, `Plugins/Nvidia`, `Plugins/Runtime`,
`Plugins/Marketplace`, `Plugins`) and the NGX third-party folder; `DLSS*`/`Streamline*` plugin names sorted, other
names ignored; nothing below or beside the fixed folders, and a folder named like the file is not a match; the
ascent stops at four folders above the exe's, at the nearest `Engine` and at the filesystem root; a file named
`Engine` is no root; the root's single project is used for an exe outside every project, two projects are not
guessed between; an unreadable folder is skipped without throwing. Wiring: a path the existing searches found is
kept unchanged, missing ones are filled with the file itself and logged once each, and a non-Unreal layout fills
nothing.

Before the change (2026-10-30) the suite failed to compile (no header), and against the unmodified `dllmain.cpp`
its wiring check reported no Unreal search; with the `has_value()` guard removed the wiring run fails "a file the
existing searches found is kept unchanged". Host-only: no game, Wine or Windows loader was run.
