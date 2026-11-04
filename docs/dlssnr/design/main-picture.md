# Several live DLSS features: NR follows one main picture

Date: 2026-11-04. Status: implemented and host-tested, revised after an independent review (Codex, five findings, all
taken). No game with two live features has been run with it.

## Report

janblade/OptiScaler-F5-DLSSNR-Multipass #285 (GPL-3.0; idea only, no code taken): Neverness to Everness keeps a
second DLSS feature for the 3D character preview of its Character, Gift and Party pages, next to the world's, and
evaluates both every frame. Their NR had one model and one history, so the two pictures took turns in it.

## What Retoque did before this

This was traced by reading the code at `92982c03`. Nothing was run.

NR never received the game's feature handle. Every seam keeps one state:

- **Different output sizes or formats, D3D12 (the preview case):** the after-upscale settling gate `g_postExtent`
  saw a new contract on every call and restarted its 500 ms. Neither picture was evaluated; the skip reason was
  "working resolution is settling". The world had no NR for as long as the preview was shown, plus 500 ms. Stage 1
  behaved the same through `g_preExtent`. Nothing rebuilt, which is why it never froze as janblade's before-SR path
  did.
- **Same size and format, separate allocations:** both pictures ran on one model. Each was reconstructed over the
  other's history. Two shared states could also mix the pictures:
  - the exposure meter's slot ring;
  - the cadence signature, which carried no handle.
- **Adaptive cadence** turned on the chain's once-per-present guard. Whichever feature evaluated first in a present
  ran, and the other was skipped.
- **Vulkan:** the preview's evaluate, refused while older recordings were still busy, set the reset flag. The world's
  history could start over on every frame.
- **Release:** `ReleaseFeature` told only the exposure scan and the render-geometry record.

## What it does now

`retoque/dlssnr/DlssNr_Pictures.h` holds the rule, using the standard library only. `DlssNr_PictureGate.h` is the
glue. NR follows one feature, the main picture. Any other feature is left as it is: its evaluate passes untouched,
and no NR state sees it.

- **Who asks.** Each NR seam asks right after its NR-off way out and before any other: `ScopedPreUpscale` (before the
  Present hook-method decline), `EvaluateAfterUpscale` (before the ApplyAfterRR decline), `EvaluateAfterUpscaleVk`,
  and `CarryNativeDx11Guides` (the native D3D11 guide transport, after its not-wanted check).
  - The question uses the feature key and output size from the evaluate's render-geometry binding
    (`DlssNr_RenderGeometry.h`, per input API and NGX handle), plus the colour allocation the seam reads. The colour
    is read only when there is a bound evaluate to judge, so an unbound evaluate reads nothing new.
  - The first seam judges the evaluate. A second seam of the same evaluate, such as a stage-1 scope's after-upscale
    fallback or a D3D11/Vulkan bridge, gets the same answer.
- **An aside picture** returns before the coverage, the render count, the status line, the settling gates, the meter,
  the cadence and the recordings.
  - It is not declined, so the after-upscale seam does not run over it.
  - It never sets the history-reset flag of the picture NR follows, and reports no skip reason.
  - On the after-upscale route it is counted in the funnel as `other_game_picture`.
- **Which feature is main:**
  - The first feature to evaluate.
  - A picture with a quarter more output area takes main: the world, not a preview. An unknown area never does.
  - A feature on main's colour allocation is a view of main's picture (one feature per screen of a multi-monitor
    layout). It goes on as before to the multi-view stand-down (`dx12-robustness.md` §2), even when it is larger.
    Views keep main alive.
  - When the game releases main (`RenderGeometry::Forget`), a feature not yet known as another picture takes main at
    once: the same picture, made again. A known other picture waits 16 judged evaluates for that successor, and then
    takes main as a new picture.
  - When main stops evaluating while others evaluate, 16 judged evaluates later the next caller takes main. This covers a menu
    scene's feature the game never released, and a feature made again before the old one was released.
- **History: picture epochs.** A takeover by size, by quiet, or by a known other picture after the wait opens a new
  picture epoch. Taking main first, or as an unknown successor after a release, keeps the epoch, so a game with one
  feature, made again, keeps exactly what it had.
  - The epoch is cached with the evaluate's judgement and travels in its frame (`DlssNrFrameInfo::PictureEpoch`, set
    by `GatherGuideFrame`). A frame captured for the present carries the epoch of the evaluate that captured it. This
    includes the native D3D11 transport and the D3D12 present route.
  - Whoever holds the model's history consumes the epoch under its own lock: `Dispatch` under `g_nrMutex`, the Vulkan
    route under `g_vkMutex` (`Pictures::EpochGate`). It does not matter in which order evaluates reach that lock.
    - A newer epoch starts the model's history over. It also invalidates the exposure meter: the held exposure, the
      readback slots and the plausibility latch.
    - An older epoch is a frame of a picture already replaced, and is refused.
    - The first epoch a holder sees is its own, and a frame no rule judged (epoch 0) changes nothing.
  - The seams no longer request a reset themselves. A boolean request has no picture identity and could be consumed
    by the wrong evaluate (review 2026-11-04, F2).
- **Logs.** INFO lines, all multi-feature only: the first time a picture is left aside, every 3600th aside
  evaluate, and each takeover. A game with one feature logs nothing new.
- **No exceptions.** An evaluate without a binding is main, as every evaluate was before: a feature created while NR
  was off, a replay, or a caller outside the NGX inputs.

There is no setting, so no control and no configuration key.

## Not taken

- **A model of its own for each picture** (#285's "Own model"). That needs a second model and its surfaces, and a
  lifetime and retirement per picture on release. Nothing has measured its cost.
- **Sharing one state** (#285's "Share"). That was the behaviour before this.
- **Telling the meter, the cadence signature or the multi-view detector which picture a call is.** Nothing but main
  reaches them now, so they still see one picture.

## Limits

- A quiet handover, or a known other picture waiting for main's successor, is counted in judged evaluates, not frames.
  Both seams of one evaluate count once. With one other feature evaluated per frame, that is 16 frames without NR on
  the new picture; with several per frame, fewer.
- The colour identity is the allocation the seam reads. On the D3D11 and Vulkan bridges, the first seam to judge may
  see the bridge's own copy. A view of main through a bridge is then judged as another picture, and its feature is
  left aside instead of being stood down by the multi-view rule. Either way NR does not run over it; only the
  multi-view counters differ.
- The order in which a real game evaluates its features, and whether a preview evaluates every present, have not been
  observed.

## Tests

`tests/nr-main-picture`:

- The rule and glue on fake parameters under ASan/UBSan, against these scenarios:
  - one feature, made again;
  - the world and a preview, at different sizes and at one size;
  - a larger picture made later;
  - views of main;
  - a quiet main;
  - main released while the preview is open;
  - unbound, foreign or disabled evaluates.
- Picture epochs: the epoch gate in both arrival orders (old then new, new then late old), unjudged frames, and the
  first epoch a holder sees.
- 30 rule and glue mutants.
- Source guards with 21 mutants, covering:
  - the four seams' order;
  - the lazy colour read;
  - the epoch carried by `GatherGuideFrame`;
  - its consumption under `g_nrMutex` and `g_vkMutex`, with the restart, meter invalidation and stale refusal;
  - the release hook.

`tests/nr-before-upscale` runs the world and a preview through the production `ScopedPreUpscale` and
`EvaluateAfterUpscale`, in both stages and in either order. The preview reaches no pass, borrows no scratch, is never
declined, and leaves the world's status, coverage and history flag as they were. In one of the orders, the preview
evaluates first from the start and the larger world takes main from it. There, `Dispatch` is the suite's fake, so the
epoch consumption is covered by the rule cases and the source guards, not end to end.

Not proved: any game's feature layout, the picture on a GPU after a takeover, or native Windows.
