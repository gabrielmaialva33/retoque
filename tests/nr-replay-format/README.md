# nr-replay-format (host)

Run `python3 tests/nr-replay-format/run.py`. Standard-library Python only.

Generated raw f16/f32/u8 and PFM fixtures exercise dimensions, row padding, endian/scale,
bottom-up orientation and finite values. Manifest mutations cover frame/clock identity,
completion, exposure, unsafe paths/symlinks, hashes, coverage and required metadata.
Color jitter declarations and trial IDs are required; skipped entries need an unscored
capture frame, a reason, zero completed passes and certified output/timing.
The suite also exercises the runner's stable-ID → explicit path mapping, duplicate paths,
escaped paths, orphan directories and declared toolchain requirements.

No model, GPU, capture session or image-quality acceptance. Temporary fixtures are removed.
The wire contract is [offline-replay.md](../../docs/dlssnr/design/offline-replay.md).
