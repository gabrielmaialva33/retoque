# Native D3D11 scene-stage host contracts

`python3 tests/nr-dx11-scene-stage/run.py` compiles the production admission/fence/queue policy,
checks config declaration/startup snapshot/read/save/INI, the native backend success seam, main-picture
ordering, shared-fence producer/NR/consumer edges, the memory bound, retirement, status and mandatory
renderer recording tracking. Nineteen mutations run independently and must each fail their relevant oracle.

`scene_cases.py` also compiles the complete `DlssNr_Dx11Scene.cpp` control flow against inert graphics
and renderer interfaces in `scene.cpp`, using the real picture, geometry and policy headers. Nineteen
scenarios cover input Color versus Output identity, missing/mismatched/zero feature bindings,
NR-off creation then enable/recreate, reset debt after enable/stage/FG/busy gaps, Aside/cold paths,
a concurrent main evaluate, and each known FG signal. Nine further mutants must compile and fail
runtime assertions (a compiler error is a test failure). Older routes' unbound behavior is checked.

Outputs stay in ignored `artifacts/`. No game, Windows, GPU, NGX or image-quality coverage is claimed.

[The GPU fixture](../dx11-scene-transport/README.md) compiles the actual production transport header
and checks same-frame pixels and early Present ordering on a local, explicitly identified Proton runtime.
