# Native Vulkan pre-upscale infrastructure (stage 2b)

`python3 tests/nr-vk-pre-upscale/run.py`

Compiles the production admission/observation headers under ASan/UBSan and the complete passive runtime adapter
against fake driver/resolver/Detours boundaries. Runs compiling policy and observation mutants serially. Scratch
stays in this suite's ignored `artifacts/` directory. `--no-mutants` is a development shortcut, not the complete gate.

Contracts: opt-in install, exact API results/arguments, success-only creation/lifecycle observation, aliases attached
once, composite device/handle keys, independent view epochs, image incarnations and exact feature-source release,
command-pool family, canonical VkRecording tickets, primary/one-time/simultaneous facts, explicit
in-recording layout evidence, rendering-state refusal, copied pipeline/descriptor/push state, unsupported-binding
invalidation, feature type/device/incarnation and release/device cleanup. Stage-1 rule tests reject RR, FG, every
on12 backend, unknown identity, missing evidence and multiple outstanding recordings. Selected Stage 1 never falls
back in the proposed outcome table; **the scope consumes this table on the live native NGX input route**.

No Vulkan loader, real Detours patching, NGX, game, driver/GPU memory or shader execution is exercised. Missing hook
targets are reported, not qualified. A complete list of installed targets still does not prove GPU completion,
queue ownership, resource usage rights or correct replay of application bindings. The design note documents
intentional observation gaps. The full host gate and runtime qualification belong to later work.

`identity.cpp` covers cross-device numeric collisions, view/image reuse, scoped reset/free/destroy and exact NGX
release. `recording.cpp` covers the canonical ticket, failed/repeated/missing/late Begin observations, mismatched
device/buffer/reuse and retirement. The adapter fake exercises real `VkRecording::Recordings()` with early,
missing, late and duplicate lifecycle callbacks. These are host ordering contracts, not proof of live hook order.
`source_contracts.py` hashes the preserved post-upscale body against HEAD and the baseline; a synthetic body
change must be detected. Nine additional source mutants exercise placement, completion, layout and lifetime guards. It also rejects new lifetime-registry mutations in the passive adapter.

The revised step 3 uses the official NGX evaluate-window state contract: no binding replay. `scope_contracts.py`
extracts the production scope class, constructor/destructor and resource admission into a host fixture with fake
prepare/SR boundaries. Twenty-three scenarios cover binding, restoration before End, thrown/partial preparation and
SR exceptions, aside/RR/unknown/bridge refusal, active rendering, layouts, one-time admission, format enablement,
subrects, exclusive/missing-family ownership and view recreation. The fake SR establishes its own state and the
fake game rebinds before its sentinel dispatch. Eleven compiling scope mutants are rejected. This does not execute
the GPU preparation helper or qualify its shader/NGX behavior.

The recording helper's source guards check pin/reservation before create, owned scratch writes, sampled exit,
scope-only invocation, unchanged Stage-0 body and absence of added GPU waits/submits. Binding observations are
diagnostics only; diagnostic hook gaps do not qualify or disqualify layout coverage. The runtime fake also checks
device creation's formatless-storage/format-flags2 facts. DOOM Eternal native Vulkan DLSS on Proton is the future
real validation target, not a passing result.

`placement.cpp` exercises the atomic first-placement decision with real racing threads: a cold post caller wins
and pre refuses, or pre wins and every post call needs the CPU loan. The decision is process-latched (restart to
move from a post session to pre), so cleanup cannot invalidate a caller already waiting for the model mutex.
`gate.cpp` checks the nonblocking init gate, recursive ownership and refusal while device creation holds it.

Review regressions (`runtime_contracts.py` and `history_contracts.py`, both included in `run.py`):

- The complete runtime adapter is compiled against fake resolver/Detours boundaries. Nineteen coverage cases
  include core Vulkan 1.3 without optional extensions/KHR aliases, alias-only equivalents, a missing mandatory
  command in each promoted pair, failed attachment of each callable alias, failed core/export attachments and
  optional diagnostic failures. Distinct core/alias/device/export addresses must all attach; loader export stubs
  alone cannot qualify a missing device command. Seven compiling mutants must fail behavioral assertions.
- The history fixture compiles the production post admission prefix, wrapper, recording-busy helpers and both
  real model-call tails. Shader/allocation preparation and model execution are fake; the fake captures the actual
  reset argument, same feature identity and equal 100x50 dimensions. Real recording-registry pins cover a busy
  recording, End without retirement, legal Reset and re-Begin. The first post call must get reset=1, the second
  reset=0, including skipped unknown-recording, Aside and preparation gaps; post-only stays unchanged. Four
  compiling mutants cover forgotten publication, ignored consumption/model reset and bypassed retirement.
  Source guards also require publication under the pre route lock and preserve the post body's hash.

These checks prove the host control flow and arguments, not GPU retirement, NGX temporal quality or live Detours
coverage. The game must legally retire its Vulkan recordings; the fixture's Reset is not a new completion oracle.
