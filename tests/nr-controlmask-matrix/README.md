# ControlMask matrix and evidence contracts (host only)

Run `python3 tests/nr-controlmask-matrix/run.py`. Requires Python with numpy and Pillow
(the existing loopback analyzer dependencies), plus a host C++20 compiler.
No Wine, MSVC, shader compiler, model DLL or GPU is invoked.

The compiled `dlssnr-loopback/mask_matrix.h` is the same plan the GPU fixture consumes.
The host checks its complete 184-case Cartesian product against independently enumerated
Python analysis metadata, UNORM rounding (0, 64, 128, 191, 255), channel assignments and
null-control ordering. The full GPU orchestration branch itself is compiled against recording
fakes to verify trial settings, all 5888 ordered frames and 184 releases. Its resource setter is exercised against separate typed slots,
including transitions between overloads, null cleanup and failed readback, under ASan/UBSan.

Synthetic two-pixel PPM captures exercise the real analyzer end to end: measured MAE,
channel/setter comparisons and complete output identities. Missing captures, changed input,
failed ApplyModel=0, unstable repeated/closing controls, inert unmasked output, an empty
region and missing provenance must reject the run and remove stale reports.
Runner tests reject partial/wrong-mode/duplicate completion receipts and mutually exclusive
CLI flags. The legacy 24-case receipt remains distinct from full-v1's 184 cases.

These are fixture contracts, not evidence that the model honors ControlMask, that a resource
setter works through the real NGX ABI, or that the Windows harness compiles or runs. See the
[experiment note](../../docs/dlssnr/design/controlmask-retest-20261126.md).
