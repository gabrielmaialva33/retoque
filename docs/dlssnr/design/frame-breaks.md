# Frame breaks: skipped frames between NR frames, and resets that are not cuts

Status: investigation 2026-11-22 (F6 and F7 of the flicker research), plus a counter, a log and one opt-in key.
`[DlssNr] SkipStandDown` (default **off**, D3D12 only) is host-tested (`tests/nr-frame-breaks`); **not built into a DLL, not
run on a GPU or in a game.** Everything below says which statements are read from the code and which would need a game.

## F6: what a frame shows when the pass skips it

**Confirmed by reading the code.** `DlssNr_Dx12::Dispatch` has about forty `reportSkip(...)` returns. Every one of them
leaves the game's own frame in place: the pass has recorded nothing, and nothing in this fork records a copy of the last
answer into a frame it cannot track. So a skip on one frame between frames the pass ran on is a raw frame between NR frames,
which is a flicker of its own (the plain picture against the model's, every other frame).

Which skips can be steady rather than one-off (read, not measured):

| Skip | Where | Steady? |
|---|---|---|
| Admission refusals of a tracked recording: more than three earlier recordings still on the GPU, unfinished work on two queues, the list already holds an unsubmitted recording, a recording waiting for its first submission, the GPU eight recordings behind, "one queue not yet established" | `TrackNrRecording` -> `Submission::Admit` -> `reportSkip(g_chainStatus)` | Yes, whenever the GPU is the bottleneck. Tracked recording is what `Passes > 1` turns on, so the three-pass case is the one that meets them. More passes mean more NR work in flight per game frame. |
| "chain already evaluated at this observed present" | `g_chainSchedule.Evaluate` | By design: the second call of one present. Not a break (see below). |
| Guide resample unavailable, peripheral pack failed, command-list state not restorable | after the early checks | Rare; most latch the feature off. |
| A pass of the chain waiting for its creation, or failed | the chain loop | Not a skip: the frame shows the last completed pass's answer (a partial chain), permanently once a pass failed ("retry latched"), for a while while one is created. A different flicker, a steady one, documented in `chain-in-flight.md`. |

Not a fix that was available: *holding the last answer* on a refused frame needs GPU work (a copy) on a command list the
admission rule has just said it may not track, which is exactly what a refusal protects against. So no frame is invented.

### What was added

- `retoque/dlssnr/DlssNr_FrameBreaks.h`, `SkipTracker`: every present's outcome (the calls of one present merged: a second
  view or the chain's second call is not a break) goes into a 32-outcome window. A **break** is a run of skipped frames between
  two frames the pass ran on. Settling at the start or a stop is not a break.
- The log says, at most every 10 s and with the last reason, `frames between frames the pass ran on were shown as the game drew
  them (N breaks in the last 32 evaluations, last reason: ...)`. That is the measurement that was missing.
- `[DlssNr] SkipStandDown` (menu: "Hold NR off when it alternates", D3D12 only): with it on, three breaks inside the window hold
  the pass off for one second, with a status, and the model's history is reset when it resumes. The picture is one thing for
  a second instead of two things every frame. It is a trade, not a cure: the cause (a GPU more than three recordings behind)
  is still there, and the pass will run again after the second and may alternate again. Off, nothing changes.

Not verified: whether one second of plain frames reads as better than alternation in a game (a judgement), and how often the
admission refusals really happen in a three-pass session: the new log line is how to find out.

## F7: spurious resets

Read in the code; none needed a change except the missing counter.

| Suspect | Finding |
|---|---|
| `DXGI_PRESENT_TEST` counted as a present (SirenBrink `68a5345e`) | **Not present.** `wrapped_swapchain.cpp` computes `willPresent = (Flags & DXGI_PRESENT_TEST) == 0` and counts `frameCount` only inside it; the D3D11-on-D3D12 swapchain returns for a test present before its `frameCount++`. |
| A present counter that advances during an evaluate | The count is advanced in the wrapped `Present`, after the pass ran; the pass reads `State::frameCount` once per call. No pair is rejected on it: this fork has no seam clock that pairs frames by it. The cadence ages by "presents the pass ran at" and treats a second call in one present as another view. |
| Generated frames counted as presents (janblade #306) | `frameCount` is advanced by the wrapped swapchain (`isCurrent`) and by the D3D11 bridge's swapchain for the game's own presents; a frame generator's presents through its own swapchain are not counted there. A DLSS-G that presents through the wrapped swapchain would advance it for generated frames too. **Not verified without a game**; the consumers are robust to it: the chain's `Evaluate` needs only an increase, the cadence's age is by presents the pass ran at (and it stands down with frame generation unless allowed), and the guide identity's lag rule is enforced at the game's own present boundary only. No reset is derived from the count. |
| Vulkan never advancing `frameCount` (found by OpenCode) | **Verified, harmless for resets.** Nothing on the Vulkan route increments it, and the only reader there is `facts.present` in `DlssNrFeature_Vk.cpp` (diagnostics). The Vulkan reset flags are the game's, size changes, and the zero-guide policy; none uses the count. Left as it is: incrementing a shared counter from a Vulkan hook would change every other reader for no reset benefit. |
| Policy resets every frame | `ZeroGuideReset` is **off** by default (`Config.h`); on, it sets the reset on every zero-guide frame by design, and the present route does not apply it while the game's guides hold the history. |
| Route toggles | `g_resetOnReturn` is set when the source switches between the upscale route and the present route, and when a route stands down (game's own NR, multi-view, NR off). A game that really alternates the two routes every frame would reset every frame; the placement rules (`HookMethod`, Auto's 500 ms present-hook liveness) make that a configuration, not the default. Not seen in code as steady. |
| `g_nr.reset` raised by the pass | About fifteen sites: a setting changing (a pulse), a size, a route, the cadence's stale history, a failed guide resample, a held frame released. Each is one-off by construction. A site that fires every frame would now show up in the counter below as "this fork's own". |

### What was added

`ResetRate` counts every history reset sent to the model (the first pass's evaluate with `Reset = 1`), by cause: the game's own
flag, the zero-guide policy, the synthesized motion's scene cut, and "this fork's own" (everything else). The log says, at most
every 10 s while any were sent: `DLSS-NR resets: N sent to the model in the last 10 s (game a, zero-guide policy b, scene cut c,
this fork's own d); at most n in one second; T since the session began`. Always on; a counter and a ring of 128 timestamps,
no allocation per frame, no GPU work. D3D12 only: the Vulkan route's resets are not counted (its evaluate path is separate and
was left alone).

## How to use it in a game

1. Play the three-pass case where the flicker shows, read `OptiScaler.log`.
2. If a `DLSS-NR resets:` line shows a steady "this fork's own" count on a still camera, that is a spurious reset with a cause
   to find (the count by cause says where to look first); "game" is the game's own flag.
3. If a `frames between frames the pass ran on ...` line appears, the pass is alternating with the game's frame; the last
   reason says why. Try `SkipStandDown`, and compare with `WorkingScale` 200%, which the user reports lessens the flicker.

## Not verified

Everything in a game. No frame was seen shown raw. The admission rule, the swapchain counting and the policy defaults were
read, not run; the tests check the counting and the decision, not the picture.
