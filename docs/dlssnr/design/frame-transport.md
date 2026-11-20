# One frame contract, several producer adapters

2026-11-20, based on Retoque `f4bfb52d`. **Proposal with an isolated host-tested control core. No shipped
caller, new supported API, GPU implementation, Windows run, performance measurement or product control.**
The [legacy API audit](legacy-apis.md) remains the source of the NeuRotic comparison at `beae4eea`.
The idea of legacy connections feeding an x64 renderer comes from that fork; the common-contract proposal
and code here are independently authored. No reference source, tables, strings, shaders or model data copied.

## Decision and its limits

Share the frame identity, admission, bounded ownership ledger and teardown vocabulary. Reuse one x64 model
host. Keep acquisition and API synchronization in separate adapters. This is more maintainable than three
independent helpers; **it is not evidence of being faster**. D3D10 cannot acquire D3D12 fence semantics merely
by using the same packet format. Cross-process execution can be slower than an x64 in-process connection.

The common fast-path bar is **at most one full-plane GPU copy into the shared slot, at most one output copy
out, zero CPU pixel copies, no waiting for the peer on the game's render thread or GPU queue**. A producer
that cannot meet the bar must report a distinct relay cost or refuse; never call a relay a zero-copy route.
Same-frame in-game output, no wait, arbitrary model cost and guaranteed NR on every frame cannot all hold.
For initial adapters prefer external output; in-game return remains a separate qualified scheduling contract.
Keep the existing in-process routes for games they already reach. This project does not replace them.

## 1. Resource and frame contract (proposed adapter layer)

A session binds one authenticated process identity, one adapter LUID/node (UUID/driver identity where needed),
one output policy and one resource generation. No cross-adapter copies in v1. The consumer owns the exported
allocations so a peer cannot request arbitrary imports. A generation has **two slots maximum**, each with a
colour input and a distinct processed output, plus optional depth/motion inputs; separate input/output avoids
an in-place read/write race. A single slot may be negotiated for lower memory. The window capture implementation
currently has three slots; it is not changed by this proposal.

Registration, not each frame, describes each plane: role, consumer-assigned resource ID, typed format, width,
height, mip/slice/sample count, allowed access, colour interpretation, row origin, and handle kind. Import must
query the actual resource and compare every property; caller-supplied byte sizes are not allocation authority.
V1 proposal: 2D, one mip, one array layer, one sample, full-frame extent, no sparse/protected/typeless/MSAA images;
no implicit conversion. Initially accept BGRA8_UNORM or RGBA8_UNORM SDR colour/output; RGBA16_FLOAT linear
colour is a later negotiated/qualified profile, not an HDR claim. Output must match colour extent and format.

Optional depth is R32_FLOAT normalized device depth, with a declared normal/reversed-Z convention and finite
projection parameters if the consuming NR path needs them. Optional motion is RG16_FLOAT current-to-previous
pixel displacement at the declared extent, top-left origin, no jitter baked into motion; explicit jitter pair
and reset/cut bit accompany its frame metadata. No confidence/geometry inferred from missing metadata. V1
requires same extent and valid rectangle as colour; different resolutions/subrects require a later version.
A producer may decline guides and send colour only. WGC supplies neither native depth nor native motion;
zero guides or existing SynthMotion remain consumer decisions. No estimated depth is introduced.

Only formats actually shared, sampled and written on both endpoints are admitted. Unsupported depth views,
sRGB encoding, MSAA resolve, bottom-left GL rows and motion-Y conversion are adapter refusal reasons unless
an explicit GPU conversion profile is qualified. A conversion is counted as an additional full-plane pass,
even if it is not named CopyResource. An API extension string is insufficient admission evidence.

Every submission carries `(session, generation, serial, slot)` and an input-ready fence value. Serial is
strictly increasing across accepted submissions in a generation, never recycled even on a dropped frame.
The full adapter also retains the source capture timestamp, geometry/crop epoch, original game-frame lease,
real-versus-generated provenance and temporal reset facts; those metadata fields are **not encoded in this
first control-core wire subset**. A backend must not supply guides or return in-game output until that schema
is designed and tested. Never let a session nonce stand in for an HWND, COM pointer or resource handle.

The reply refers to the **same identity** and the output-ready value; it grants a read lease of that slot's
processed colour, not ownership of arbitrary memory. Output is consumed only under the registered output
policy. There is no model path, INI text, shader bytecode or filesystem path in a frame message.

### Ownership and completion

Two shared completion primitives **per generation**, one producer-owned timeline P and one consumer-owned
timeline C, serve all slots. Only their designated queue owners signal them, monotonically in submission order.
They are not one pair per plane or per frame. Capture and final producer-use signals share P, so their values
are assigned in actual submission order, independently of frame serials; never preassign `2*serial` and then
signal later captures over earlier unscheduled releases. A relay may need additional local queries; these
are counted explicitly and cannot be hidden by the common contract.

| Transition | Access allowed / evidence required |
|---|---|
| Free → Submitted (`Submit`) | Reserve before recording capture. Producer writes only that slot's inputs, then signals P=p. Consumer cannot read yet. Failed submission aborts; no timed reuse |
| Submitted → Processing (`Begin`) | Local observation of registered P ≥ p, live device, and oldest pending serial. Producer stops accessing inputs/output; consumer reads inputs and writes output. Only one model recording at a time |
| Processing → Output (`Return`) | Consumer signals C=c after **all** NR/composition work and restores agreed shared state; local observation C ≥ c. Producer can now read/copy/present output; consumer must not touch that slot |
| Output → Free (`Release`) | Producer signals new P=r after its **last** output use, including final copyback or external presentation read; local observation P ≥ r. Both input and output leases retire together |
| Any occupied state → Abandoned (`Abort`) | Peer loss, timeout, protocol fault or device loss. No new admission/reuse; no transition from Abandoned back to Free |

For external output owned by the consumer, C must also cover the presenter's last read of output. Return is
then a completion acknowledgement: the producer does not read output and signals P for Release only after
observing C. In return-to-game mode C covers NR/composition, and P covers the producer's later copyback.
The registration fixes this output policy; it cannot switch mid-frame. Same-process WGC can perform both
roles locally, but must preserve the same completion order. This extra acknowledgement can hold a slot
longer than a specialized capture ring, another cost to measure before replacing today's host.

A CPU Return/Release packet, a successful queue submission, an elapsed timer, a process exit or `UINT64_MAX`
from a D3D fence is not completion. The header models the observation gate; it cannot prove which fence was
read. The platform owner must bind each expected value to the real last-use submission, reject wrong queue
identity and failed signals, and observe independently. A consumer cannot choose an arbitrary old completed
value for its reply. Inputs stay immutable through the full lease; this costs memory but reduces lifetime cases.

D3D12 producer/consumer release shared resources to the agreed COMMON state before signalling; the receiving
API performs its own transitions, UAV/cache barriers and ownership acquire as required. Vulkan external queue
family transfers and compatible layouts must be explicit; GL semaphore barriers must name the touched textures
and layouts. A shared handle alone is no execution or memory dependency. Proposed worker polling observes
ready fences before scheduling work, rather than putting an untrusted future wait on a game's queue. This
can add wakeup latency compared with GPU-only chaining; qualification must measure it.

### Invariants

1. **Unreachable by default:** this core is not included by any shipped source/build. No config or UI key exists.
2. **One identity, one lease:** a slot is never reused while Submitted, Processing, Output or Abandoned. Exact
   session/generation/serial/slot matching applies to every transition, including returns and releases.
3. **Evidence belongs to its operation:** only the registered device/queue's observed completion authorizes the
   next owner. No CPU fake signal, wall-clock shortcut or cross-generation fence comparison.
4. **Bounded everywhere:** at most two leases, one model recording and one 56-byte packet accumulator in the core.
   The future channel additionally caps outstanding control packets, handles, memory and launch attempts.
5. **No hidden stalls:** a full ring declines before touching the source. No busy-loop, blocking pipe IO, peer
   wait or pending peer fence wait on the game's render/present thread or GPU queue.
6. **No wrong-frame substitution:** return-to-game requires the still-owned original frame lease. A result for
   frame n never overwrites n+1. External output may display a labelled fresh delayed frame; stale output hides.
7. **Fail closed, retain uncertain work:** timeout/death/device loss stops the generation. Resources with possible
   GPU references remain retained until separately proved drained, or abandoned until process teardown.
8. **No silent downgrade:** missing required capability/format/adapter/identity refuses. No automatic CPU fallback,
   guide estimation, cross-adapter transport, changed default or unqualified “supported” badge.

## 2. What the host-tested core actually implements

[Protocol.h](../../../retoque/frame_transport/Protocol.h) contains no Windows headers or handles. It is a
single-threaded decision ledger and codecs, not the platform channel or a second NR implementation.
[Module obligations](../../../retoque/frame_transport/README.md) specify the boundary the adapter must enforce.

All integer fields are explicit little endian, no packed struct serialization, `size_t`, pointer or ABI padding:

| Byte offset | Header (16 bytes) |
|---|---|
| 0 | u32 magic `0x31544652` (RFT1) |
| 4 | u16 exact version 1; different versions refuse rather than guessing a compatible prefix |
| 6 | u16 kind: 1 Hello, 2 Submit, 3 Return, 4 Release |
| 8 | u32 total packet bytes: 40 Hello, 56 frame |
| 12 | u32 reserved, must be zero |

Hello payload at 16: u64 offered bits, u64 required bits, u32 slot count, u32 reserved zero. SharedGpu=1 and
Timeline=2 are mandatory; Depth=4 and Motion=8 optional. The agreement intersects known offered bits and takes
the smaller valid slot count (1..2); it must contain both peers' required bits and mandatory bits. Unknown
optional bits are ignored; unknown required bits refuse. This does not negotiate actual resource import support.

Frame payload at 16: u64 session, u64 generation, u64 serial, u32 slot, u32 reserved zero, u64 fence value.
P is used by Submit and Release, C by Return. Hello occurs once per session. Session/generation setup here is
a local trusted operation after the not-yet-implemented bootstrap/registration, not a network command.
The u64 session label is correlation only; bootstrap authentication uses a separate 128-bit random secret.

`Reader` accepts split/coalesced byte-stream input, consumes only one packet per call and retains at most 56
bytes. It refuses oversize at the header, malformed kind/version/reserved fields and trailing bytes in Decode;
partial EOF is Truncated. Framing failure is sticky. The caller caps any unconsumed suffix and applies a read
deadline. No allocation based on remote lengths. Enum refusals distinguish protocol, capability, capacity,
identity, ordering, completion, busy, peer-loss, timeout and device-loss cases; a future adapter localizes them.

`Session` rejects saturation before mutation, duplicate/stale serials, double return/release, mismatched
identity, fence rewind/sentinel and occupied-generation replacement. Begin is oldest-first and serializes the
model; incomplete completion leaves ownership unchanged. Return/Release require an observed value. `Abort`
latches the first cause and marks occupied slots Abandoned. Device-loss observation aborts automatically;
other runtime faults must call Abort. Resize requires zero outstanding leases and a strictly newer generation;
it resets timeline counters only because the adapter must create **new fence objects** for that generation.

Tests in [frame-transport](../../../tests/frame-transport/README.md) prove these decisions under synthetic
observations, fragmented packets and malformed inputs. Ten mutants individually fail the intended diagnostic,
including all six requested failure classes. No clock, GPU object, texture descriptor/import, authentication,
OS transport, recording/copyback or real producer is implemented. Registration must get its own versioned codec,
allocation budget tests and native fixtures before connecting this subset to any process.

## 3. Copies, traffic and latency: arithmetic, not measurements

Count **full-plane logical copies outside NR internals**, not command count: batching four planes is still four
plane copies. NR encoding, model intermediates, composition, guide resampling, driver compression, WGC/DWM and
swapchain work are additional. Zero-copy sharing removes only explicit transport copies, not NR memory traffic.

| Producer/output candidate | Ingress / egress colour copies | Why / where zero is possible |
|---|---|---|
| D3D12, including x86 → x64 | 1 / 1 | Private game image → shared input; shared output → exact leased game image. 0 ingress only if the producer already renders into the registered exportable image; 0 egress only if an owned presenter can directly consume output. Ordinary swapchain buffers are not assumed exportable |
| D3D11 with NT sharing/fences | 1 / 1 | Same-adapter compatible textures opened directly by D3D12. No extra private normalization pool. 0 ingress if a registered shared target is already the source. Modern fence interfaces required |
| OpenGL EXT interop, native Windows | 1 / 1 | Blit to imported shared input, and imported output back to exact framebuffer. 0 if rendering/presentation genuinely uses the imported allocation. Origin/format conversion or resolves add passes and need a separate profile |
| Vulkan external-memory producer | 1 / 1 | Import/export a compatible allocation plus D3D12-fence-compatible external semaphore. 0 with a suitable pre-owned shared render target; swapchain/export/tiling support must be queried |
| WGC → existing host | 1 / 1 to host backbuffer, **no return to game** | Current Capture→Transport and Output copy. WGC/compositor work excluded. Proposed direct offscreen NR can remove the intermediate backbuffer use but final presentation may still need one copy. No promised zero ingress from WGC |
| D3D10 with legacy→NT relay | **2 / 2**, does not meet fast-path bar | Game→legacy shared image, relay→NT image; reverse on output. Alias-compatible opening without relay copy would reduce to 1/1, but is not a Windows D3D10/D3D12 contract assumed here. Decline fast-path admission until proved; prefer capture |

Both copies in today's WGC path precede NR: “egress” in that row means leaving the shared transport for
the host backbuffer, not exporting an already processed result. The proposed common consumer must not
confuse that ordering with return-to-game processing.

Consumer NR must operate directly on the registered surfaces or a qualified composition target. The present-only
`RetoqueWindow_*` seam cannot currently do this offscreen. Reusing it through a hidden backbuffer plus shared
output would introduce extra copies; do not ship that as meeting the table. Internal keep/proxy/model copies
remain accounted in NR timing even after the future offscreen seam is implemented.

For width W, height H, B bytes/pixel: one payload = W×H×B. One copy logically moves that payload and normally
reads **and** writes it (2× traffic before compression/cache). Colour round trip with one copy each way = 2×
payload, approximately 4×payload DRAM read+write. Numbers below are arithmetic for tight planes; MiB = 2^20 bytes:

| Extent | RGBA8 one-way bytes (MiB) | RGBA8 round-trip copied bytes (MiB) | Approx. read+write MiB | RGBA16F round-trip MiB |
|---|---:|---:|---:|---:|
| 1920×1080 | 8,294,400 (7.91) | 16,588,800 (15.82) | 31.64 | 31.64 |
| 2560×1440 | 14,745,600 (14.06) | 29,491,200 (28.13) | 56.25 | 56.25 |
| 3840×2160 | 33,177,600 (31.64) | 66,355,200 (63.28) | 126.56 | 126.56 |

With R32 depth and RG16F motion copied in once (4+4 bytes/pixel), add 8×W×H: **15.82 / 28.13 / 63.28 MiB**
copied per frame at those resolutions, doubling the RGBA8 colour-only round-trip total. Two RGBA8 input/output
slots consume 4×W×H×4 = **31.64 / 56.25 / 126.56 MiB** before guides/alignment/model memory. Two extra guide
planes in each slot double that. A 2/2 relay doubles colour transport traffic and requires extra allocations.
At 60 fps, 4K RGBA8 1/1 copies imply 3.71 GiB/s copied payload and roughly 7.42 GiB/s GPU read+write; not a
bandwidth benchmark. CPU transport's two readbacks plus two uploads would add 126.56 MiB/frame crossing the
GPU/CPU boundary at 4K before IPC, about 7.42 GiB/s at 60 fps. No GPU path here permits CPU pixel transfer.

### Added latency in frames

Let T be the game's frame interval, M the measured model+composition time, I/O the measured ingress/egress
work and W the actual worker/IPC scheduling time. **Arithmetic**, not measured prediction:
`D = (I + M + O + W) / T` frame intervals, plus any presentation/capture queue age. At 60 Hz T=16.67 ms;
a hypothetical 8 ms chain is 0.48 frames before scheduling/display, not “zero latency”.

| Producer | Added queue age / contract |
|---|---|
| D3D12, D3D11 or Windows GL, x86 or x64 | Same-frame return adds 0 whole queued source frames but D elapsed intervals on the critical path. Without an owned bounded presentation scheduler, never wait for the peer: use already-complete output for the exact frame or bypass NR. A deliberate one-frame pipeline adds 1+D source intervals and requires holding/presenting that original frame, never inserting it into the next frame |
| Vulkan external memory | Same 0-or-1 policy, plus any external ownership-transfer scheduling in I/O. No latency advantage inferred from Vulkan underneath DXVK |
| D3D10 relay | Same policy plus game-query/relay completion observation in I/O/W. Likely worse latency; no numeric improvement claimed. If render-thread cooperation would require blocking drains, refuse this fast profile |
| WGC of any API/bitness | Source capture/compositor age A/T plus D and output-compositor delay. Budget initial evaluation around 1–2 added refresh intervals **as a hypothesis**, not a minimum/maximum promise. Keep newest unsubmitted capture only; an already leased slot is never evicted |
| Native Linux GL/Vulkan → Wine | Unknown until FD/Win32 bridging and presenter qualification; no numeric promise or supported adapter |

Two slots bound memory, **not time**: a hung GPU can hold one forever. Proposed worker cutoff: suppress output
older than two source intervals for external presentation (hide, reset temporal history); stop a generation
on 250 ms without progress. Both are untested budgets to qualify, not a drain. Same-frame return's admission
is nonblocking; no “bounded 250 ms” game-thread wait. A future display scheduler needs its own explicit latency
budget and owner acceptance. No automatic endless restart after a stalled generation.

Against NeuRotic's audited connections: eliminating private normalization/relay pools can reduce a 2/2 route
to 1/1 **only** for direct-compatible D3D11/GL/Vulkan images and a new offscreen NR seam. Moving query/fence
observation to a worker avoids the audited blocking capture/finish drains, but can increase image age and drop
NR opportunities. D3D10 still needs relay synchronization; x64 IPC can cost more than NeuRotic's local
`ProcessTextures` path. GPU NRH2 already uses shared memory, bounded leases and two fences, so those are parity,
not unique speed improvements. The largest arithmetic saving is against its optional CPU pixel transport,
which our GPU-only proposal refuses rather than trying to outperform. Source comparison is not an A/B run.

## 4. What the platforms actually expose

Sources checked 2026-11-20. API specifications show possible operations; source snapshots show implemented
branches. Neither establishes the installed driver/Proton build, interoperability or game coverage.

| Boundary | Available contract and refusal |
|---|---|
| D3D11/D3D12 NT handles | D3D11.1 `CreateSharedHandle`/`OpenSharedResource1` use NT-handle textures; D3D12 `CreateSharedHandle`/`OpenSharedHandle` share committed resources/heaps/fences. Query compatible format/flags and same adapter; D3D11 `ID3D11Device5`/context4 fence interfaces are a separate requirement. [Microsoft NT import](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11device1-opensharedresource1), [shared heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps) |
| D3D10 legacy handles | `IDXGIResource::GetSharedHandle` yields a legacy handle, **not** an NT handle: do not DuplicateHandle/CloseHandle it. D3D10/11 resource sharing is documented; D3D12 uses NT sharing, so the conservative adapter opens legacy in D3D11 and copies into a separately created NT resource. D3D10 queries/flushes establish completion; it lacks this D3D12 timeline interface. Exact formats may refuse. [Legacy handle rules](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiresource-getsharedhandle), [D3D10/11 opening](https://learn.microsoft.com/en-us/windows/desktop/api/d3d11/nf-d3d11-id3d11device-opensharedresource), [DirectX shared-resource model](https://microsoft.github.io/DirectX-Specs/d3d/ResourceHeaps.html) |
| Windows OpenGL | Require EXT_memory_object + EXT_memory_object_win32 and EXT_semaphore + EXT_semaphore_win32. The spec defines D3D12 resource and fence import kinds and fence-value parameters. Requires matching physical device, format/layout and current-context rules; GL availability alone is insufficient. [Khronos EXT_external_objects_win32](https://github.com/KhronosGroup/OpenGL-Registry/blob/main/extensions/EXT/EXT_external_objects_win32.txt) |
| Vulkan | External-memory capability queries must report import/export for the exact image/format/usage and handle kind; honor dedicated-allocation requirements. Win32 NT D3D11 texture/D3D12 resource or heap types differ from OPAQUE_FD/DMA_BUF. External semaphore D3D12_FENCE represents a D3D11/12 fence, with matching device/driver identity restrictions. [Memory handle types](https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalMemoryHandleTypeFlagBits.html), [semaphore handle types](https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalSemaphoreHandleTypeFlagBits.html) |
| x86 ↔ x64 | Duplicate NT handles into the **target's** handle table, then transmit the target-sized value as a checked fixed-width token; never cast/send a COM pointer or the original numeric handle. Microsoft explicitly permits 32/64-bit DuplicateHandle. The broker maintains typed tokens and owns closing only its duplicates; no truncating pointers. [DuplicateHandle](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-duplicatehandle) |

### Wine/Proton evidence, not blanket support

- Wine's [win32u Vulkan implementation](https://github.com/wine-mirror/wine/blob/c053869a6f177468271e756069652d744ab894bf/dlls/win32u/vulkan.c#L1096)
  has NT/KMT external-memory branches including D3D11 texture and D3D12 heap/resource types. Its
  [semaphore import](https://github.com/wine-mirror/wine/blob/c053869a6f177468271e756069652d744ab894bf/dlls/win32u/vulkan.c#L2631)
  handles D3D12_FENCE. That is source-level plumbing, not proof of cross-process synchronization in our prefix.
- [vkd3d-proton Create/OpenSharedHandle](https://github.com/HansKristian-Work/vkd3d-proton/blob/b443c7935b9d591f06259c96c9e58c762286752c/libs/vkd3d/device.c#L6709)
  implements Windows shared-resource/fence paths. [DXVK D3D11Device](https://github.com/doitsujin/dxvk/blob/master/src/d3d11/d3d11_device.cpp#L1408)
  implements NT-resource and shared-fence opening, including D3DKMT/legacy Proton branches. The latter URL is
  a moving source reference, inspected this date. Actual pairing/version and D3D10 format interop remain unknown.
- Crucial limitation: Wine's [GL import_memory/import_semaphore](https://github.com/wine-mirror/wine/blob/930d6bc53d941ed46a8a468ef28dd2e89b986a39/dlls/win32u/opengl.c#L3517)
  accept **OPAQUE_WIN32 and OPAQUE_WIN32_KMT** and map FD_OPAQUE to GL FD imports; their switches return
  GL_INVALID_ENUM for other types, including the proposed direct D3D12 resource/fence kinds. Thus this snapshot
  does **not** implement the native-Windows GL/D3D12 profile as specified here. An opaque-memory/semaphore bridge
  would be a different tested profile, not a cast of one handle enum to another. Proton patches may differ;
  installed-build behavior and fence-value translation are unknown. Missing profile must refuse.
- Windows capture remains Windows-only per [NR Anything](nr-anything.md). Native Linux GL/Wayland uses native
  buffers/FDs and compositor permissions, not a Windows NT handle channel. No cross-prefix shared-object or
  native-to-Wine equivalence is assumed. No WGC, DComp, native GL or Wine GPU API was executed for this task.

## 5. Failure, resize and teardown

| Event | Required behavior (platform proposal unless noted) |
|---|---|
| Peer disconnect/crash while a lease is occupied | Abort, cease admission and hide external output. Retain every possibly referenced allocation/import and source lease. Do not infer completion from EOF/process death; a submitted GPU command can outlive its sender |
| Partial/oversized/version-invalid message | Stop parsing that session, close its control path, Abort if opened. Reader bounds/latch are tested; actual pipe closing and OS cleanup are not implemented |
| Deadline/no progress | Stop accepting, suppress stale output, Abort. Do not signal the peer's fence or return slots. A bounded supervisor can end only its own helper; the surviving game adapter must keep uncertain references, disable NR and require restart |
| Device removed/lost | Sentinel is a failure, never a completed maximum fence. Abandon old GPU objects; no ordinary release on the assumption that removal drained them. Core latch tested, native teardown not |
| Resize/format/crop/context/adapter change | Stop submission and temporal history. Drain all known last uses on a worker with a bounded wait; only then retire and re-register with a newer generation. Otherwise Abort and keep old objects. LUID/format changes are not in-place slot edits |
| Stale frame/output | No copyback to a newer frame. External presenter hides/drops it. Complete the normal no-copy release after proving no output use remains, or abort on uncertain work. An occupied queue entry is not evicted to admit a fresh one |
| Normal stop | Disable acquisition, finish/retire outstanding leases and producer local uses, drain known queues, then close imports and duplicated NT handles. Never free recorded-but-unsubmitted or replayable resources without settling their recording ownership |

A refused Submit happens before capture touches a slot, so the ordinary game path continues. After capture
has been queued, a failure cannot “roll back” by freeing the slot. After copyback has started, report a fault
and preserve references; do not pretend to undo writes. The real adapter needs states for recording/submitted
copyback and queue ownership beyond this logical ledger. GL cleanup must run under the correct live context;
otherwise retain, not issue deletes under a different context. No automatic new generations/helpers after
uncertainty; bounded retained memory is achieved by stopping, not by eventually freeing on a timer.

## 6. Channel security and limits

Proposed bootstrap: the app starts one known sibling x64 host with a private inherited bootstrap endpoint,
explicit handle list and a random 128-bit secret; no secret on argv or in logs. A local named pipe uses an
explicit logon-SID DACL, `PIPE_REJECT_REMOTE_CLIENTS`, first-instance protection and verified client/server PID,
creation time and session identity. Same user alone is not authentication. Pin the launched process handle and
validate the secret before exchanging resource tokens. No arbitrary PID from a packet is used with OpenProcess.
Named-pipe security/access mechanisms are described by [Microsoft](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights);
the bootstrap policy here is our unimplemented design, not proof of isolation.

Consumer allocates only typed, bounded resources on the selected adapter and brokers explicit minimal-access
handle duplicates. Refuse foreign handle types, arbitrary object names, unexpected descriptors, oversized
extents, replayed registrations, aliases between input/output, unknown required bits and wrong identity before
import/recording. Proposed caps: two slots, eight plane handles plus two fence handles per generation, at most
384 MiB of transport allocations, 4096×4096 extent, one live generation and no queued reconnect. Use actual
allocation size/alignment and remaining budget, not only W×H×B. Model memory gets a separate admission budget.
No filename/shader/model/config command from the producer. No image persistence or network listener.

The future IO adapter caps control backlog to 16 packets and work per tick, rejects floods and incomplete-read
deadlines, and never allocates based on untrusted lengths. The core's 56-byte reader and two-slot ledger are
proven bounds; those OS/resource/rate limits are **not implemented**. Logs contain reason and opaque frame
identity, never titles, image data, raw handles, secrets or user paths.

An authenticated but compromised producer can still scribble into shared writable allocations, signal a
shared fence dishonestly or submit GPU work that stalls. Handle ACLs and numeric validation **cannot** make
shared GPU execution safe against a malicious same-user process. The trust boundary is a cooperatively
behaving installed adapter, in an unprivileged isolated helper with no command/file execution surface.
Untrusted adapters must be refused, not “sandboxed” by this ledger. No indefinite GPU wait on peer-chosen
values; timeout terminates use and retains uncertain resources. Driver isolation/device-wide denial of service
remains a risk, and preventing it would require a different copying/isolation design and performance budget.

## 7. Migration through the existing window host (all later steps proposed)

1. **This slice:** design plus standalone control header/tests only. No vcxproj, export, menu, app, INI or existing
   host change. Tests establish protocol decisions; capabilities remain unchanged.
2. **Private OS transport fixture:** x86 synthetic producer ↔ x64 helper, no model, no game and no existing CLI
   change. Implement bootstrap/registration/descriptor codec and actual fences. Verify changing pixel patterns,
   split/flooded channel, wrong identity/adapter, peer death at each submission stage and memory caps. Windows
   first; a pinned same-prefix Proton run is an independent gate. Check 32/64-bit handles without using pointers.
3. **Private headless NR seam:** add separately versioned, gated owned-resource record/submit/completion exports
   next to `DlssNr_WindowHost`, rather than changing v1 Present's ABI. Audit every global/history/reset/guide
   assumption and bind the exact last-use fence, including internal model queues. Preserve Version/Begin/Present/End,
   one owner and wrapped-swapchain refusal. The current seam uses local COM pointers, calls RunPresentPass on a
   swapchain and End frees nothing; it is not an IPC or arbitrary-image renderer today. No visible path yet.
4. **Capture adapter shadow fixture:** let WGC in the same process describe a frame using the contract without
   starting another process. Keep production Main/Transport/Output unchanged until pixel/geometry/lifetime
   equivalence is demonstrated. Reuse existing capture and presenter policy; do not create a second model host.
   Measure current two-copy path against offscreen candidate before replacing anything.
5. **One producer at a time:** D3D11 NT or D3D12 first, x86 transport packaging separately, GL exact format/origin
   probe next, D3D10 relay last only if demand survives capture qualification. Each uses the same consumer,
   with source-specific state restoration and provenance fixtures. Legacy APIs do not become “thin” until this
   work is proven. Linux-native remains a separate adapter project with an early stop gate.
6. **Qualification before reachability:** native Windows RTX 3060 and Proton RTX 4090 separately; transport-only
   pattern/hash oracles, NR ApplyModel off exact pixels, NR on moving scenes/HUD, resize/alt-tab/minimize, device
   and peer fault injection, bounded resource lifetime and external latency measurements. Only then propose an
   explicit experimental user path and app text. Keep old preview/overlay path as rollback until equivalent.

A same-process capture producer may use the ledger without serializing packets; that avoids unnecessary IPC.
The logical contract is common, the process boundary is not mandatory. An external producer cannot call the
old seam with pointers from its process. No current caller is switched merely because the host suite is green.

## 8. Rejected alternatives and open qualification choices

- **Three separate end-to-end paths:** easier isolated spikes, but duplicates framing/authentication, model
  setup, lease/quarantine logic and tests. Retain separate API adapters/fixtures, not separate consumer protocols.
- **One universal GPU handle representation:** impossible across legacy KMT, NT handles and POSIX FDs. Keep
  typed broker tokens and explicit profiles; reject mismatches. Do not truncate/cast to “make it work”.
- **Claim D3D10 is 1/1 by hiding the relay:** rejected. The conservative path is 2/2; alias/no-copy is a test gate.
- **Return the latest completed result to the next game frame:** rejected for identity/HUD/input correctness.
  Async overlay is a different output policy with visible age, not an in-game shortcut.
- **Wait on the game's GPU queue for a remote fence:** rejected for a peer that can disappear. A CPU timeout
  cannot unschedule that GPU wait; the game can remain stuck after the helper dies.
- **CPU fallback:** adds bus/IPC copies and another security/lifetime surface; refuse until an explicitly desired
  low-resolution case justifies its own design. No fallback on uncertain GPU work.
- **Rewrite window host immediately / hidden swapchain as offscreen NR:** respectively regresses its existing
  contracts or adds copies. A separately gated seam/fixture must prove the candidate before moving production.
- **Unbounded pipelining / restart until success:** rejected for latency, temporal history and retained VRAM.
- **Treat source/API support as Proton qualification:** rejected; Wine GL's direct D3D12 import gap is concrete.

Untested judgments: two slots, optional guide profile, direct offscreen composition feasibility, 384 MiB budget,
staleness/progress thresholds, adapter ordering and output preference. The owner still chooses real titles,
acceptable in-game versus external output, latency budget and target platform. These choices do not block this
unwired prototype, and none is silently promoted to a product default. No promise of covering every game or
outperforming NeuRotic is supported until the per-route measurements above exist.
