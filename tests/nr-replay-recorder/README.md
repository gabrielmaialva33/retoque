# nr-replay-recorder

The pure half of the replay recorder (`docs/dlssnr/design/offline-replay-recorder.md`), compiled with the host
compiler against a scripted backend:

- **Session:**
  - idle records nothing;
  - a refusal on the first frame fails the request with its reason;
  - a pending request is not restarted.
- **Clip length:** a clip ends at 6 s of steady-clock time or at 360 frames.
- **Ring:** a full ring of six readback slots ends the clip and never skips a frame, and the frames in flight
  still make the clip.
- **Gaps:**
  - a frame discarded in the middle ends the clip at the certified frames before it, and the later frames are
    released unread;
  - a failed completion keeps its slot busy forever.
- **Clip identity:**
  - the first frame is a reset;
  - ids and time are the clip's own;
  - the present counter is the game's.
- **Refusals and manifest:**
  - the refusal rules;
  - a capture manifest (schema v2) that Python parses, with the warmup split at 1500 ms, plane descriptors, params
    and escaping.

It runs no D3D12, no copy and no writer thread: the DLL backend does, and a loopback recording does.
