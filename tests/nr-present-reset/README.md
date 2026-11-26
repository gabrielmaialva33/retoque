# Present camera resets (host)

`python3 tests/nr-present-reset/run.py` compiles the production pending-reset mailbox and actual Streamline 1/2
constant-hook bodies under ASan/UBSan. Covers NR disabled, false/invalid flags, forwarding, FG input isolation,
skipped captures, skipped model frames, failed/abandoned submission, concurrent producers and acknowledgment
racing a newer cut, plus seven failing mutations. Source checks cover NGX `DLSSG.Reset` (first MFG evaluation only), latching before capture
refusal, D3D12 submit acknowledgment and the existing reset-rate cause path.

`tests/nr-present-host` additionally executes the production bridge host against fakes: a cut with no captured
guides survives model refusal and abandoned recording, reaches the next submitted model frame and clears once.

`tests/nr-before-upscale` executes the actual capture producer with missing depth: the camera cut is retained,
while NR-off and after-upscale-only remain inert.

No Streamline runtime, Detours, NGX, D3D12 GPU, game or native Windows runs. See
[scene cut detection](../../docs/dlssnr/design/scene-cut-detection.md) for scope and delayed-DLSSG limitations.
