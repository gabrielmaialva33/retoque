# Present-guide runtime evidence — 2026-10-29

Status: **integration blocked**. The D3D12 present-guide case and the after-upscale replay control have
bounded before/after evidence. The DLL-backed D3D11 bridge run is **FAIL** in both binaries because
`D3D11_Shutdown1` crashes during cleanup. The final native host tier passed on stable non-Markdown sources;
documentation and privacy checks remain to be repeated after the documentation changes.
No commit, merge, push, deployment, game run or native Windows qualification is implied.

This record follows the earlier [source/build review](2026-10-29-present-guides-extraction.md). Paths below
are relative to a user-local `<evidence-root>`; they are not repository artifacts. Do not copy raw logs,
binaries, captures or local paths into Git.

## Identities and environment

| Item | SHA-256 |
|---|---|
| Before OptiScaler DLL | `687cdbe2adf574321dfb3f03fe5e4964741bf8a2b4cd0a5f5408314006d03f04` |
| After OptiScaler DLL | `258ebbab84f6a8558750410046da781bb773b12225d53cfb98a443c019ccde40` |
| Before forwarder | `68f78e20ebf2e150647c66c584631a7ddcb86c4346566260ae0ecbaba178c8ad` |
| After forwarder | `6b19e45a5e736bfbbf1e735d33855ced8b532e529c85f22220c304c8695be0b6` |
| Tested `_nvngx.dll` core | `dabe18b0dc559750113b589d06f672141c8c49a2a571e0a5a7ca7b3e584a7ac3` |
| NR model | `6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927` |
| D3D11 instrumented harness | `6f4634e367d0586aa678d3e1ebe686b25dcf2df74d2842483fbd60563da978d8` |

The before build carries stamp `da751da7`; its exact source-to-binary provenance is not established and it
is not relabeled as a clean base build. The after DLL was built from base `dd2a5891` plus the uncommitted
extraction; HEAD alone does not identify that diff. Production C++ has not changed since the candidate build.
The bounded runs used Proton Experimental, NR model 310.8.2 and an RTX 4090. The core examined below has
version `32.0.16.1752`; findings are limited to this exact core hash and observed environment.

## After-upscale replay control

The earlier review correctly recorded the old frozen output baselines as missing at that time. New before-DLL
output baselines were subsequently recreated and retained; they are not those missing historical baselines.
`runtime/progress.json` records the initial six-variant, 256-frame control. The extended result is in
`runtime/extended/progress.json` and its 19-variant definition in `runtime/extended/variants.json`:

| Mode | Before/after variant pairs | Per pair | Result |
|---|---:|---:|---|
| synchronous | 19 | 256 frames | exact output digests and decision projection |
| ring 2, 3 and 4 | 4 each | 256 frames | exact output digests and decision projection |
| ring 4, cancel every 53, recreate every 47 | 4 | 256 frames | exact output digests and decision projection |
| **Total** | **35** | **256** | **8,960 frame comparisons**, including warmup |

All ten before/after stage exits were zero and each after-stage comparison in the retained progress record
reported 256/256, no decision or pixel differences. This is an after-upscale control: it does not establish
present or D3D11 bridge equivalence, perceptual quality, game performance or native Windows behavior.

## D3D12 present-guide case

`runtime/present-delayed-summary.json` and `runtime/present-delayed-{before,after}/result.json` record
PASS for both DLLs. Each run used the production DLL, completed 80 evaluations and 16 harness captures,
and reported **34 uses of game guides** in the D3D12 present identity window. The runner also checked
that the selected native DLL was loaded. The original `runtime/present-{before,after}` attempts remain
**FAIL**: the snapshot was admitted but the run ended before the identity consumer's two-second window
published a positive counter. The opt-in 30 ms post-Present delay extends the test; it does not change
production behavior or weaken the counter gate.

The moving-scene readbacks are collected **before** `Present`, not from its final backbuffer. The positive
result is guide capture/identity/use coverage for this bounded case, not a present-pixel comparison, a
claim that all 27 functions executed, or evidence for resize, route switching, all lag cases or games.

## Native D3D11 bridge and shutdown — FAIL

Both instrumented runs in `runtime/dx11-instrumented-{before,after}` completed **200 evaluations and 200
presents**, in 6,883 ms and 6,946 ms respectively. Their logs show native guide transport and bridge use:

| Binary | Carried in logged 2 s window | Bridge used in two logged 2 s windows | Refused in those windows |
|---|---:|---:|---:|
| before | 42 | 64 + 66 = 130 | 0 |
| after | 41 | 64 + 66 = 130 | 0 |

These counters describe logged windows, not totals for all 200 frames. The fixed 640x360 fixture supplies
zero motion; it does not establish motion quality or final-present pixels. The markers show the D3D11
completion fence, `ReleaseFeature` and `DestroyParameters` returned, then stop at `Shutdown1 before`.
Both processes returned 5; the enclosing probe results remain **FAIL**, with no final PASS. The older
uninstrumented `runtime/dx11-{before,after}` FAILs are preserved separately.

Both SEH traces report `0xc0000005`, a write to address zero at `_nvngx.dll + 0x3b8b4`, called from
`OptiScaler.dll + 0x346969`. This occurs with both before and after binaries: the failure is observed on the
retained baseline as well as the candidate. It is not evidence that the extraction introduced the fault.

Read-only disassembly in `runtime/shutdown-abi-inspection/` shows that this core's D3D11 `Shutdown1` export
(RVA `0x5de30`) saves `RDX`, passes it through `R9` to the shared routine at `0x3b820`, which copies it to
`R13` and writes a 32-bit value through `[R13]` at `0x3b8b4`. The corresponding D3D12 core path also uses
this second output argument. In the source, `OptiScaler/proxies/NVNGX_Proxy.h` declares/returns the D3D11
function as a one-argument pointer, and `OptiScaler/inputs/NVNGX_DLSS_Dx11.cpp` calls it with one argument;
D3D12 has `ShutdownDx12Device`, which supplies an output. The trace and inspected ABI support an
argument-contract mismatch for **this core**, not a conclusion about every NVIDIA driver or core version.
No fix, cleanup bypass or alternate ABI was applied in this round.

## Rechecking retained evidence

Set `EVIDENCE_ROOT` to the local evidence root; do not put its personal absolute path in this document.
The following read-only command checks the retained extended-stage exits and comparison cardinality:

```bash
python3 - "$EVIDENCE_ROOT/runtime/extended/progress.json" <<'PY'
import json, sys
rows = json.load(open(sys.argv[1], encoding='utf-8'))
assert len(rows) == 10 and all(row['exit'] == 0 and not row['failed'] for row in rows)
pairs = [detail for row in rows for detail in row['compared'].values()]
assert len(pairs) == 35
assert all('frames 256/256, decisions differ at [], pixels differ at []' in item for item in pairs)
print(f'{len(pairs)} pairs, {len(pairs) * 256} frame comparisons')
PY
```

`python3 tests/nr-moving-evidence/run.py` runs the new host-only validator tests: 10 for D3D12 and 12
for D3D11, alongside the existing moving-evidence cases. The coordinator's final native
`python3 tests/run_all.py --tier host --jobs 4 --strict` run passed **100/100**, with zero FAIL, SKIP,
TIMEOUT, ABORTED or INCONCLUSIVE and sanitizers intact. Evidence: `artifacts/present-guides-runtime-final/summary.json`,
run ID `36244314349044d18327bcb8a0142787`, HEAD `dd2a5891`, `dirty=true`. Non-Markdown sources were stable;
documentation changed during the run, so the coordinator will repeat the final documentation and privacy
checks. Earlier 100/100 host results in the source review remain historical.

The GPU/Proton probe entry points and required flags are `python3 tests/dlssnr-loopback/present_guides_probe.py
run --help` and `python3 tests/dlssnr-loopback/dx11_present_probe.py --help`. Use the corresponding retained
before/after DLL and forwarder, the same hashed dependencies, and a **new** output directory per run. Execute
only under the coordinator's serial runtime admission. Existing evidence must not be overwritten.

For the same bounded probes, set `PROTON_EXEC` and `STEAM_ROOT` to the intended local runtime and client,
then choose a new tag. These commands use the retained, already-built harnesses and the replay dependency
links; they create new result directories and never reuse the retained ones:

```bash
: "${EVIDENCE_ROOT:?}" "${PROTON_EXEC:?}" "${STEAM_ROOT:?}"
D="$EVIDENCE_ROOT/runtime/replay-before/replay-run"
TAG="$(python3 -c 'import uuid; print(uuid.uuid4().hex)')"
for v in before after; do
  python3 tests/dlssnr-loopback/present_guides_probe.py run \
    --dll "$EVIDENCE_ROOT/binary-$v/OptiScaler.dll" \
    --forwarder "$EVIDENCE_ROOT/binary-$v/nvngx.dll_dlssnr.dll" \
    --harness "$EVIDENCE_ROOT/runtime/present-delayed-before/dlssnr-loopback.exe" \
    --model "$D/nvngx_dlssnr.dll" --sr "$D/nvngx_dlss.dll" --core "$D/_nvngx.dll" \
    --proton "$PROTON_EXEC" --steam "$STEAM_ROOT" \
    --out "$EVIDENCE_ROOT/runtime/recheck-present-$TAG-$v"
done
python3 tests/dlssnr-loopback/present_guides_probe.py compare \
  --before "$EVIDENCE_ROOT/runtime/recheck-present-$TAG-before" \
  --after "$EVIDENCE_ROOT/runtime/recheck-present-$TAG-after"
for v in before after; do
  python3 tests/dlssnr-loopback/dx11_present_probe.py \
    --dll "$EVIDENCE_ROOT/binary-$v/OptiScaler.dll" \
    --forwarder "$EVIDENCE_ROOT/binary-$v/nvngx.dll_dlssnr.dll" \
    --harness "$EVIDENCE_ROOT/runtime/dx11-instrumented-before/dx11-present.exe" \
    --model "$D/nvngx_dlssnr.dll" --sr "$D/nvngx_dlss.dll" --core "$D/_nvngx.dll" \
    --proton "$PROTON_EXEC" --steam "$STEAM_ROOT" \
    --out "$EVIDENCE_ROOT/runtime/recheck-dx11-$TAG-$v"
done
```

These commands test the retained binaries, not a future fix. A D3D11 FAIL, including a shutdown crash,
must be retained as FAIL; matching guide counters alone do not turn it into a PASS.

## Remaining gate

Investigate and separately authorize a production fix for the D3D11 `Shutdown1` ABI; reproduce it against the
same core and verify cleanup before considering the D3D11 route passed. A fix is outside this record. Final
documentation and privacy checks, native Windows, resize, route switching/lag, present output pixels and
game/runtime qualification remain open. Integration remains blocked; no result above should be promoted beyond its route
and observed environment.
