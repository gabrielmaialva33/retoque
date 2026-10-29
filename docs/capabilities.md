# Capabilities and evidence

Source review snapshot: `9778aeb5`, 2026-10-29. This is the navigation authority for implementation versus
validation. It is not an assertion that the current DLL or every GPU passed. Update an entry when its behavior
or evidence changes; do not silently apply an old result to a newer binary.

## Reading the states

**Implementation:** implemented, experimental, proposal, parked or external integration.
**Evidence:** host contract, GPU fixture, game observation, or pending. These are independent axes, not a single
"supported" checkbox. Defaults below refer to source defaults; explicit user settings and game profiles can override
them. Configuration truth remains in [Config.md](../Config.md) and [OptiScaler.ini](../OptiScaler.ini).

The suite links identify coverage, not a fresh PASS. The dated records identify what actually ran. Missing
platform evidence means pending/unknown, never inferred success.

## Runtime paths

| Capability | Implementation / activation | Code and tests | Evidence and boundary |
|---|---|---|---|
| D3D12 after-upscale NR | Implemented; NR opt-in, `HookMethod=1` default placement | [Renderer](../retoque/shaders/dlssnr/DlssNr_Dx12.cpp); [dispatch host](../tests/nr-dispatch/README.md), [loopback](../tests/dlssnr-loopback/README.md) | [Rendering qualification](superpowers/reviews/2026-10-28-rendering-qualification.md): 2026-10-28, source `abad9704`; scoped GPU fixtures, not current-binary or universal game certification |
| Vulkan after-upscale NR | Implemented; NR opt-in | [Feature](../retoque/dlssnr/DlssNrFeature_Vk.cpp); [Vulkan guides](../tests/nr-vk-guides/README.md), [guide GPU fixture](../tests/guide-match-vk/README.md) | Same 2026-10-28 report covers named Vulkan scenarios and states readback/resubmission limits explicitly |
| D3D12 present NR | Implemented; `HookMethod=2` or eligible auto routing | [Present boundary](../tests/nr-present-boundary/README.md), [present host](../tests/nr-present-host/README.md) | [NR before game DLSS-G](dlssnr/design/nr-before-dlssg.md); placement is not equivalent across all FG providers or late runtime toggles |
| D3D11 bridge and native DLSS guides | Implemented; game/profile and bridge routing apply | [Transport](../retoque/dlssnr/DlssNr_Dx11Guides.cpp); [host contracts](../tests/nr-dx11-native-guides/README.md), [GPU transport](../tests/dx11-guides-transport/README.md) | [Design and observations](dlssnr/design/dx11-native-dlss-guides.md); historical Sons Of The Forest report on 2026-10-15, not a claim about every D3D11 game |
| Vulkan present without an upscaler | Experimental; `VulkanPresentHost=false` default | [Host](../retoque/dlssnr/DlssNr_VkPresentHost.cpp); [host contracts](../tests/nr-vk-present-host/README.md), [runtime cases](../tests/vulkan-present-host/README.md) | [Host design](dlssnr/design/nr-vulkan-present-host.md); observe route hand-back, formats, queue family and FG stand-down separately |
| HDR at present | Experimental; `HdrPresent=false` default | [Colour policy](../retoque/dlssnr/DlssNr_PresentColour.h); [math](../tests/nr-hdr-math/README.md), [D3D12 fixture](../tests/nr-hdr-d3d12/README.md) | [HDR conversion](dlssnr/design/hdr-present-conversion.md); D3D12/bridge and Vulkan-host evidence are separate; other formats may be declined |
| Model cadence | Implemented, opt-in frequency reduction; `Cadence=1` runs every frame | [Cadence contracts](../tests/nr-cadence/README.md), [replay metrics](../tests/nr-replay-metrics/README.md) | [2026-10-28 analysis](superpowers/reviews/2026-10-28-temporal-cadence-analysis.md) rejected the tested adaptive candidate on quality; functional PASS is not visual approval |
| Working-scale / RR scale controls | Implemented; architecture/placement-specific defaults | [Scale policy](dlssnr/design/working-scale-auto.md), [render size tests](../tests/nr-render-size/README.md) | [2026-10-29 retention measurement](dlssnr/design/working-scale-detail-retention.md): build `c862bd2e`, one RR clip on RTX 4090; does not qualify a default change or non-RR route |
| LSFG x2/x3/x4 presenter | Implemented study path, optional | [Module](../retoque/lsfg/README.md); [integration](../tests/lsfg-integration/README.md), [presenter](../tests/lsfg-presenter/README.md) | Those host suites do not execute D3D12. No broad runtime qualification is asserted here; user-owned shaders/models remain external |
| RTX 20/30 FG via sm86 | External integration, distinct from native Retoque provider work | [Research and field records](dlssnr/design/mfg-rtx20-30.md), [probe](../tests/mfg-probe/README.md) | 2026-10-29 field record: Retoque `c862bd2e` plus external sm86 0.4.1, RTX 3060, Windows, Mortal Shell II. Not a native-provider implementation or all-Ampere/Turing certification |
| ViT reuse | Proposal/re-evaluation; profiler exists, reuse is not promoted by this index | [Design](dlssnr/design/vit-skip.md), [profiler contracts](../tests/nr-kernel-profile/README.md) | Earlier study has known limitations; an external fork's newer implementation still needs current code review and target-hardware measurement |

## Reliability and tooling

| Capability | Source state | Evidence / next boundary |
|---|---|---|
| Second DLL copy pass-through | Implemented, commit `8e6d4f4b` | [Instance-claim contracts](../tests/optiscaler-instance-claim/README.md); an actual duplicate-proxy run must identify both DLLs. Keep one proxy per installation |
| Display-only adapter filtering | Implemented, commit `99945d24` | [Adapter contracts](../tests/display-only-adapter/README.md); a Parsec/Windows run remains its own observation, not implied by host tests |
| Zero-motion probe | Implemented, gated by `ZeroGuideReset`; gate commit `a5960549` | [Host contracts](../tests/nr-zero-motion/README.md), [design](dlssnr/design/zero-motion-detection.md); new D3D12 pass requires native runtime evidence |
| Installation receipts and rollback | Implemented | [Deploy tests](../tests/deploy-safety/README.md), [Windows-installer fixtures](../tests/windows-installer/README.md); PowerShell-on-Linux cases do not certify NTFS/Windows process behavior |
| Source test reports | Implemented, invocation-aware | [Runner tests](../tests/runner-results/README.md), [methodology](../tests/README.md); process-exit suites and structured-result suites are labeled separately |
| Package identity and source evidence | Tooling in this change | [Release contract](releases/VALIDATION.md); runtime remains NOT_RUN in the automatic manifest. CI itself must run before claiming a hosted release passed |

## How to record new evidence

Record source commit plus dirty state, binary/model hashes where relevant, date, environment (GPU/driver/API/runtime),
settings, scenario, command and outcome, then its limits. Prefer a link to the review/measurement record over copying
metrics here. A source snapshot is not a binary build receipt; a model's successful call is not a perceptual verdict.

Current priorities live in the [roadmap](dlssnr/roadmap.md). Detailed suite descriptions live in the generated
[catalog](../tests/CATALOG.md) and individual READMEs. A release's exact archive identity belongs in its manifest,
not in this manually maintained overview.
