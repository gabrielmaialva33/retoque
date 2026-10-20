# Looks: one-click appearance bundles

Design before implementation, 2026-10-20. Idea from janblade's F5 fork (v0.1.31, "one-click looks",
`DlssNr_LookPresets.h`); the values and the code here are our own.

## What it is

A **Look** selector, in both the simple and the full view, that writes a named set of appearance values into
the existing keys in one click. It is an action, not a setting:

- **No new key.** What the ini records is the values it wrote, and every individual control still shows and
  edits them.
- **Read back from the current values.** When they match a look, the selector names it; otherwise it reads
  "Custom".

Looks touch appearance only:

- the model's Style, Intensity, Local structure, Local tone, Skin structure and Auto skin mask (the master
  settings, written through `SetDlssNrMasterSetting` like the sliders);
- Colour strength;
- Detail strength (`TransferStrength`).

They never touch resolution, passes, cadence, exposure, white point or the composition mode, which are
performance or correctness choices, not looks.

## The looks, and where each value comes from

| Look | Values (everything not listed is the model default) | Source |
|---|---|---|
| Default | Intensity 1, Style A, structure 1, tone 1, skin -1, mask on, colour 1, detail 1 | the model's and this fork's defaults |
| Natural | Style B | NVIDIA's own style B |
| Cinematic | Style C | NVIDIA's own style C |
| Subtle | structure 0.7, tone 0.3, mask off | what NVIDIA's RTX Remix integration ships (already cited in the shipped ini) |
| Gentle | intensity 0.5, structure 0.7, tone 0.3, colour 0.5, detail 0.85 | the conservative community recipe gathered for Cyberpunk on 2026-10-19 (Igor'sLAB, Caseking, Dagherbou discussion #17), confirmed against our code by Codex |
| Game colour | colour 0 | keep the game's own hue; only light changes (Caseking) |

## Limits

- **Individual pass settings.** With these on, per-pass overrides stay and win for their pass. The help says
  so.
- **Matching.** It compares within 1e-4, so a value typed in the ini that rounds differently reads as Custom.

## Validation

- **Host.** The table is well formed, every value is inside its control's range, matching finds each look and
  rejects near-misses, and the menu applies master settings through the locked setter.
- **No GPU claim.** These are named starting points, not measured improvements.
