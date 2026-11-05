# Native D3D11 scene-stage host contracts

`python3 tests/nr-dx11-scene-stage/run.py` compiles the production admission/fence/queue policy,
checks config declaration/startup snapshot/read/save/INI, the native backend success seam, main-picture
ordering, shared-fence producer/NR/consumer edges, the memory bound, retirement, status and mandatory
renderer recording tracking. Nineteen mutations run independently and must each fail their relevant oracle.
Outputs stay in ignored `artifacts/`. No game, Windows, GPU, NGX or image-quality coverage is claimed.

[The GPU fixture](../dx11-scene-transport/README.md) compiles the actual production transport header
and checks same-frame pixels and early Present ordering on a local, explicitly identified Proton runtime.
