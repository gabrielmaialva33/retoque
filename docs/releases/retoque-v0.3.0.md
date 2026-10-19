# Retoque v0.3.0 — draft for Gabriel's review

Proposed tag: `retoque-v0.3.0`. This text is a draft, not a published release or a claim that the release
build has passed testing. Publication requires Gabriel's approval.

Retoque brings DLSS Neural Rendering to games that did not ship with it, on top of OptiScaler's upscaler
replacement and frame-generation support. This first versioned package makes installation simpler:
extract the zip, run the Windows or Linux installer, pick the game folder, and choose whether to enable NR.

## What is in this fork

- Neural Rendering after the upscaler on D3D12 and Vulkan, plus present-time paths for games without an
  upscaler and a D3D11-to-D3D12 bridge.
- Game profiles: for the titles where NR needs a particular route (Divinity: Original Sin 2, Generation Zero,
  Sons Of The Forest, Hellraiser: Revival, DOOM Eternal, PCSX2), the keys are applied automatically when NR
  is on, and only where your ini leaves them at `auto`. Under Proton, a prefix without `nvapi64.dll` gets
  Proton's own copy for the session. `[DlssNr] GameProfiles=false` turns this off. Measured in Divinity and
  Sons Of The Forest with no hand-set keys; the menu shows which profile was applied.
- A simple menu by default: on/off, status, model preset, pass count, detail strength and the GPU cost.
  **Show all settings** brings back every control, unchanged.
- Proton fixes for Vulkan overlays and DLSS-G, and NR ordering ahead of the game's frame generation.
  Opening the menu pauses DLSS-G so the menu stays readable; closing it resumes generation.
- Colour composition based on RenoDX, exposure controls, comparison views and GPU timing. Working scale,
  model cadence and other optional controls let you measure the cost of the effect on your hardware.
- Support for the two weight sets in model 310.9.16: Preset 1 selects September, Preset 2 selects October.
  The menu reports which weight set the model actually built.
- Installers that refuse a running game or a conflicting proxy, preserve ReShade when requested, and
  record their changes for uninstall. Linux also copies the driver's Wine NGX loader. Both can enable NR
  without hand-editing the ini.

This combines ideas and fixes from Dagherbou, scottmudge, janblade, wilsjo2, RenoDX and other contributors
with this fork's Proton work and regression tests. It is not a claim that every feature is unique, or that
Retoque is faster than every other fork. See the README credits and the included licenses.

## Start playing

1. Extract `Retoque-v0.3.0.zip` into a separate folder and close the game.
2. Windows: run `install-windows.cmd`. Linux: run `bash install-linux.sh`.
3. Select the executable folder and a proxy name; accept **Turn Neural Rendering on?**
4. Supply your own `nvngx_dlssnr.dll` beside the game's executable. It is not in this download.
   Model 310.9.16 needs the compatible build on RTX 20/30/40; Preset 2 is its October weight set.
5. Proton: set `WINEDLLOVERRIDES="dxgi=n,b" %command%` for the default proxy, or substitute your proxy name.

Home opens the menu with the release ini. Existing settings retain their menu key. F7 is available only
if you configure `[DlssNr] ToggleKey=0x76`. Preview and uninstall commands are in the README.

## Limits to keep in mind

Experimental and unsupported by NVIDIA. The model is user-supplied and hardware compatibility depends
on its build. No universal FPS or image-quality improvement is promised. Blackwell compatibility remains
under investigation; this release does not claim to fix Xid 69. Games outside the profile list may still
need route keys in the ini (see the README).

Before publication: build the final Windows package in CI; test the extracted installers on native Windows
and Linux; verify the final archive and game smoke tests; review these notes and attach the final checksum.
