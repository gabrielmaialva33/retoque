# XeSS Unreal Engine recovery hardening — 2026-11-26

Scope: the existing D3D12 UE XeSS plugin recovery from upstream `7e941561`, integrated in Retoque by `b634ebeb`.
The source at `3abf29b5` ignored three `DetourTransactionCommit` results, queried the command-list device only
while creating its first copy shader, and retained View-register provenance after a 32-bit register MOV.
There was no startup switch for this plugin recovery. No module-specific README/design existed for this input;
the upstream-adjacent API comment in `XeSS_UE_Dx12.h` describes the existing route.

The small hardening changes are adapted from janblade/OptiScaler-F5-DLSSNR-Multipass `3617472e` (GPL-3.0).
Upstream files stay in `OptiScaler/inputs/`. The existing DeferredRelease calls, descriptor/copy ring, fences,
resource ownership, motion rules and NR configuration are unchanged. The donor's retirement machinery is not ported.

## Decisions

- A failed copy-hook commit clears all three original-function pointers and returns failure. A later request
  cannot enqueue work merely because the already-created shader is initialized. Resources are retained;
  there is no new GPU cleanup or retry policy.
- Both failed plugin-hook transactions clear their function pointers and associated name/offset state. `Hooked()`
  remains false, so callers cannot assume reversed depth or adjusted motion on the basis of an uninstalled hook.
  The existing checked-module policy is unchanged; this does not add retries for that module.
- Every copy request queries its command-list device. A foreign device or failed query returns no depth before
  buffer growth or request insertion. The successful temporary GetDevice reference is released once. Copies on the
  first device remain usable. This is not a redesign of global candidate discovery or multi-device support.
- Only View-register tracking is extracted to `XeSS_UE_Decode.h`. Register-to-register MOV without REX.W clears
  its destination's View bit; REX.W preserves the existing propagation rule. Displacements use memcpy to avoid
  unaligned C++ loads. This remains a deliberately partial decoder of the existing recognized forms.
- `[XeSS] UnrealDepth=auto` defaults to true. `false` skips plugin discovery/installation before module enumeration.
  It is a startup switch, documented as requiring game restart, not a live unhook operation. Declaration, parser,
  persistence, shipped INI and Config.md agree. It affects existing XeSS UE recovery even with NR disabled, as
  explicitly requested for this upstream-input fix; it adds no NR-specific hook or allocation.

## Evidence and limits

[Host suite](../../tests/xess-unreal/README.md): production decoder and hook/copy source bodies with fakes,
ASan/UBSan and failure mutations. The original source fails the regression; the changed source passes.
These checks validate CPU decisions only. The worker did not run MSVC, Wine, shader compilation, GPU suites,
games or native Windows. Actual Detours rollback, UE plugin variants, resource identity and device replacement
must be validated in a built DLL before runtime qualification. There is no image-quality/performance claim.
