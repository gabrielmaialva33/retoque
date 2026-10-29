# OptiScaler instance claim

`python3 tests/optiscaler-instance-claim/run.py` compiles `retoque/misc/InstanceClaim.h` against a fake Win32 (named
mutexes, the process environment, `OutputDebugStringW`, the process id) together with dllmain's attach block, sliced
from `OptiScaler/dllmain.cpp` from `CheckForExcludedProcess()` up to the pass-through branch, under ASan/UBSan.

Two proxy names in one game folder load two copies of OptiScaler, whose hooks fight until the game dies at start
(CLAUDE.md, "One proxy per game directory"). The first copy to attach now claims the process; a later one runs in
upstream's pass-through mode (it only forwards its exports) and leaves its path in a per-process environment
variable, which the first copy reports where it logs the GPU list (`OptiScaler/misc/IdentifyGpu.cpp`, checked here
by text).

Cases: a second and a third copy stand down and are both reported; a single copy reports nothing; a child process
(another pid) neither inherits the claim nor reads the parent's note; an excluded process is pass-through without
taking the claim from the copy that runs; when no mutex can be made every copy keeps its hooks, as before. Without the
change six checks fail. The fake proves the decision, not Wine's or Windows' loader order.
