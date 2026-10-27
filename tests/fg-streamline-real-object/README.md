# FG Streamline real object

`python3 tests/fg-streamline-real-object/run.py` slices `Util::CheckForRealObject` from `OptiScaler/Util.cpp`, the
two factory blocks of `DLSSG_Dx12::CreateSwapchainInternal` and `CreateSwapchain1Internal`
(`OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp`) and the head of `DxgiFactoryHooks::HookToDLSSGFactory`
(`OptiScaler/hooks/DxgiFactory_Hooks.cpp`), and compiles them under ASan/UBSan against a factory proxy that answers
Streamline's retrieve-base query the way `sl.interposer`'s `DXGIFactory::QueryInterface` does: the base, AddRef'd
once.

`CheckForRealObject` releases that reference itself, so the object it hands back is borrowed. The three sites
released it a second time, one reference off the real factory per DLSS-G swapchain created over a Streamline
proxy (wilsjo2 `e797906f` found the same in their tree). The matrix creates eight swapchains through each site and
checks the real factory's count is unchanged; before the fix the fake aborts on a release with no reference left.

The second half reads every `CheckForRealObject` caller under `OptiScaler/` and `retoque/` and fails when one
releases the variable it passed, up to the end of the enclosing function. Before the fix it named the three sites.
The fakes stand in for Streamline and DXGI; this proves the reference arithmetic, not a game's behaviour.
