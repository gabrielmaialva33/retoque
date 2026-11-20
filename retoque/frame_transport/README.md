# Experimental GPU frame transport control core

This folder is **unwired**: no DLL, window host, app or build project includes it.
[Design and evidence boundary](../../docs/dlssnr/design/frame-transport.md).

`Protocol.h` is C++20 and standard-library only. It contains explicit little-endian codecs (no struct ABI),
a 56-byte bounded stream reader, symmetric capability negotiation, and a single-owner-thread two-slot ledger.
`tests/frame-transport` compiles this exact header under ASan/UBSan and rejects ten targeted mutants.
There are no GPU objects, handles, platform calls, threads, waits, allocations, IO or destructors with side effects.

The wire subset is Hello, Submit, Return and Release. Registration, authentication, descriptors, deadline
scheduling and producer adapters are **not implemented**. This is a control-contract prototype, not an IPC server.
Do not add it to a shipped project until the design's platform and security gates are met.

## Caller obligations before any future integration

- Authenticate and bind one peer/session; negotiate once; verify/import a bounded resource generation independently.
- Serialize every call on one owner thread. `Submit` reserves the slot **before** capture is recorded; failed
  recording/submission after reservation aborts the session. There is no speculative cancellation/recycle API.
- Feed `Begin`, `Return` and `Release` only local fence observations tied to the actual registered queue and
  submitted operation. A decoded Frame contains a requested fence value, never proof. Keep one monotonically
  increasing producer timeline (capture and final-use signals in submission order), and one consumer timeline.
- `Begin` waits for nothing; it refuses an incomplete capture. It admits only the oldest submitted frame, with
  at most one Processing slot. `Return` publishes only after consumer completion. `Release` frees the logical
  lease only after the producer's last read/copyback completed. The adapter must track a submitted return's
  exact fence while the slot remains Processing; peer messages cannot pick a previously completed unrelated value.
- Treat protocol errors/peer death/deadline expiry as `Abort`; it retains occupied slots as Abandoned. An observed
  device-loss sentinel latches automatically. `Incomplete`, `Busy` and `Saturated` are nonblocking decisions,
  not permission to free, retry endlessly or overwrite. The core itself has no clock or automatic deadlines.
- `Resize` is permitted only when all leases completed; separately prove all adapter queues/recordings drained,
  retire the old resources and create new fences for the strictly newer generation before accepting new frames.
- `Reader::Feed` returns at most one message and a consumed count; keep the unread suffix bounded. A partial
  packet at EOF is Truncated. Framing errors stick. Clean EOF still means peer gone to the session owner.

Resource descriptors and completion evidence cannot be verified by this header. It must never be presented
as a GPU lifetime implementation or hostile-process isolation boundary. No reference-project source was copied.
