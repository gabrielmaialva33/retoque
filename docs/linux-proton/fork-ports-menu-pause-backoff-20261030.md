# Dynamic MFG retry wait, NR pause guard and menu key polling (2026-10-30)

Three fixes from other GPL-3.0 forks, each verified against this tree and adapted rather than merged. No NR model,
shader, config key or default changes. Host-tested only; see the limits under each fix.

## Dynamic MFG: a failed step down waits up to 10 minutes

Source: [janblade/OptiScaler-F5-DLSSNR-Multipass a1490c26](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/commit/a1490c26).

Applies as is: both controllers (`XeFG_DynamicCount`, `XeFG_DynamicCap`) capped the wait after a failed step down at
60 s. Where a limiter holds the presented rate at the target, a GPU bound game never looks lighter than when the step
failed, so only the clock retried it: in NBA 2K27 with XeFG a 3X -> 2X step was taken and undone about every 61 s, each
a visible dip. The cap of the step-down wait is now `kMaxDownBackoffMs` = 600 s in both. The count controller's
up-step wait keeps the 60 s `kMaxBackoffMs`. Recovery is unchanged in kind: a lighter game the count controller can
see (8% more presented) or the cap controller's estimate (10% more reachable) still bypasses the wait, a new target
clears it, and otherwise it ends when the wait does.

Host (`tests/xefg-dynamic-target`, same simulated game as the source's test): undone steps down in minutes 10-20 fell
from 10 to 1 (count) and 9 to 1 (cap); the longest wait reached 600 s; once the GPU floor goes away both step down to
2X, at +268 s and +56 s, and stay. Not measured here: any game. Behind a limiter, a game that gets lighter can now keep
one count more for up to 10 minutes.

## PauseWithoutFrameGen: only accepted requests, one viewport

Source: kkyleeb21/OptiScaler_DLSSNR, D18 0.4.1 (3f61eb29). Its pause implementation is not ours; the two guards are.

Both applied. `hkslDLSSGSetOptions` stored the game's request in `State::dlssgGameRequestsFg` before calling
Streamline, whatever it returned and from whichever viewport. A refused FG-off could pause NR while DLSS-G still
generated frames, and a game with two DLSS-G viewports, one on and one off, would toggle the one signal between them.
The request is still taken before any OptiScaler policy, then published only when the call returned `eOk`, only from
the first viewport accepted (`retoque/hooks/Streamline_DlssgRequest.h`, one lock-free word), and never for
`updateDlssgOptions`, OptiScaler's own replay: before the game's first call that goes out on viewport 0 and would make
a game using another handle look like two. Once a second viewport is accepted, `PausedWithoutFrameGen` never pauses
for the rest of the process. The rule is unchanged otherwise.

Host: `tests/nr-pause-frame-gen` (contract and the owner under ASan/UBSan) and three `pause-request-*` cases in
`tests/mfg-provider-capacity` driving the production setter. Not covered: any game with two viewports, and the
menu does not report why the pause stood down (the log is the same as before).

## Menu key polling beneath other overlays' GetAsyncKeyState hooks

Source: [tig3rmast3r/OptiScaler_DLSSNR_VR 3054caf2](https://github.com/tig3rmast3r/OptiScaler_DLSSNR_VR/commit/3054caf2).

Applies: `OptiInput::RealGetAsyncKeyStateSafe` was the source's code before the fix. It called our detour's
trampoline or `GetAsyncKeyState`, so another overlay's detour (REFramework reports keys up while its UI takes input)
also hid the menu key from our polling. It now calls win32u's `NtUserGetAsyncKeyState`, the system call underneath
user32 (`retoque/menu/input/native_key_state.h`). Unlike the source, where the export is missing (Windows before 10
1607) it keeps the previous path rather than reading every key as up; the choice is logged once. In Wine 11.20 the
export tables (read, not run) show user32's `GetAsyncKeyState` as its own function, not a forwarder, and win32u
exporting `NtUserGetAsyncKeyState`, so a detour on the former does not sit on the latter; Proton was not checked.

Host: `tests/menu-native-key-state`. Not covered: the real export on Windows or Proton, REFramework in a game, and the
window-message path, which is unchanged.
