# Streamline tag inventory: what the game tags, and whether a HUD-aware present could use it

Date: 2026-11-03. Status: implemented, host-tested, log only; not run in a game.
Source: the 2026-11-01 present-route audit (opportunity 3, step A).

## Why

The present pass at the game's Streamline boundary edits the HUD with the rest of the frame
([nr-before-dlssg.md](nr-before-dlssg.md), H1). Its designed successors (H2/H3 there, mask (c) in
[hud-protection.md](hud-protection.md)) would protect the UI with the game's own buffers: the HUD-less colour
(Streamline buffer 2) or the UI colour with alpha (23). That needs a title that tags one of them, with a lifecycle
that keeps it valid until Present (`sl::eValidUntilPresent`). With `eOnlyValidNow` or `eValidUntilEvaluate`,
Streamline may copy the buffer and the game may reuse the texture before Present. Which titles qualify was not
known, so this note adds the measurement only.

## What it records

`retoque/dlssnr/DlssNr_SlTagInventory.h` (standard library only) keeps, for each buffer type the game tags with a
resource:

- how many tags arrived;
- which lifecycles were used, as a mask;
- where the tag was seen: `slSetTag`, `slSetTagForFrame`, or a `ResourceTag` among `slEvaluateFeature`'s inputs;
- the last shape: width, height, native format (from `ID3D12Resource::GetDesc` on D3D12, otherwise the
  `sl::Resource` fields or the tag's extent), and whether the tag's extent is only part of the resource. A buffer
  is said as "buffer", without a size: its width is bytes, not pixels.

Tags without a resource (a removal) are counted separately. Types past 95 are counted together.

Once per change, at most every two seconds, one line:

```
DLSS-NR: Streamline tags from the game: depth (until Present, per frame, 1720x720, format 40); motion (...);
HUD-less colour (until Present, per frame, 3440x1440, format 10) | for a HUD-aware present: HUD-less colour
valid until Present, UI colour+alpha not tagged (log only)
```

The verdict for each of the two buffers is one of: not tagged; valid until Present; not valid at Present; or
valid until Present only sometimes (both kinds of lifecycle seen).

## Where it is fed and said

- **Fed** by the Streamline hooks: upstream's `hkslSetTag`/`hkslSetTagForFrame` when a Streamline FG input
  installed them, and the evaluate hook, which is always installed. The hooks only note: atomics, no lock, no
  log, no allocation and no config read in the hook.
- **No Streamline FG input** (the usual case for a game running its own DLSS-G, such as Cyberpunk 2077 with
  `FGInput=auto`): upstream does not install the tag hooks. With NR on, `hkslSetTagNoteOnly` and
  `hkslSetTagForFrameNoteOnly` are installed in their place. They note and forward the call untouched: none of
  upstream's tag handling (the FG input report, the Cyberpunk HUD-less state quirk, the skip of DLSS-only tag
  sets) runs. They also feed `DlssNr_SlNativeNr.h`'s tag observation, which upstream's hooks fed only with a
  Streamline FG input. `unhookInterposer` detaches exactly the detours that were attached, whichever tag hooks
  those were ([sl-interposer-unhook.md](../../linux-proton/sl-interposer-unhook.md), which since 2026-11-03 also
  fixes the upstream defect of detaching exports that were never attached).
- **Armed** at hook time when `[DlssNr] Enabled` is true, and fixed for the session.
  - Started with NR off: no new detour, nothing noted, no line, as before.
  - Started with NR on and NR turned off in the menu: the hooks keep noting until the process ends (a few
    relaxed atomics, and on D3D12 one `GetDesc` per tag). No line is said, because the routes that say it do
    not run.
  - Started with NR off and NR turned on later: nothing is armed until the next start.
- **Said** by the D3D12 status line (`ReportRuntimeStatus`, after its two-second gate) and by the Vulkan
  after-upscale route next to the game-NR ask. Both say it only with NR on, and only when the inventory changed.

## Limits

- It says what the game declared, not what the buffers contain. A tagged HUD-less buffer can still hold the
  HUD, or be the wrong frame; H2/H3 need their own content and frame-token checks.
- Readability at the boundary is inferred from the lifecycle only. Resource state and queue ownership at the
  game's Present are not checked here.
- The Cyberpunk HUD-less state quirk does not run on the note-only path. That is by design: the quirk adjusts
  the tag for OptiScaler's own FG input, and that input is not in use there.
- D3D11 titles are noted the same way. Their `GetDesc` path is D3D12 only, so their shape comes from the tag.

## Tests

`tests/nr-sl-tag-inventory`:

- the header under ASan/UBSan, with 16 rule mutants;
- source guards with 11 mutants on:
  - arming and installing only with NR on;
  - the FG-input gate's whole expression;
  - note-only hooks that do nothing but note and forward;
  - `GetDesc` only on D3D12 textures and buffers, with buffers said as buffers;
  - the unhook detaching only what was attached (the record is tested by `tests/sl-interposer-unhook`);
  - the line and its signature taken from one read;
  - where the line is said.
