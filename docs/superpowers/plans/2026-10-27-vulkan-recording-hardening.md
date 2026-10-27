# Vulkan recording hardening implementation plan

Design: docs/dlssnr/design/vulkan-recording-lifetime.md. Work stays on fix/rendering-hardening-20261027.

- [x] Reproduce modulo descriptor/UBO reuse and premature generation retirement with behavioral tests of production integration.
- [x] Add and test the device/pool/recording-epoch registry and fixed slot leases.
- [x] Notify the registry only after successful Vulkan lifecycle operations; cover free/pool/device invalidation and host-owned recordings.
- [x] Reserve composition slots before any frame commands and bind the reserved descriptor/UBO slice per dispatch.
- [x] Pin the after-upscale generation before feature-create/frame recording.
- [x] Gate resize, scaler changes, release and hand-back on recording quiescence, without renewing pins while waiting.
- [x] Preserve abandoned-device behavior; prove no GPU callbacks occur after abandonment.
- [x] Run focused behavioral and available Vulkan fixture validation, full host tests and Release build.
- [x] Independent review and a separate implementation commit; record actual GPU/hardware limits. Implementation: `72c9ecd0`; verification record: `docs/superpowers/reviews/2026-10-27-rendering-hardening.md`.
