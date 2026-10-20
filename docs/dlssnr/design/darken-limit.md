# Final-composition darkening floor

Design before code, 2026-10-20. Default off; no model/feature/resource lifetime change.

## Contract

`[DlssNr] MinRatio=auto` resolves to 0 (off). Finite values clamp to [0,1];
NaN and infinities resolve to off. 0.75 means the resolve's output retains at least
75% of the original pixel's Rec.709-weighted RGB signal (0.2126 R + 0.7152 G +
0.0722 B). 1 prevents darkening; 0 preserves the previous shader exactly.
This is a floor, not a strength or a percentage of permitted darkening.

The reference is the untouched input to this composition, including signed HDR
channels, not a preceding NR pass, the encoded model input, or a previous frame.
It applies once after all model passes, colour amplification, replace decode,
highlight recovery, white-point denormalization and signed-channel restoration.
It precedes comparison overlays and output storage. Alpha and debug views are
unchanged. Strength zero and ApplyModel=false retain their existing early returns.

Let O be that original RGB, C the existing composed RGB, Y the dot product above,
and m the configured floor. If Y(O)>0 and Y(C)<m Y(O), interpolate C toward O by
`t=(m Y(O)-Y(C))/(Y(O)-Y(C))`. Otherwise return C unchanged. Thus the corrected
luminance is m Y(O), within floating-point rounding. There is no second clamp or
application at any intermediate ratio. For m=1 the correction returns O directly.
Convex interpolation handles a black result without dividing by its luminance,
keeps each component between C and O, and trades some model colour/detail for the
original. This intentionally does not preserve C's chromaticity like a scalar lift.

Nonpositive original luminance has no meaningful positive floor and is unchanged.
Nonfinite luminance leaves the existing result unchanged (this is not a new global
sanitizer); an overflowing interpolation denominator falls back to O, which obeys
the floor. No fixed luminance epsilon suppresses corrections in dark finite pixels.

## Existing bounds and routes

Contrary to the old INI comment, MaxRatio already bounds both directions in the
composed modes: a reciprocal shadow floor rising toward unity near paper white,
and a brightness ceiling suppressed near black. Preserve these equations and
correct the stale comment. MinRatio can only pull a darker result toward O; it
cannot widen those bounds. ResidualLimit also stays unchanged and can impose a
stricter bound. Replace curves 2/4 bypass both older guards but **do** get the new
final floor, after their inverse decode. Chroma boosts and recovery cannot undo it.

`gPassthrough` continues to gate every existing encode/decode and white-point
conversion. The new floor operates directly in the final buffer's units, without
another conversion: linear luminance for linear HDR, weighted encoded RGB signal
for already-tone-mapped SDR. These are deliberately not equated to display nits.
HDR10/scRGB present conversion runs outside this resolve; the floor applies in its
decoded linear working buffer, before quantization/re-encoding. Subsequent game
postprocessing, optional output stabilizer, format rounding and comparison bars
are outside this contract; this is not a guaranteed display-nit floor. A setting
change resets the stabilizer's anchor and pulse window so old settings do not persist.

D3D12 and both Vulkan composition hosts send the sanitized value. Driver-proxy
D3D12 bypasses composition, so the menu hides the control there. In Full view it
sits next to Highlight guard where that control exists, and is also offered on
present routes whose exposure panel omits the guard. Simple view is unchanged.

## Implementation and validation plan

1. Add a host suite slicing the real shader's final writeback maths; show a dark
   fixture failing before implementation. Cases: 0.75 once (not squared), colours,
   black, signed HDR, near zero, HDR scaling, passthrough, invalid config, m=1,
   stronger old limits and brighter/already-bounded pixels unchanged.
2. Append MinRatio to the C++/HLSL scalar lists; four-point INI round-trip, Full UI,
   pt-BR label/help, capture/timing provenance, log and stabilizer/pulse invalidation.
3. Add a single final-output helper/call without editing the preceding maths.
   Prove default-off via bitwise host comparisons and a source reconstruction
   against baseline e5f592de; compile both shader targets under the shared Wine lock.
4. Run the complete host tier in the requested memory-capped scope, clang-format 22
   on changed C++, diff checks, and locked Release build. No commits or deployment.

Host float32 checks/source identity do not prove GPU instruction-level equivalence,
image quality, or native Windows acceptance. Record these limits in the report.
