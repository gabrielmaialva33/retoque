# Vulkan recording ownership (host)

Run `python3 tests/nr-vk-recording/run.py` from the checkout. It exercises the production
recording helper, dispatch/route bodies and lifecycle wrappers with C++20, ASan and UBSan.
It needs GCC, Clang and the repository's Vulkan headers; no Vulkan driver or Wine is used.

The cases cover:

- admission only after a known allocation and a successful begin;
- End retaining pins for submission and legal resubmission, while rejecting additional recording;
- all-or-none pinning, deduplication by shared ownership, and stale ticket rejection;
- individual reset/free, pool reset/destruction and device invalidation, including handle reuse;
- generation quiescence after two old buffers recycle without acquiring new pins;
- preservation of a pin after a simulated failure partway through recording;
- resource deleters reentering the registry after each retirement operation, without its mutex held;
- the nineteenth live slot being refused, atomic multi-slot reservation and reuse only after release;
- shared leases and their state surviving destruction of the `SlotPool` object;
- slot leases held by an executable recording until reset, and concurrent reservation exclusion.

## Contract used by the integration

The caller sends lifecycle notifications **only after Vulkan succeeds**. Failed operations must not
be notified. `Begin` covers the implicit reset of an old executable recording. Allocation alone does
not authorize recording. A ticket is valid only for its device, buffer and current open recording epoch.
End, queue submission and GPU completion are not retirement events: executable recordings may be
submitted again. Valid reset/free notifications establish that the old recording cannot execute again.

The caller must stop admissions and prove device safety, or mark its GPU objects abandoned, before
`ForgetDevice`. This helper issues no Vulkan calls and does not decide whether GPU destruction is safe.
Opaque tokens must preserve the caller's existing device-loss policy. Shared-pointer deleters run
outside the registry lock, but any additional API/thread-affinity requirements remain the caller's.

`SlotPool::Acquire(n)` returns a shared lease with a read-only `slots` vector view. Reservations are
all-or-none; zero, over-budget and currently unavailable requests return null. The lease returns its
slots only when its final shared owner releases it. Its internal state is independent of the pass or
`SlotPool` object's lifetime. The caller reserves the complete frame requirement before recording its
first command and pins the lease for the recording that uses it.

## Limits

This is a host ownership test. It does not execute command buffers, query a fence, validate image
layouts, exercise NGX, or prove that all game/host/bridge function pointers reach the notifications.
It does not validate the integration's resource-mutation gates or its GPU work after a failed model
call. Real Vulkan validation/readback and lifecycle-hook coverage remain separate checks.

The default run enables leak detection. In a ptrace-restricted executor where LeakSanitizer cannot
inspect `/proc`, run with `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1`; address and undefined-behavior
sanitizers remain active, but that invocation does not validate leaks.

## Production-body Vulkan host integration

Run on a host with real Vulkan headers and both compilers:

```sh
python3 tests/nr-vk-recording/integration.py
```

`--compiler g++` or `--compiler clang++` selects one compiler; the default runs
both. Each translation unit is compiled with AddressSanitizer and
UndefinedBehaviorSanitizer. `VULKAN_HEADERS` may identify the Vulkan include
directory; otherwise the script tries `external/vulkan/include`,
`OptiScaler/external/vulkan/include` and the system include directory. It never
loads Vulkan, starts Wine, downloads assets, creates a GPU device or accesses a
game. `--generate-only DIR` emits translation units for inspection without SDK
headers or compilation.

These are behavioral integration tests of selected **production function
bodies**, rather than source-text presence checks. The generator extracts full
functions by signature and balanced braces, ignoring comment/string braces.
It compiles the real `DlssNr_Vk.h` and `DlssNr_Common.h`, plus unmodified
`CreateDummy`, `WriteDescriptors`, `Dispatch` and, when present, `Reserve` bodies.
A small base-shader stub supplies memory allocation, render readiness and layout
recording; fake Vulkan functions deep-copy descriptor updates and capture the
actual mapped constants used by each recorded dispatch.

The V-06 cases admit eighteen live dispatches, verify every earlier descriptor
and constant slice still matches its recorded snapshot, then require the
nineteenth admission to fail without changing uniform bytes, descriptors,
command counts or layout transitions. The baseline compatibility path calls the
old Dispatch signature when Reserve does not exist; it does not implement a
lease policy and still requires rejection. With the new API, real Reserve and
the production global recording registry retain leases after local Batch
objects disappear. The current-API case also reserves a Batch, performs a
real registry Reset/Begin on the same command-buffer handle, and requires the
stale Batch to be refused before any constant, descriptor or dummy-image effect.
A compile-time assertion requires the mutable Batch cursor to be move-only.

The placeholder case executes real CreateDummy through `Prepare(A)`, discards
the first recording without submitting it, then executes `Prepare(B)`. It
requires one image allocation and a transition in each recording, exposing a
CPU-cached layout that incorrectly assumes a discarded transition ran on GPU.

The V-02 cases extract complete `FreeRouteObjects`,
`FinishAfterUpscaleHandBack` and, when present, `RouteRecordingBusy` from the
production feature source. A fake Vulkan/NGX resource boundary counts actual
release calls, deferred destruction, pass destruction, parameter destruction
and owner hand-back commitment. Two shared recording pins must prevent both
free and owner commitment; dropping one must remain blocked, and dropping the
last must permit teardown/hand-back exactly once. The baseline fallback busy
helper returns false and cannot supply the missing production guard.

The integration generator uses `retoque/dlssnr/DlssNrFeature_Vk.cpp` from the checkout.
Extraction or compilation failures are failures, not skipped coverage.

These host tests establish admission, immutability and destruction boundaries.
They do not prove hook installation, real driver behavior, submitted GPU work,
rendered image quality or a game integration. The separate recording-registry
suite owns lifecycle/epoch/lease tests; native GPU fixtures remain necessary
for execution coverage.

## Lifecycle wrapper boundaries

`hooks_test.py` extracts the production application wrappers and the host-owned
allocation/begin/end/free paths. A fake driver supplies success and failure results;
the real recording registry must retain pins on failures, retain them after End,
and release them only after successful reset/free/pool retirement. It also covers
virtual-buffer End, host trampoline bypass, duplicate notifications and allocation
failure without exceptions escaping the Vulkan entry points.

The production route integration additionally records two complete sets of
transitions for all eight fully overwritten owned images, discards the first
recording, and requires the next recording to start each image from UNDEFINED.
It does not infer GPU layout from a transition that only ran on the CPU.

`install_test.py` additionally executes the production host's HookDevice, target
registration and independent lifecycle callbacks without linking the wDx12 bundle.
Detours and address lookup are fake boundaries: the cases cover separate and
aliased exports/device procedures, multiple targets, install opt-in, failed detour
registration, foreign-versus-own module addresses and successful/failed lifecycle
forwarding. These tests verify the installation policy and callbacks; real Windows
code patching and Vulkan execution require the runtime fixture.

## Allocation failure during retirement

`oom_test.py` executes the production registry under deterministic allocation failure.
It runs each lifecycle case with native standard containers and with an allocating
empty-container fixture, covering STL implementations that allocate a list sentinel
or a map during default construction. The latter is a portability fixture, not a
claim that the host compiler uses the MSVC standard library.

First use, begin/reset, pool/device retirement, free, replacement allocation and
registry destruction must retain their ownership contract without allocating. Pin
failures must be contained before mutation, and a 4,096-record retirement must drain
iteratively with reentrant deleters outside the registry lock. Wrapper and observer
cases also free under OOM, reuse the native handle in another pool after an unobserved
allocation, and verify that destroying the old pool cannot release the new recording.

Bulk-free cases require every old record to be detached before any deleter runs. A callback
recycles another handle from the freed batch; its new recording and pins must survive the
remaining notification. The helper, wDx12 hook and independent observer each exercise this case.

## Abandoned-device integration boundary

The production-body host integration additionally executes complete production
`ShutdownVk(false)` and `AfterUpscaleDeviceGone(device, true, lines)` functions.
It uses the real recording registry and real CPU-only slot leases with two
recordings retaining the route generation. Each abandonment path must invalidate
that device's registry entries and old tickets, release its leases, preserve an
unrelated device, and forget its route handles without invoking release,
destruction, device-wait or deferred GPU callbacks. Late Begin/End/Reset/Free and
pool/device notifications must remain inadmissible and CPU-only.

The fake resource boundary counts calls that would touch GPU-owned objects.
Production intentionally abandons CPU owners whose destructors could touch a
lost device. The test records those fake heap allocations before the call and
reclaims them directly **after** its assertions, solely to keep LeakSanitizer
useful. That test cleanup does not change or substitute the production
abandonment policy, and it is not counted as behavior of the abandoned route.

These cases demonstrate the after-upscale route's abandoned-device boundary.
They do not execute an actual lost device or independently exercise the Vulkan
present model's builder/transport abandonment. Host transport wiring and
metadata-only pins remain the prerequisites of this contract.
