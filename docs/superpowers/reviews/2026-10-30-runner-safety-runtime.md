# Runner safety: bounded runtime evidence — 2026-10-30

The frozen runner-safety source snapshot was exercised under the shared fixture lease with a
private copy of the MSVC toolchain and compiler prefix. No game was launched or modified.
The production DLL used was the Vulkan after-3 candidate, SHA-256
`a3003b6a8e1f74810274b6cd047923edafbe64a823b2d41d77ebbfd1ba0461be`;
source base `5faa8dce` was dirty, so the source hashes in the receipt identify the actual
runner snapshot. The core was `dabe18b0dc559750113b589d06f672141c8c49a2a571e0a5a7ca7b3e584a7ac3`,
the NR model `6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927`,
and the SR model `3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983`.
The toolchain and prefix copies were made after no-game preflight, under the global lease.

- The legacy D3D12 sweep compiled using `run_watched` and the owned-process supervisor,
  then passed all seven steps: 4,519 successful evaluations, no refused evaluation,
  five feature creations, checked teardown and Shutdown1 result `0x00000001`, exit 0.
  The legacy runner normally chooses its kit core; the invocation staged the approved
  core symlink immediately before Proton execution while holding the same lease.
- The normal `--replay-nr` entrypoint compiled under its own outer lease and exited 0.
  Six variants each replayed 256 frames: all 1,536 pixel hashes and decisions matched
  the retained baseline. Five repeated every-frame calibration runs and the perceptual
  judge were disabled for this bounded identity comparison; quality was not assessed.
- Both runs used RTX 4090, Linux driver 615.78.08 and Proton Experimental. This proves
  those harness paths on this runtime, not native Windows, games, frame rate or general
  image quality. Host fault injection covers rejection/unknown-fence branches; this
  successful GPU run does not claim those failures were induced on a GPU.

Exact commands, logs, binary and source hashes, exit codes, per-step counts, replay
reports, manifests and outputs are retained under `<evidence-root>/runners/`; the
summary is `<evidence-root>/runners/summary.json`. The combined host run independently
reported 100 PASS with intact sanitizers (run ID `c5b80504802b4497beea8c2da71042bb`).
