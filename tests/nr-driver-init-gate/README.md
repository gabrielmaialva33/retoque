# Driver-init gate (host)

```bash
python3 tests/nr-driver-init-gate/run.py
python3 tests/run_all.py --only nr-driver-init-gate --strict
```

The production `retoque/dlssnr/DlssNr_DriverInitGate.h`, compiled alone with g++ C++20, ASan/UBSan and `-Werror`, on
real threads with real mutexes standing for ours. Design: `docs/dlssnr/design/nr-vulkan-present-host.md`, "The
driver-init gate (2026-10-30)".

## Behaviour covered

- **exclusion**: a second holder on another thread never holds the gate while the first does, for every pairing of
  a vkCreateDevice call down and an NGX call; the wait names what the other thread held.
- **reentrant**: the owning thread re-enters at three levels without waiting; others are kept out; it is free after
  the outermost scope.
- **not-wanted**: `Scope(..., wanted=false)` (NR off) neither waits, holds nor counts.
- **suspend-resume**: the present host's hook below OptiScaler's runs its own code outside the gate (`OwnCode`), the
  call down inside it, and the outer hold is back when it returns.
- **no-suspend-under-ngx**: a create nested inside an NGX call keeps the gate.
- **bounded**: a holder that never lets go costs a waiter the bound; the waiter goes ahead unheld, says so, and its
  scope's end releases nothing of the holder's.
- **resume-times-out**: a hold that cannot be taken back is accounted; the outer scope's end does not release
  another thread's hold.
- **hook-chain**: the cycle the design avoids -- vkCreateDevice (gate, then the transport lock), vkDestroyDevice (the
  transport lock, then `g_vkMutex`), the after-upscale route (`g_vkMutex`, then the gate) -- finishes with no wait
  reaching the bound.
- **process-and-reporter**: one process gate; a set reporter receives a timed-out wait's line as a warning, after
  the gate was let go, and nothing for an uncontended scope.

## Mutation controls

Each must turn the named cases red: `OwnCode` letting nothing go (`hook-chain` reaches the bound, `suspend-resume`),
no exclusion (`exclusion`), no re-entry (`reentrant`), suspension under an NGX hold (`no-suspend-under-ngx`), and a
lost hold's end releasing whatever is held (`resume-times-out`).

## Source guards

- Each site sits in a block that takes the gate first and runs one statement: `g_nr.create` (2, D3D12),
  `NVNGXProxy::InitDx12` (the NGX core for the D3D12 model), the opt-in proxy probe's two creates and the D3D11 init
  probe, the enlargement's DLSS feature, `g_fwd.init`/`g_fwd.create` (the Vulkan after-upscale route and the present
  host's builder), `NVNGXProxy::InitVulkan` (the builder), and the two vkCreateDevice call downs.
- The listed locks held at each site match the documented order: `g_nrMutex` on the D3D12 route and the D3D11 probe,
  `g_vkMutex` on the after-upscale route, nothing in the builder, the probes' and core's callers and the hooks.
- Both vkCreateDevice gates are conditional on `[DlssNr] Enabled`; `OwnCode` is the first declaration of the present
  host's hook and appears nowhere else.
- The gate is used only in the reviewed files, and its reporter is set once.
- `g_preMutex`, `g_nrMutex`, `g_vkMutex` and `g_presentNgxMutex` are taken only in the files they were when the
  order was checked; a new site fails the suite until reviewed against it.

## RED / GREEN (2026-10-30)

Against the tree before the call sites were wired (header present): the nine behaviour cases and five controls passed,
34 site guards failed (`... takes the gate first`, `OwnCode first`, `gates only with [DlssNr] Enabled`). The cases
compiled against a tree without the header fail with `dlssnr/DlssNr_DriverInitGate.h: No such file`. After wiring,
the suite passes.

## What this does not cover

No driver, NGX, Vulkan or D3D12 runs. It cannot show the NVIDIA driver's lock inversion, that the gate removes the
Assassin's Creed Odyssey freeze, or that no NGX-internal path takes a lock a gate waiter holds; the waits' lines
(`DLSS-NR driver-init gate: ...`) are what a game run should be read for. The hook order (the present host's
vkCreateDevice below OptiScaler's) is taken from `DlssNr_VkPresentHost.h`, not observed.
