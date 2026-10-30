# D3D11 core shutdown: review and validation — 2026-10-30

## Conclusion and scope

The `Shutdown1` blocker in the reproduced D3D11 shutdown scenario is resolved for the
runtime and inputs below. This is not a general driver, game or Windows qualification.
The public SDK callable retains its one-argument ABI; the raw core slot and load cast use
an isolated two-argument ABI, with initialized writable output supplied by the adapter.
Shutdown and cleanup were not bypassed. No shader, INI or main-branch changes are part
of this fix. Production and test sources remained frozen by hash during validation;
subsequent changes are documentation only.

## Provenance

Both builds used base `dd2a589108652870a702313678cf67cf8a98ced7` with `dirty=true`;
HEAD alone does not identify either build. The receipts identify the actual binaries and
source snapshots:

| Build | OptiScaler.dll SHA-256 | Source snapshot SHA-256 |
| --- | --- | --- |
| Fix only | `6f87f65d0ba9b85491a9309b14b7b15342732f3ba79febbdf3be470154d80668` | `d40a4fd1486f891896d8d600f9fa35ab354d3b5e7fd83e883e1e6cbf2089fb87` |
| Combined with present-guide extraction | `63d95a0f8a2a9d8d0ac72bfd0acfd09aae6140491f83fb21bb3d14967bfc32a4` | `a7438996111e6b9aed4972a8541bddc087f898532e1fae63ae3d06ccdb09b0ac` |

The combined composition identity records the refactor patch SHA-256
`2c001fe7223a7043b67c5eee78c609e1d42dff579b42022c41b999c389033773`
and fix code patch SHA-256
`852d38d167fd472fbab6cabdedf7de73b2aedc62c08e0895da89373d3674443b`.
Receipts: `<evidence-root>/fix-only-binary/receipt.json`,
`<evidence-root>/combined-binary/receipt.json`, and `<evidence-root>/composition/identity.json`.

## Evidence

- Baseline DLL `687cdbe2adf574321dfb3f03fe5e4964741bf8a2b4cd0a5f5408314006d03f04`
  reproduced FAIL on 2026-10-30, at Shutdown1 after 200 frames. The result JSON reports
  harness failure; the retained trace supplies the shutdown location.
- Fix-only passed 200 evaluations and 200 presents. Shutdown1, ReleaseSwapchain,
  ReleaseContext, ReleaseDevice and DestroyWindow returned, and the process exited 0.
  Two combined D3D11 runs each passed 200 evaluations and 200 presents with exit 0.
  No `c0000005` or unhandled exception was observed in the three corrected D3D11 traces.
  Core, model and D3D11 harness inputs were identical across these runs and baseline.
  Results: `<evidence-root>/baseline/result.json`, `<evidence-root>/fix-only-runtime/result.json`,
  and `<evidence-root>/combined-runtime-summary.json`.
- Combined D3D12 present passed and recorded 67 guide uses. This is bounded evidence of
  guide consumption; there was no post-present pixel readback.
- Combined replay compared six variants, 256 frames each: 1,536 frames had identical
  pixels and decisions to baseline `687cdbe2adf5`. This is a new bounded comparison,
  not the earlier 8,960-frame result. Evidence: `<evidence-root>/combined-replay-summary.json`.
- Each of the fix-only and combined host runs recorded 100 PASS with sanitizers intact;
  each Release build passed. The stage records report stable source. Fix-only evidence:
  `artifacts/dx11-shutdown-final/host/summary.json` and `artifacts/dx11-shutdown-final/stages.json`.
  Combined evidence: `<combined-validation>/host/summary.json` and
  `<combined-validation>/stages.json`.
- Independent Claude review found no blocker. Its low-severity documentation comments
  were corrected; the host RED was a type assertion, not a runtime reproduction, and
  does not establish Windows x64 calling convention. The fixed runtime provides the
  separate Proton evidence. Review: `<evidence-root>/claude-review.txt`.

`<evidence-root>` denotes the retained D3D11 shutdown evidence directory;
`<combined-validation>` denotes the combined worktree's validation artifacts. Paths are
placeholders to avoid recording machine-specific personal paths.

## Limits and remaining gate

The observed environment was RTX 4090 on Linux driver 615.78.08, Proton Experimental,
core 32.0.16.1752 SHA-256
`dabe18b0dc559750113b589d06f672141c8c49a2a571e0a5a7ca7b3e584a7ac3`, and model 310.8.2.
No native Windows, games, resize, route switching, performance, final-present image
quality or null-device runtime validation is claimed. The null and unavailable-entry
paths have host fake coverage only; the inspected private ABI is not a universal
contract across core versions.

Final metadata checks passed natively: docs, privacy and nr-shutdown, 3/3 PASS.
Evidence: `artifacts/dx11-shutdown-metadata/summary.json`, run ID
`92294dd89f8b4ad48bacc77436a78035`. The coordinator will repeat checks after this textual update.
