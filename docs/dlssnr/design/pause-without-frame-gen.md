# Pause NR while the game turns its frame generation off

Design before implementation, 2026-10-20. Idea from kkyleeb21's D18 0.4.0 ("Pause NR while the game turns frame
generation off"); our implementation is independent.

## Why

Many games turn their own DLSS-G off in menus, dialogue and cutscenes, and drop the frame-rate cap with it. NR
then runs at the full uncapped rate on frames nobody needs enhanced, which costs power and heat and makes the
menus heavier than the game. `[DlssNr] PauseWithoutFrameGen=true` skips the pass while that lasts.

## Rule

Revised after Codex's review: the first draft read the post-policy mode, lost resets, counted entries rather
than time, and offered the control on native Vulkan.

- **Off by default.** The pass is unchanged unless the key is set.
- **The signal is the game's own request.** `hkslDLSSGSetOptions` records whether the game asked Streamline for
  DLSS-G into `State::dlssgGameRequestsFg`, an atomic, before any OptiScaler policy runs: the menu interlock, the
  interpolation override, our own DLSS-G substitution. None of those can pause NR. DLSS-G a game drives through
  NGX without Streamline is not seen; the pause simply never arms there.
- **Arms only after seeing it.** Nothing happens until the game has asked for DLSS-G in this session.
- **Never for our own menu.** With our menu open, NR keeps running so settings can be judged.
- **Debounced by the clock.** The off state has to hold for 500 ms, so the options the menu re-sends when it
  closes cannot flicker it. It resumes on the first frame the game asks for DLSS-G again.
- **History.** While paused, `g_resetOnReturn` is set, so the first evaluation after resuming resets the model's
  history. The model did not see the frames in between, and any reset the game asked for then was never
  delivered.
- **What it skips.** Inside `Dispatch`, before any tracking or recording. Callers may already have done their
  own preparation (the before-upscale scope's copy, a present host's decode); those keep the original frame
  when the pass declines, as they do for any refusal.
- **Where it shows.** D3D12 and the bridges only. The menu hides the control on native Vulkan.

## Validation

- **Host.** A source contract (config, the arm, debounce and menu-exclusion order).
- **In game.** Cyberpunk's pause menu turns DLSS-G off: with the key on, NR pauses there and comes back on
  resume. The log shows the transitions.
