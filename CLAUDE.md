# Working on Retoque

`AGENTS.md` points here. This is the current entrypoint for all agents, not a release-status report.
Retoque extends OptiScaler with NR, presentation and Proton integrations. The default branch is
`dlss-neural-rendering`; the upstream we integrate is `optiscaler/master`, not the older Dagherbou fork.

## Non-negotiable boundaries

- Check status, branch, worktrees and recent reflog before writing. Other agents and the user work concurrently.
  Preserve their changes. Use an isolated branch/worktree for unproven work; never reset another checkout.
- Never commit, merge or push without the user's authorization for that operation. The default branch has
  historically been mirrored externally; do not assume a local commit there remains private.
- Keep upstream files at upstream paths. Fork-owned implementation belongs in `retoque/`, tooling in `tools/`,
  suites in `tests/` and design/measurement records in `docs/`. Do not reorganize `OptiScaler/` for aesthetics.
- Read the module's README and relevant design note before changing behavior. Preserve rejected experiments
  and their evidence. The authoritative NR rules are [DEVELOPMENT.md](docs/dlssnr/design/DEVELOPMENT.md).
- NR disabled/default paths must remain unchanged. No control for an unavailable feature; config keys round-trip
  through declaration, read, save and shipped INI. C++ constants and HLSL cbuffer layout stay append-only/in sync.
- Never free resources still used by the GPU. Preserve lock order and recording/completion identity; abandon
  resources after device loss when a safe drain cannot be proved. Do not introduce speculative lifetime changes.
- Never overwrite DLLs in a running game. Use receipt-based installers; inspect the actually loaded proxy.
  Keep one proxy per game folder even though a second copy now has a pass-through guard.
- Never redistribute `nvngx_dlssnr.dll` or the user's LSFG shaders/models. Preserve third-party licences and
  `Licenses/RenoDX_ATTRIBUTION.txt`. Keep personal paths, network addresses, tokens and raw local captures out of Git.
- Do not commit `x64/`, `.winx/`, build stamps, temporary clones or machine-local scripts. A successful build or
  host test does not prove GPU behavior, image quality, performance or native Windows compatibility.

## Read by task

| Task | Read first |
|---|---|
| Find responsibilities | [Architecture](docs/architecture.md), then the code-adjacent README |
| Contribute or review | [Contributing](docs/contributing.md), [NR rules](docs/dlssnr/design/DEVELOPMENT.md) |
| Determine what works | [Capabilities and evidence](docs/capabilities.md); never infer support from a toggle |
| Plan next work | [Roadmap](docs/dlssnr/roadmap.md); proposals are not implementation authorization |
| Build, shaders, upstream integration | [Maintenance](docs/maintenance.md), [tools](tools/README.md) |
| Choose tests | [Tests](tests/README.md), generated [catalog](tests/CATALOG.md) |
| Package or publish | [Release validation](docs/releases/VALIDATION.md); publication remains manual |
| Investigate old merge decisions or Wine stalls | Dated [maintainer notes](docs/history/maintainer-notes-20261029.md) |

## Build

Windows DLL, MSVC, x64; Linux uses MSVC under Wine, not a native Linux build.
`./build-local.sh [Release|Debug]` owns the shared compiler-prefix lock. Never run other Wine/compiler work
against that prefix concurrently or wrap the script in the same lock. Never reset/kill the shared prefix
as automatic recovery; preserve evidence and establish process ownership. Redirect logs to a file, not a pipe.
Do not deploy a failed build. See [maintenance](docs/maintenance.md#build-and-toolchains).

Do not run `git submodule update --init --recursive` blindly. The old FidelityFX SDK is deliberately partial.
Host CI checks out only `external/simpleini`, `external/spdlog` and `external/vulkan`.
Use clang-format **22**, not whichever version happens to be on PATH. Every C++ translation unit starts
with `pch.h`; headers never include it. See upstream [CONTRIBUTING.md](CONTRIBUTING.md).

## Tests

```bash
python3 tests/run_all.py --list --group-by domain
python3 tests/run_all.py --domain tooling --strict
python3 tests/run_all.py --tier host --jobs 4 --strict
python3 tools/check_docs.py
```

Wine/GPU suites are serial and require their own prerequisites. A SKIP is not a PASS. Record exact commands,
commit/dirty state and binary hashes, with the limits of each result. New D3D12 passes require native Windows
validation; Proton accepting a call is not a D3D12 correctness oracle.

## Precompiled shaders

HLSL edits do nothing until bytecode headers are regenerated. Rebuild both DX12 and Vulkan for the common
NR shader (`dxc cs_6_0`, plus `-spirv -D VK_MODE`); use the code-adjacent [recipe](retoque/dlssnr/README.md).
Keep generated headers out of formatting. Review passthrough and constant-buffer invariants before deployment.

## Linux / Proton patches

Retain the fork's hook/overlay, device ownership and frame-generation protections during upstream merges.
Resolve toward upstream structure, then reapply required behavior and test it. Current merge records are indexed
in [maintenance](docs/maintenance.md#upstream-integration); the detailed historical patch table remains archived.

## Test target

Use the [route validation matrix](docs/maintenance.md#validation-route-matrix), not a single game's success.
Game-specific settings and model versions belong in their design notes and user-owned installations.

## Documentation

[docs/README.md](docs/README.md) defines document authority. Keep current instructions short, dated observations
in evidence records, and history explicitly historical. Regenerate `tests/CATALOG.md` after registry edits with
`python3 tools/check_docs.py --write-catalog`. Never promote a historical run to current-build qualification.
