# Own frame generation swapchain and the game's DLSS-G

Run `python3 tests/sl-own-fg-swapchain/run.py` on the host with g++.

When OptiScaler makes the frame generation swapchain itself (an FG Input other than None or NvngxFG, with an FG
Output), a game's own Streamline DLSS-G must not hook that swapchain too: Streamline wraps it a second time and crashes
in sl.interposer at the first present (Forza Horizon 6 with OptiFG, janblade/OptiScaler-F5-DLSSNR-Multipass PR #347).
`retoque/hooks/Streamline_OwnFgSwapchain.h` decides it; `hkdlssg_slOnPluginLoad` then clears the plugin's hooks as it
already did for FG Input/Output DLSSG, and `hkslDLSSGSetOptions` keeps the game's DLSS-G off with its resources freed.

The runner compiles the header against the production `FGInput`/`FGOutput` enums sliced out of `OptiScaler/State.h`
under ASan/UBSan and checks every input, output and API against a table written in the runner. Six rule mutants
(Vulkan included, NvngxFG or None making a swapchain, no output needed, FG Input DLSSG counted as a replacement, a
replacement without cleared hooks) must fail. The rule is held to the first rule of `CheckForFGStatus`, and the three
call sites (plugin load, `slDLSSGSetOptions` before its auto/Dynamic MFG policy, the frame generation menu) are read off
the source.

This checks the decision and its wiring. It does not run Streamline, a game or frame generation: the crash and its fix
were seen by janblade in Forza Horizon 6 on their fork, not on this build. D3D12 only; Vulkan and SL1 games are unchanged.
