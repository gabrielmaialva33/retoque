# Mask inspection, slice 0 (host contracts)

`python3 tests/nr-mask-inspection/run.py`

`[DlssNr] MaskInspection=log` makes the D3D12 present pass say, in the log only, which protections a recorded frame
was sent with ([design](../../docs/dlssnr/design/character-inspector.md), section 4). This suite holds the three
things slice 0 promises: off is inert, the key round-trips, and what is logged is bounded and says no more than was
recorded.

**The header as it ships** (`retoque/dlssnr/DlssNr_MaskInspection.h`, `cases.cpp`), under ASan/UBSan, through its own
`Scope` with a fake config, clock and logger. The process's `operator new` is replaced by a counter, so the off path
is measured rather than read:

- off (unset, `off`, a malformed value, or `log` with NR disabled): no allocation, the clock never read, no line, and
  the observer byte for byte what it was, over thousands of frames that try to write, restart and publish;
- the first line and what it states: route, frame, generation, working extent, passes, the UI alpha handed or the
  reason it was not, the AutoMask and SkinStructure of each evaluated pass, the model's skin mask named as internal,
  and each composition branch as applying or not, with the wording that these are recorded facts and not pixels;
- an unchanged status said once; changes coalesced to one line per two seconds carrying the latest frame; a change
  that went back unsaid; a clock that ran backwards; a status changing on every frame for 100 s;
- every other D3D12 route unavailable, once, stating nothing the pass wrote; the driver proxy;
- a declined or failed frame never described by an earlier one, a single decline between recordings not said, a
  lasting one said; a half-written snapshot stating none of its half;
- switching off with a change pending, and on again: nothing said while off, a new generation after;
- generations on resize, model rebuild, pass count, route and a restart, and on nothing else;
- model cadence's carried frames as the same status; the composition branches against the resolve's own order
  (strength zero, `ApplyModel`, debug views, replace modes, tone-mapped frames, each optional limit);
- the longest possible line, text that does not fit, the 256-line budget and silence after it; unsafe metadata
  skipped as if never seen; a logger that throws.

**The key** (`config_cases.cpp`): the production `CustomOptional`, `Config::readString` and the key's own declaration,
read statement and save statement are spliced out of `Config.h` and `Config.cpp` and run against the real SimpleIni:
unset, `auto`, `off`, `log` in any case, nine malformed values, save and reload with unrelated keys kept, NR disabled
and session-only values never written as the user's choice, and the shipped `OptiScaler.ini` resolving to off.

**The statements in `Dispatch`** (`wiring_cases.cpp`): the gate and the four guarded blocks are spliced out of
`DlssNr_Dx12.cpp` exactly as written and run, in `Dispatch`'s order, against the production `DlssNrFrameInfo`,
`DlssNrConstants` and `DlssNrPassSnapshot`: off untouched with allocations counted, every early return said as
"nothing recorded", the reset flag read but left to the pass, and each route by name. What surrounds them in that
file is a script, not the pass.

**Source guards**: the gate sits after the lock and the config and before any return and before the advisor's scope;
every other use of the inspector is under `if (maskInspection ...)`, writes only into the snapshot, and names no
command list, device, resource, log, allocation or `return`; one constant-initialized observer; no other reader of
the key and no menu control; Vulkan says "unavailable" once per process behind the same request, from the one helper its three routes share, so
no route body changes; the header
includes nothing that can lock, wait, open a file or grow; the resolve's branch conditions in `dlssnr.hlsl` are still
the ones `Compose` mirrors; the key's four points. That last-but-one guard pins shader text on purpose: when a branch
of the resolve changes, `Compose` and this suite change with it.

Ninety-eight seeded mutations must be caught: 52 of the header's rules, 13 of the statements in `Dispatch`, 6 of the
key's round trip and 27 of the source guards. When present, `clang++` (C++23) builds and runs the cases and MinGW
parses them for a Windows target; both are reported separately.

## What this does not prove

No D3D12 object, command list, model, shader, Wine or game is involved, and the DLL is not built here. It does not
show that the line appears in a real `OptiScaler.log`, that a frame the line calls recorded reached the screen, that
the UI mask covers the interface (the suite never sees a mask), that MSVC compiles the unit, or what the inspection
costs with the key on. `Dispatch` itself is not executed: only the statements added to it are. Those remain for the
integration build and a game run on the D3D12 present route.
