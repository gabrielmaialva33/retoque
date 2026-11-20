# Legacy APIs and 32-bit games: design under review

Date: 2026-11-20. **Renderer proposal only; no new supported route or runtime qualification.**

The separately invoked [metadata CLI diagnostic](../../../tools/app/LEGACY-PROBE.md) now implements slice 0.
The adapter/helper options below remain proposals; the original documentation-only verification boundary at
the end of section 5 describes this design's initial delivery, not the later CLI implementation.
This note recommends an observation-only slice before committing to a legacy renderer. It does not authorize
an x86 DLL, a helper, new hooks, installation in games or changes to defaults. API and executable bitness are
independent: D3D10 can be x64, and an x86 game can use a modern API.

The shortest plausible Windows route is to qualify the existing external window host. The shortest plausible
Proton D3D10 route is to establish whether the existing Vulkan present host actually sees DXVK's presentation.
Neither premise is a current compatibility claim. A native D3D10 adapter, OpenGL interop and an x86 transport
are substantially different projects; do not approve them as one parity checkbox.

## 1. Source boundary and the NeuRotic reference

Retoque baseline: `675722dc`, clean before this documentation change. NeuRotic Alpha 0.9.9 reference:
`beae4eeaab0032b665f7a52684db3aa7345bef69`, clean at inspection, read only. `R:` paths are relative to this
repository; `N:` paths are relative to that reference clone. Line anchors refer to those snapshots. The idea
of legacy API connections feeding an x64 renderer comes from NeuRotic. No source, data tables, translations,
shaders or model data were copied. This is a source audit, not a build or execution of that fork. External sources below were consulted on
2026-11-20; primary documentation establishes API contracts, not compatibility on either target machine.

### What is wired

There are **two transports**, not one universal shared-memory helper. The top-level add-on README's statement
that legacy APIs/every x86 caller use CPU transport does not describe all the newer wiring in the same commit.
The code attempts the GPU connections first. Treat the README's qualification disclaimer as a boundary, and
read the call sites for transport selection (`N:addons/prepared-guides/README.md:64-79`;
`N:addons/prepared-guides/src/prepared_guides_addon.cpp:582-719`).

| Observed component | Code evidence and actual boundary |
|---|---|
| Acquisition and activation | ReShade add-on callbacks observe the API, selected depth and effect-produced motion; process opt-in `NEUROTIC_PREPARED_GUIDES=1` plus core eligibility gates processing. `Begin`/`Technique` reach `Feed`, which reaches the API connection. This is not an autonomous D3D10/OpenGL proxy. `N:addons/prepared-guides/src/prepared_guides_addon.cpp:153-164,741-790,823-861,1010-1040` |
| Build inclusion | Both architectures include the D3D10, D3D11, D3D12 and GL connection sources and GPU client; the x64 build adds the local processor/flow. The scripts are source evidence, not a build reproduced here. `N:addons/prepared-guides/build.cmd:7-40` |
| D3D10 capture and return | Same-device, leased SDR single-sample color/depth are checked, then copied to D3D10 shared textures. A same-adapter D3D11 relay opens their legacy shared handles; event queries/Flush establish completion. `D3D11Connection` then normalizes into D3D12. Returned output is copied through the relay back into the original D3D10 target. `N:OptiScaler/dlssnr/connections/D3D10Connection.cpp:16-25,31-73`; `D3D10EventRelay.h:150-199,233-344`; `D3D11Connection.cpp:120-162` |
| D3D10 synchronization cost | Capture waits for the game query, and Finish waits for relay completion and then the final game copy. These are real blocking paths, in addition to D3D11/D3D12 completion waits. The relay header's historical two-drain explanation understates this caller's final confirmation. A timeout is not accepted as completion. `N:OptiScaler/dlssnr/connections/D3D10Connection.cpp:57-72`; `D3D10EventRelay.h:292-344` |
| OpenGL capture and return | Requires a current context lease, the four EXT memory/semaphore extensions, an exact-format private GL→D3D12→GL pixel probe and matching adapter/node when identity is available. Blits color/depth and optional motion/distrust into imported D3D12 resources, signals imported fences, uses `D3D12Connection`, reacquires imports and blits back. Bottom-left rows and motion Y are handled explicitly. No whole-frame CPU transport on this branch; the admission probe does read pixels. `N:OptiScaler/dlssnr/connections/GlConnection.cpp:35-79,81-145,177-227`; `GlExternalObjects.h:538,595` |
| x64 GPU consumer | `RunConnection` calls `textureProcessor.ProcessTextures` in the game process, then `connection.Finish`. No helper process is required for that selected route. `N:addons/prepared-guides/src/prepared_guides_addon.cpp:480-580` |
| x86 GPU consumer | The same caller instead creates `GpuRemoteConsumer` pointing to a managed sibling `NeuRotic.GpuHost/NeuRotic.PreparedGpuHost.exe`. The x86 process owns capture and a local D3D12 transport device; the model runs in the x64 child. Output is returned to the source API and its completed copyback acknowledged before releasing the helper lease. `N:addons/prepared-guides/src/prepared_guides_addon.cpp:539-568,608-614`; `N:addons/prepared-guides/helper/GpuRemoteConsumer.cpp:52-75` |
| Production helper binding | The GPU helper validates its managed sibling `OptiScaler.dll`, binds prepared exports and calls `NativeGuides::Processor::ProcessTextures`. This is a real renderer consumer, not the test shader recorder. `N:addons/prepared-guides/helper/PreparedGpuMain.cpp:17-58` |
| CPU alternative | `FeedHelper` can read back through ReShade, pack planes and upload completed output. With no `NgxPath` it calls the loaded local core via `CpuMainlineClient`; explicit `NgxPath` selects the separate public-NGX/DLAA helper. GPU uncertainty suppresses fallback; derived-flow and owner policy can refuse it too. Do not claim every x86 failure falls back successfully: local-core availability is checked. `N:addons/prepared-guides/src/prepared_guides_addon.cpp:660-739`; `src/CpuMainlineClient.cpp:100-130,210-244`; `helper/PreparedHelperMain.cpp` |

The last two rows' `src/` and `helper/` paths remain under `N:addons/prepared-guides/`.
D3D10 sharing here is not a general guarantee about every D3D10 driver or typeless depth format: actual creation
is probed and may fail. GL likewise refuses multisampling, unsuitable formats, sRGB attachments without an
encoded-copy policy and unavailable context ownership. Merely having an OpenGL driver is insufficient.

### Exactly what crosses the process boundary

**CPU v1:** inherited stdin/stdout pipes carry pointer-free requests and packed image bytes: SDR color,
device depth, pixel motion and optional distrust. The reply carries completed color pixels and identity/completion
metadata. The helper creates private GPU devices, uploads, evaluates NR, copies to staging, observes a D3D11 event
query and maps before replying. The protocol caps payloads at 160 MiB; the documented request/reply layouts are
288/136 bytes, excluding payload (`N:addons/prepared-guides/helper/PreparedHelperProtocol.h`,
`PreparedHelperClient.h:63-110`, `PreparedHelperMain.cpp:34-62,150-180` under the same helper directory). No game pointer is
meaningful in the child. `CpuMainlineClient`, despite its name, also has GPU-resident methods and is not itself IPC.

**GPU NRH2:** a private inherited bootstrap handle starts the x64 child; a current-user-only, remote-client-rejecting
named pipe carries registrations, fixed-width descriptors, capture/generation tickets, fence values and releases.
PID/creation time and a random session nonce are checked. Texture/fence NT handles are duplicated into the peer
handle table before import, not passed as raw COM pointers. Pixel storage is shared GPU memory, not a CPU mapped
frame buffer. The bounded outer packet is at most 4096 bytes; ten textures in two slots and two fences are used
by the current consumer, with at most one outstanding submitted/output-leased frame. Local private→shared and
shared→private copies remain; shared handles do not mean zero copies. See
`N:addons/prepared-guides/helper/PreparedGpuClient.cpp:22-69`, `PreparedGpuProtocol.h:14-38`,
`GpuRemoteConsumer.cpp:29-75`, `PreparedGpuHost.cpp` and `GPU_HELPER.md:10-45`.

A scoped search of `N:addons/prepared-guides/{src,helper}` found no `CreateFileMapping`/`MapViewOfFile` frame
transport. This absence is scoped to these paths, not all NeuRotic subsystems. Both transports synchronize
before accepting output; no measured latency follows from these source observations.

### Implemented, unavailable and unproved are different

- **Implemented connections:** D3D10/GL Capture/Finish, x86→x64 GPU transport and CPU helper evaluation are wired.
  Calling them stubs is inaccurate. ReShade/resource/route admission can still reject all frames in a given game.
- **Actual stub:** `N:addons/prepared-guides/helper/NativeFsrUnavailable.cpp:3-8` makes `LoadFsrApi` return false.
  The helper/add-on does not own the game's presentation scheduler. NR helper support does not supply standalone
  frame generation for these legacy APIs.
- **Explicitly unqualified:** `N:addons/prepared-guides/README.md:13-21,100-105` disclaims game/GPU/presentation
  qualification; `helper/GPU_HELPER.md:52-65` records a software-D3D12 transport test on 2026-10-05, not RTX/private
  model or hardware cross-API qualification. Those are the author's records, not independently reproduced results.
- **Failure policy in code:** invalid/foreign/stale frames are refused; uncertain referenced work quarantines the
  session. Connection destructors retain uncertain resources rather than release them; GL additionally refuses
  cleanup under the wrong current context. The GPU client gates new sessions after uncertainty. The source lease,
  helper lease and final copyback completion are separate. This intent is visible in
  `D3D10Connection.cpp:28-30`, `GlConnection.cpp:174-227`, `GpuRemoteConsumer.cpp:49-75` and the add-on's
  `RunConnection`. It is not a proof that every device loss, peer crash or context destruction is safe.

## 2. Retoque foundations and gaps

| Existing piece at the baseline | Nearest reuse and missing boundary |
|---|---|
| x64 DLL and install guard | `R:tools/app/crates/retoque-core/src/engine.rs:679-695` refuses an **exhaustively enumerated x86-only folder**; mixed folders get a note. `safety.rs:28-34` does not infer x86-only from a partial scan. Removing this guard cannot make the x64 proxy load in x86. There is no legacy producer/helper package to install |
| D3D11→D3D12 swapchain bridge | `R:OptiScaler/with_dx12/dx11_with_dx12_sc.cpp:392-406,1068-1132,2143-2150,2248-2313,2390-2518`: shared textures, fences, NR `PresentHost`, and a D3D12 visible presenter. Nearest Windows D3D10 foundation, but an `ID3D10Device` is not an `ID3D11Device`; capture, event-query relay, ownership and restoration are missing. Do not reuse historical design line numbers as current defect claims |
| D3D12 present host | `R:retoque/shaders/dlssnr/DlssNr_Dx12.cpp` (`PresentHost`, `RunPresentPass`) supplies zero-guide NR, composition and existing settings. It needs a valid owned D3D12 device/queue/target; it is not an arbitrary pointer/IPC renderer export |
| Vulkan present host | `R:retoque/dlssnr/DlssNr_VkPresentHost.cpp:3256-3336,4291-4345` and `DlssNrFeature_Vk.cpp:5019-5440` supply present transport/model/synthesized motion. `DlssNr_VkPresentRegistry.h:49-65` already recognizes `d3d10core.dll` as DXVK provenance. This is not native D3D10 capture, an x86 DLL or a Linux ELF layer. Actual loader reachability, initialization ordering, format/queue admission and single NR ownership remain to be measured |
| External window host | `R:retoque/window_host/Capture.cpp:113-177,187-235` uses WGC in a separate process, keeping the newest frame. `Transport.cpp:49-104` shares three D3D11/D3D12 slots and two fences; `Output.cpp:303-350` copies into its own presenter. Nearest route for all three gaps on Windows: the captured app need not load our DLL. No game depth/motion or return into its swapchain; result is a separate preview/overlay |
| Window DLL seam | `R:retoque/dlssnr/DlssNr_WindowHost.cpp:56-117` exports Version/Begin/Present/End, guarded by `RETOQUE_WINDOW_HOST=1`, one session/owner thread and no wrapped swapchain. Present takes **in-process COM pointers** and invokes `RunPresentPass`; End frees nothing. It is neither cross-process ABI nor a guide-texture request/reply service. A future helper could own a presenter and use it, but headless output extraction would require new, reviewed completion/lifetime contracts |
| Synthesized motion | `R:retoque/shaders/synth_motion/README.md:12-64`, `SynthMotion_Dx12.cpp`, `SynthMotion_Vk.cpp` and `R:retoque/dlssnr/DlssNrFeature_Vk.cpp:5438` supply image-derived current→previous pixel displacement. It is not native engine motion or recovered geometry. Warm-up, skipped captures, cuts, crop/resize and submission-confirmed history still matter. Use neutral depth; no depth estimator is proposed |

The [window host](../../../retoque/window_host/README.md) and [NR Anything design](nr-anything.md) explicitly
remain Windows-only and not runtime-qualified. Their existing app screen does not establish Windows execution.
The [bridge design](nr-dx11-bridge-host.md), [Vulkan host](nr-vulkan-present-host.md) and
[synthesized-motion design](synthesized-motion.md) retain their own historical validation boundaries.

## 3. Platform meaning, costs and options

### What changes under Proton and native Linux

[DXVK](https://github.com/doitsujin/dxvk) implements D3D10 using Vulkan. Therefore a **64-bit Windows D3D10 game
under DXVK** is a candidate for our existing Windows Vulkan present boundary, without a D3D10→D3D11 relay.
That is an inference to test, not support. DXVK's [loader](https://github.com/doitsujin/dxvk/blob/master/src/vulkan/vulkan_loader.cpp)
tries `winevulkan.dll` before `vulkan-1.dll`; Retoque installs through the latter and also resolves driver function
targets (`R:retoque/dlssnr/DlssNr_VkPresentHost.cpp:3616-3655`). Prove interception of actual device/queue/present
calls from the selected DXVK build. Neither module presence nor a recognized caller basename proves coverage.
x86 DXVK still has an x86 Windows process: Vulkan underneath does not let it load an x64 DLL.
[Microsoft's process interoperability rules](https://learn.microsoft.com/en-us/windows/win32/winprog64/process-interoperability)
explain the architecture boundary.

Keep three cases separate. Windows OpenGL games use Wine's OpenGL/WGL bridge, as shown in
[Wine's implementation](https://github.com/wine-mirror/wine/blob/master/dlls/opengl32/unix_wgl.c).
Windows **D3D** games can instead use OpenGL-based wined3d: Valve documents `PROTON_USE_WINED3D` for D3D9/10/11 in
[Proton's runtime options](https://github.com/ValveSoftware/Proton#runtime-config-options).
Native Linux OpenGL applications use native GL, without a Windows DLL loader. Thus “OpenGL games go through
wined3d” conflates API translation with OpenGL dispatch. None of these paths implies a Vulkan present visible to
Retoque. A translation layer such as Zink would require its own observed route and is not an automatic solution.

WGC/DirectComposition on native Windows cannot be assumed to work under Wine. The existing product decision is
Windows-only, not “run the same helper in a second prefix”. Native GL would need a native capture/layer frontend,
Linux GPU-memory and synchronization transport into a Windows NR host (or a separately viable model backend),
and compositor integration. The [Khronos Win32 external-object specification](https://github.com/KhronosGroup/OpenGL-Registry/blob/main/extensions/EXT/EXT_external_objects_win32.txt)
describes D3D12 resource/fence imports; its Win32 handle contract is not a POSIX-FD transport. Driver extension
strings alone do not prove Wine handle translation or same-adapter sharing.

### Common cost and failure assumptions

All sizes below are **engineering estimates for one experienced maintainer**, including focused fixtures and
one supported route, excluding waiting for owner testing, broad game coverage and release support. They are not
commitments. Model evaluation time is additional to transport time, depends on model/scale/GPU, and is never
estimated from an old 4090 result for a 3060. “Copies” below counts full-color transfers outside NR internals;
guides, conversion passes and driver/compositor work can add more.

For scale: `1920*1080*4` is 7.91 MiB per RGBA8 frame. A CPU color-only round trip needs at least two readbacks and
two uploads, about 31.6 MiB/frame or 1.85 GiB/s at 60 Hz, before pipe/memory copies. At 3840×2160 that becomes
126.6 MiB/frame or 7.42 GiB/s. These are arithmetic traffic floors, not measured bus throughput or latency.
GPU sharing avoids those CPU crossings; it does not erase copies, waits, synchronization bugs or model cost.
Synchronous processing adds its elapsed critical path to Present; pipelining can trade stalls for one or more
frames of image age. Never silently put an old processed frame into the current game frame.

For every future invasive route: refuse before touching a source if prerequisites are missing; retain the ordinary
present path. Once GPU work references resources, a timeout/device loss/peer death must poison that generation,
stop new work and preserve resources until real completion or process exit. Never signal another owner's fence
on the CPU to simulate success, free on elapsed frames, or auto-relaunch helpers into unbounded quarantined VRAM.
Resize/context/adapter changes require fresh identity and proven retirement; after output publication, restore
only what can actually be restored. The following options inherit those requirements.

### 32-bit executables (across APIs)

| Option | Model, frame path and latency | Platform, conflicts and failure | Rough size / user cost |
|---|---|---|---|
| A. Qualify external capture first | x64 `RetoqueWindowHost`; WGC frame→shared D3D11/12 slot→host backbuffer→NR→separate overlay. At least two explicit color copies before NR, plus OS capture/composition. No return to game; capture age plus model/presentation delay must be measured | Native Windows, windowed/borderless SDR only initially. No injected x86 component; capture may be blocked and anti-cheat acceptance is unknown. Recorder/HUD/other-overlay/focus conflicts still exist. Hide on stale/invalid geometry; stop/abandon on uncertain GPU work. Existing Windows-only boundary stays | 1–2 weeks of qualification/targeted fixes after slice 0, potentially longer on native faults. User accepts external output and its latency, no native guides or exclusive fullscreen guarantee |
| B. x86 producer + x64 GPU helper | x86 capture/normalization→shared GPU resources/fences→x64 NR→shared result→exact original source. At least ingress/egress copies plus each API adapter's copies, conversion and NR. Same-frame blocking or explicit bounded async age policy | New x86 ABI/build/install/receipt, authenticated local protocol, same-adapter proofs and independent helper lifetime. Each API needs a producer; “x86 support” alone reaches no frame. DLL/ReShade injection conflicts with anti-cheat and existing proxy owners. Windows first; cross-process Wine sharing and loader paths require separate fixtures. Helper crash never grants completion | 6–10 weeks for one producer/Windows route; additional APIs/Proton extra. Higher maintenance and packaging burden, but retains in-game output if qualified |
| C. x86 CPU transport fallback | x64 helper receives packed completed pixels over a local pipe (or explicitly designed bounded shared CPU memory), uploads/evaluates/readbacks; x86 uploads exact returned frame. Four GPU/CPU crossings, multiple CPU copies; no promised 60 Hz | More portable capture formats than GPU sharing, still requires a source-API producer and real completion. Same injection conflicts as B; Wine IPC is not evidence of functioning readback/model/copyback. Timeout refuses output, quarantines referenced staging work; no automatic costly fallback | 4–6 weeks for one API prototype, not recommended as default. Might be useful for low-resolution still/slow games; substantial frame-time cost |
| D. Do not add x86 support | No new model execution, copies, helper or delay | Existing installer refusal remains; ordinary game unaffected. No new anti-cheat/overlay or teardown surface | No new implementation. Users of x86-only builds remain without in-game NR; an officially available x64 edition is an alternative only after checking its actual renderer. External capture remains separately unqualified |

Porting the whole runtime/model to x86 is not a fifth cheap option. There is no qualified x86 model/backend in
this tree; an x86 proxy cannot call the supplied x64 model in process. Keep the current installation refusal even
if external capture later qualifies: that route installs nothing into the game.

### D3D10 / D3D10.1

| Option | Model, frame path and latency | Platform, conflicts and failure | Rough size / user cost |
|---|---|---|---|
| A. Existing Vulkan host through DXVK | x64 in-process Vulkan NR; source swapchain→host work image→same swapchain, at least two color copies plus NR. Existing semaphore/queue ordering; no extra process or legacy relay | Proton first, x64 only with current DLL. Must load early and observe actual loader/device/swapchain; coexistence with DXVK HUD/Steam/ReShade/FG needs proof. Do not activate both bridge and Vulkan NR. Before submission refusal leaves original waits; after consuming waits, follow established transport completion, never naïvely call original Present | 1–2 weeks to establish reachability and transport controls, then 1–3 weeks game/quality qualification if no new loader work. Does not solve native Windows D3D10 or x86 |
| B. D3D10→D3D11→D3D12 adapter | x64 in process; copy into same-adapter legacy shared texture, D3D11 alias/relay, D3D12 NR, then reverse copies to original target. Budget at least four interop color copies plus normalization; event queries introduce serial bubbles. x86 adds option B above | Native Windows first; existing D3D11 bridge supplies patterns, not a cast-compatible API. Need DXGI/D3D10 present integration, state/format/MSAA policies, source ownership and resize/device-loss handling. ReShade/proxy injection conflicts. Under Proton prefer A; do not assume DXVK legacy sharing and queries match native Windows | 5–8 weeks for color-only x64 Windows; native depth/motion selection and x86 extra. Could preserve exclusive/in-game presentation, but not promised before proof |
| C. External capture | Same x64 host and two-copy minimum as 32-bit A; original D3D10 renderer untouched; separate output | Same Windows capture, anti-cheat, overlay and teardown limits as 32-bit A. Works around API only if WGC captures that game's chosen mode | Same shared 1–2 week qualification effort, not additive per API. Loses native guides and adds capture/compositor age |
| D. Do not add a D3D10 route | No new model/copies/latency; keep ordinary presentation | No new conflicts or lifetime work; existing Vulkan host remains only a candidate on translated x64 titles | Native D3D10 users remain excluded from in-process NR. Changing a game's renderer is an individual compatibility decision, not a supported universal workaround |

### OpenGL

| Option | Model, frame path and latency | Platform, conflicts and failure | Rough size / user cost |
|---|---|---|---|
| A. External capture | x64 host; WGC→shared slot→host backbuffer→NR→overlay; same two-copy floor and capture-age cost | Windows OpenGL including x86 is a candidate without game injection. Same protected-content, overlay, focus, fullscreen and failure rules as 32-bit A. No native Linux/Proton promise | Shared 1–2 week qualification effort; simplest Windows coverage, but no engine guides and separate output |
| B. GL/D3D12 GPU interop producer | x64 in-process renderer for x64 Windows GL; x64 helper for x86. Blit/copy source into imported shared texture, D3D12 normalization/NR, return imported output and blit to exact framebuffer: at least two color transfers, often four with private normalization; fences plus GL completion. No numeric latency promised | Requires WGL context/thread/generation ownership, adapter and exact-format probes, state restoration, row origin, sRGB/MSAA policies. Hook/ReShade conflicts and anti-cheat exposure. Missing extensions or a lost context refuses; uncertain imports retained. Wine translation of Win32 handles/extensions is a separate go/no-go gate | 6–10 weeks for one x64 Windows SDR route; x86 helper and Proton extra. Reaches original presentation if qualified; oldest GL contexts/drivers may still refuse |
| C. CPU capture/helper | GL readback (possibly PBO plus actual completion)→x64 helper→upload→GL output. Four GPU/CPU crossings minimum plus IPC; async readback does not remove completion or returned-frame age | Windows or a separately authored native frontend. Preserve pixel-store/PBO/FBO state, thread/context identity and ownership; Wine/native Linux bridge is additional work. Injection and other overlays remain conflicts. Drop/refuse stale results; quarantine pending PBO/texture work on context loss | 4–7 weeks Windows prototype; native Linux end-to-end 8–12+ weeks including helper/compositor/backend investigation. Slow path, useful only if low-resolution use is worth the cost |
| D. Native Linux frontend or GL→Vulkan experiment | Model would run in a Wine x64 helper unless a different viable backend is established. Native GL/compositor capture→FD/shared memory adapter→helper→native presenter, or a measured GL→Vulkan layer path. Copies/latency unresolved until interoperability is proven | Retoque is a Windows DLL, not a native GL/Vulkan layer. Wayland permissions, compositor output, buffer formats/modifiers and cross-runtime synchronization are new ownership boundaries. No guarantee that Zink's Vulkan calls enter Windows hooks. Capture avoids injection but does not guarantee anti-cheat/recording compatibility; teardown spans compositor, native process and Wine child | 8–12+ weeks exploratory, with a stop gate before model integration. A separate product surface; defer unless native Linux is the owner's priority |
| E. Do not add OpenGL support | No new model, copy or latency; ordinary rendering | No new hook/overlay/teardown conflicts | Windows users await external capture qualification; native Linux users remain without a Retoque route. Honest lower maintenance cost, clear coverage gap |

## 4. Real candidate games, with evidence for API and bitness

These are candidates for benefit through **new reach**, not measured improvements, owner-library inventory or
current Retoque compatibility. A shipping binary/version must still be checked before choosing a fixture.
No library scan or game launch was needed for the following bounded examples.

| Exact candidate | Evidence | Why it helps decide |
|---|---|---|
| Original DOOM 3, Win32 build represented by id Software's GPL release (not later editions/source ports) | Official source at `a9c49da5afb18201d31e3f0a429a037e56ce2b9a`: [Win32 solution](https://github.com/id-Software/DOOM-3/blob/a9c49da5afb18201d31e3f0a429a037e56ce2b9a/neo/doom.sln#L23-L31), [engine target](https://github.com/id-Software/DOOM-3/blob/a9c49da5afb18201d31e3f0a429a037e56ce2b9a/neo/doomdll.vcxproj) and [OpenGL/WGL renderer](https://github.com/id-Software/DOOM-3/blob/a9c49da5afb18201d31e3f0a429a037e56ce2b9a/neo/sys/win32/win_glimp.cpp#L24-L36) establish 32-bit Windows/OpenGL for that build | One offline candidate intersects both x86 and GL gaps; dark scenes/HUD would expose unwanted NR edits. First Windows capture candidate if the owner wants it |
| Quake III Arena, original Win32 GPL build (not arbitrary modern ports) | Official source at `dbe4ddb10315479fc00086f08e25d968b4b43c49`: [Win32/x86 target](https://github.com/id-Software/Quake-III-Arena/blob/dbe4ddb10315479fc00086f08e25d968b4b43c49/code/quake3.vcproj#L9-L51) and [OpenGL renderer](https://github.com/id-Software/Quake-III-Arena/blob/dbe4ddb10315479fc00086f08e25d968b4b43c49/code/win32/win_glimp.c#L19-L32) | Another x86/GL candidate, useful offline for fast pan/crosshair and latency rejection. Competitive online use is not a proposed qualification target |
| Original Crysis, Windows D3D10 path; choose and verify the actual executable | NVIDIA's [177.92 release notes](https://www.nvidia.com/docs/IO/58600/177.92_WinVista_GeForce_Release_Notes.pdf), printed pp. 12–14 (PDF pp. 15–17), identify D3D10 and explicitly the 64-bit game alongside the D3D10 alternative. Valve's [original app support listing](https://help.steampowered.com/it/wizard/HelpWithGameTechnicalIssue/?appid=17300) describes a 32-bit distribution. These are edition/distribution evidence, not proof of the owner's installed build; the OS being 64-bit alone would not suffice | Candidate for x64 D3D10→DXVK reachability or Windows capture; a verified x86 edition instead exercises the helper/capture decision. Do not infer Remastered's renderer or a Steam installation's current bitness from these records |

For Crysis, API and bitness are supported as available historical variants; the exact installed pairing remains a
slice-0 fact to collect, not an assertion that the 2008 PDF names every current executable. If the owner cannot
supply a desired D3D10 edition with verified PE/renderer facts, do not spend weeks on the relay merely to fill a row.

## 5. Recommendation and a non-visible slice 0

**Propose observation first, then qualify Windows external capture before authoring an x86 helper or legacy
adapter.** It can address the Windows user's three gaps with existing model/composition code. Keep Proton work
separate: first test the reachability hypothesis for x64 D3D10/DXVK; only then consider using the existing Vulkan
host. Defer native Linux GL and synchronous CPU frame transport unless the owner selects a concrete unmet use.
This is not authorization for any of those implementations or tests in real games.

### Slice 0: metadata only, default off (estimated 3–5 working days)

A separately invoked read-only diagnostic, no main-view control, reads one explicitly selected executable/process
and emits a bounded local report. It must not load OptiScaler or the model into the target, inject a DLL, install
hooks, change the game/prefix/config, start a helper, create a GPU device, allocate/copy/read back textures, capture
pixels, create a preview/overlay, or change presentation. Without explicit invocation it does nothing. Prefer a
CLI-only option over a new persisted INI setting. Any later persisted key must satisfy the four-point round trip.

Record executable PE/ELF architecture, selected process architecture separately from its launcher, completeness
of inspection, window mode/geometry facts available without intervention, OS/backend/module **observations**,
and whether a matching existing Retoque route has already emitted creation/present evidence. Hash/basename
identities suffice; redact full paths, window titles and addresses. Module presence is only a candidate API;
plugins may load several graphics APIs. Unknown stays unknown. Never inject or turn on `VulkanPresentHost` merely
to obtain these facts: that existing setting can add extensions and transport resources, so it is not log-only.

On Windows, query capture API availability without starting a capture session. The existing
[window-capture probe](../../../tests/window-capture-probe/README.md) is **not** slice 0 as written: it creates
GPU devices/windows, captures and presents frames. Its later execution belongs to separately approved native
qualification. On Linux, inspect only the explicitly selected game's process/executable and already available
module/log facts; do not scan unrelated processes or infer a working Windows capture path from Wine's presence.

### Evidence slice 0 must produce before selecting a renderer project

| Gate | Required artifact / refusal |
|---|---|
| Demand and identity | At least one owner-selected title per requested gap, exact edition/executable hash/bitness, requested OS and rendering mode, desired result (overlay versus in-game). Launcher-only or incomplete evidence is a named refusal, not compatibility |
| Route hypothesis | A matrix of Windows D3D10, Windows GL, x86, x64 D3D10/DXVK, D3D/wined3d and native GL. Each fact is observed/unknown. Already available logs may prove hook callbacks; absent logs leave reachability unproved. No route gets a Supported label |
| Off path and privacy | Host tests with fake process/window/module inventories show zero work without invocation, no image data, no network, no writes to game/config, bounded logs and redacted identifiers. Include unreadable/truncated PE/ELF, x86 launcher+x64 child, stale PID/creation identity, mixed APIs, access denied, unknown mode and inspection budget exhaustion |
| Independent rejection tests | Each bad identity/completeness/default-off rule must fail its own mutant. Source-wiring checks supplement behavioral cases; finding a DLL name never becomes an automatic renderer decision |
| Cost | Report diagnostic CPU time and peak working set over repeated selected-process samples on both environments; proposed cap one snapshot, at most 2 seconds and 32 MiB additional memory, at most 64 KiB output. On budget exhaustion stop with partial/unknown. Zero GPU commands and no render-thread wait are hard requirements; this stage supplies no frame-latency number |
| Decision | If no wanted title remains uncovered, park the project. If Windows capture meets the stated output preference, qualify it next. If an x64 D3D10/DXVK title is selected, propose an isolated transport test. If only native Linux GL/exclusive in-game output satisfies demand, request a separate implementation design and budget |

Later qualification must progress through transport-only deterministic patterns (no model), same-frame copyback
and independent observed display, then NR off/on A/A and blinded A/B in fixed moving scenes. Pin source/dirty state,
DLL/helper/model hashes, OS/driver/DXVK build, bitness, resolution, scale and FG state. Compare original game,
transport-only and NR runs; report CPU p50/p95/p99, GPU copy/model/flow times, full-frame pacing, source age,
dropped frames, CPU/RAM/VRAM and external end-to-end latency. Source-to-Present age alone is not click-to-photon.

Windows/RTX 3060 and Proton/RTX 4090 are separate gates. Exercise resize, alt-tab, minimization, dialogs, source
closure, adapter mismatch, stale capture, helper crash, failed signal, timeout, device loss and context loss;
prove refusal/retention with no unresolved resource reuse. Start offline with other overlays absent, then add
Steam/ReShade/recorder independently. Neither no injection nor a clean test grants anti-cheat permission. Do not
claim exclusion bypass or run protected multiplayer as a diagnostic. Proposed later bar: zero wrong-frame/unsafe
reuse cases, no visible HUD damage, and owner-approved latency/quality budgets before exposing a control.

### Verification boundary

This change only adds the design, roadmap proposal and design-index link. Documentation/privacy checks and the
whole host tier are recorded in the worker delivery report with their exact results. They do not verify these
foreign implementations, API claims, source-license compatibility of a future port, GPU interop, game quality,
Windows capture or performance. No Wine/MSVC/dxc, model invocation, game install/launch, native GPU experiment
or source import was performed for this note. Existing validation stays with the
[capability index](../../capabilities.md) and [route validation matrix](../../maintenance.md#validation-route-matrix).

## 6. Decisions only the owner can make

1. Which exact games/editions should gain NR first, on **Windows / Proton / native Linux**, and in which rendering
   mode? The candidates above are not evidence that they are in the owner's library or a priority.
2. Is **separate window/overlay output** acceptable for those games, or must the result enter the game's own
   swapchain/exclusive fullscreen path? Recommendation: accept Windows overlay first if its measured delay is tolerable.
3. Approve only **default-off metadata slice 0** first, with the stated zero-GPU/no-visible-change boundary, or park
   legacy API work? This note does not itself approve implementation.
4. After evidence, prioritize **Windows capture qualification / x64 D3D10-DXVK coverage / one x86 GPU producer /
   native Linux GL**? Recommendation: the first two, gated separately; no all-API promise.
5. What maximum added end-to-end latency, p95 frame-time cost and RAM/VRAM use is acceptable per chosen title and
   GPU? Is low-resolution CPU fallback worth any ongoing support cost? Recommendation: no automatic CPU fallback.
6. Can the owner provide native Windows test time and accept local frame transfer to an explicitly installed x64
   helper if capture is inadequate? No network or persisted frame images are proposed; neither is authorized here.
7. For external output, should capture include other overlays, and should the processed overlay appear in recordings?
   The existing recorder-exclusion decision remains open; do not silently change its default to qualify legacy games.
