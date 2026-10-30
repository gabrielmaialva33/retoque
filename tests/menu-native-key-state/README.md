# Menu key polling beneath other overlays' input hooks

`python3 tests/menu-native-key-state/run.py` splices `OptiInput::RealGetAsyncKeyStateSafe`
(`OptiScaler/menu/input/input_system_detours.cpp`) after the real `retoque/menu/input/native_key_state.h` and compiles
them against a fake win32u and a fake user32 detour chain under ASan/UBSan. In the chain another overlay's
`GetAsyncKeyState` detour reports every key up, as REFramework's does while its UI takes input
(tig3rmast3r/OptiScaler_DLSSNR_VR 3054caf2).

- `native`: the held menu key (Insert) and a mouse button read as down and their release as up, with no call into
  the chain; keys outside 0-255 never reach the system call.
- `missing`: win32u without `NtUserGetAsyncKeyState` (Windows before 10 1607) keeps the previous path: the trampoline
  with our own detour bypassed, or `GetAsyncKeyState` itself without one.
- By text: the menu's key and mouse polls (`PollVirtualKeyLocked`, `PollMouseButtonLocked`) go through
  `RealGetAsyncKeyStateSafe`, and `input_system.cpp` never polls `GetAsyncKeyState` directly.

What it does not prove: that the real `NtUserGetAsyncKeyState` matches user32's answer on Windows or Wine/Proton, or
that REFramework (or any other overlay) no longer blocks the menu key in a game. That needs an in-game check. The
menu's window-message path is unchanged and not covered here.
