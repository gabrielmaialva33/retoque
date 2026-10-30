# Wrapped swapchain present re-entry

Run `python3 tests/fg-present-reentry/run.py` with Python and g++.

The suite compiles `Present`, `Present1` and `SetFullscreenState` of `WrappedIDXGISwapChain4` verbatim, the local
mutex wait and FG mutex acquisition of both `ResizeBuffers` methods, and the real `OwnedMutex`, under ASan/UBSan with
warnings as errors. DXGI, the present path's body and OptiScaler's state are fakes.

Same thread: a call from inside `Present` (owner 4) or `Present1` (owner 5), as when DXGI delivers an alt-tab or resize
message during present and the game resizes from its window procedure, returns instead of waiting for its own
non-recursive lock, with and without the nvngx DLSS-G replacement. A resize re-entered from OptiScaler's FG present,
which holds the FG mutex as owner 2, goes on under that lock and leaves it held. Before the fix libstdc++ reports each
of these as `EDEADLK`; Windows' SRW lock hangs.

Other threads keep their protection: while a present holds the local mutex, or the FG present the FG mutex, the
three methods wait and run their device work afterwards. The one exception is upstream's unchanged rule, which lets
them through while `Present` (owner 4) holds it under the nvngx DLSS-G replacement. A variant that skips whenever a
present owns the lock on any thread (the later hhkbble/OptiScaler 2faca8c0) fails these cases.

Limits: real threads and real mutexes, but no DXGI, no window messages, no Streamline or provider present threads.
A cross-thread wait inside a provider's own present thread is not covered. GPU work, pacing and native
Windows message delivery need a game run (alt-tab and resize under DLSS-G, FSR-FG and XeFG).
