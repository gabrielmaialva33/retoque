# NR present lists and a fence that proves nothing

```bash
python3 tests/nr-present-fence/run.py
```

`ID3D12Fence::GetCompletedValue()` answers `UINT64_MAX` once the device is removed. The D3D12 present route compared
that as a value, so a removed device read as "every list finished": the slot's allocator was reset, the idle wait
answered true and the retired HDR textures were released. `retoque/dlssnr/DlssNr_FenceProgress.h` now classifies a
completed value as completed, pending or removed, and every look at the present lists' fence goes through it.

The runner compiles that header and the verbatim production helpers from `DlssNr_Dx12.cpp` (the removal latch and
`ReadPresentFence` through `WaitPresentListsIdle`, plus `RetirePresentHdrTexture` and `CollectPresentHdrRetired`)
against fake D3D12 objects with g++ C++20, ASan/UBSan and `-Werror`, one scenario per process.
The compilation also includes the real portable advisor runtime, left idle, for
the removal latch's cancellation call; none of the eight fence mutants is replaced
or skipped because of that dependency.

## What this covers

- **A live device is unchanged**: a slot is reused once its fence value is reached, after the bounded wait when it
  is not there yet, and skipped (said once) when the wait times out; the idle wait answers as before; a retired HDR
  texture is released at the value it was retired at and not before; a build that fails part-way releases what it
  made.
- **A removed device is never completion**: seen before the reuse, during the allocator wait (the wake is not
  completion), or as a registration that fails. No allocator is reset, on that slot or any other, no list is handed
  out again, nothing is released, and the log says it once.
- **The idle wait** is false on a removed device, when the removal wakes it, and when the shared event was left set
  by an earlier registration while the fence has not reached the value.
- **Retired HDR textures** are kept on a removed device (the review's case: retired at 3, the fence reads
  `UINT64_MAX`).
- **A refused `Signal`**: the list was handed to the queue, so the answer stays "executed". With
  `DXGI_ERROR_DEVICE_REMOVED` the route ends as on any removal; on a live device it is said once and the slot stays in
  use until a later signal on the queue passes the value that was never reached.
- **Mutation controls**: eight changes that take one rule out (in the helpers or in the header) each fail a scenario.
- **Source guards**: one reader of the present fence in the renderer, `PresentSync`'s wait decides through it, the
  latch is cleared only with the objects it describes, and the header is listed in `OptiScaler.vcxproj` and
  `OptiScaler.vcxproj.filters`.

The same rule in the native D3D11 guide transport (`SlotFree`, `CacheFree`, `DrainLocked`) is held by
`tests/nr-dx11-native-guides`.

## What this does not cover

No D3D12 runtime and no GPU: the fakes say what a removed device answers (`UINT64_MAX`, woken waiters, a refused
`Signal`) as the D3D12 documentation describes it, and no device was removed under this code on Windows or Proton.
`RunPresentPass` is not executed; its one use of the rule is a source guard. `DlssNr::Shutdown` still releases the
present lists, the backbuffer copy and the HDR textures unconditionally (nothing calls it today), and fences outside
`retoque/dlssnr` and `retoque/shaders/dlssnr` were not changed.
