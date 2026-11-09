# Upstream integration review regressions

`python3 tests/upstream-review/run.py` compiles production source slices under ASan/UBSan.
`--ref 7e65b0af` reads the reviewed revision through `git show`, without checking it out or writing to it.
`--only vulkan` selects a case group. RED means a runtime assertion fails, not a missing symbol or compile error.

The Vulkan case compiles both actual destroy hooks, the shared protocol and the real deferred queue.
Boundary fakes supply model/driver outcomes and child-owner releases. It checks single preparation through
stacked hooks, children before retirement, successful release before the real destroy and no retry after
failed drain. This does not replace Vulkan validation, driver execution or native Windows qualification.

The other groups compile actual Get/Allocate/DestroyParameters entries and the proxy destructor getters,
all six inline UI submission sites with the allocator/wait helper, FG release/resize queue boundaries,
wrapped presenter creation, bridge description/dimension publication, custom runtime shutdown, automatic
D3D11 selection and the loader-lock NVAPI condition. Boundary fakes script failures; they do not establish
arbitrary vendor ABI compatibility, real Windows loader behavior or GPU correctness.
