# Documentation index and authority

Start with the document that answers your question. Historical measurements never automatically certify the
current checkout or a freshly packaged binary. Upstream-owned files keep their paths; fork guidance lives here.

| Question | Authoritative entry |
|---|---|
| What is Retoque and how do I install it? | [README](../README.md) |
| Which behavior exists and where was it validated? | [Capabilities and evidence](capabilities.md) |
| How do I contribute safely? | [Contributor guide](contributing.md), [NR invariants](dlssnr/design/DEVELOPMENT.md) |
| Where does each responsibility live? | [Architecture](architecture.md), then code-adjacent READMEs |
| What work is next or blocked? | [Roadmap](dlssnr/roadmap.md) |
| How do I build, merge upstream or investigate a runtime failure? | [Maintenance](maintenance.md), [tools](../tools/README.md) |
| Which tests exist and what do they cover? | Generated [test catalog](../tests/CATALOG.md), [test methodology](../tests/README.md) |
| What exactly is in a release and what passed? | [Release validation](releases/VALIDATION.md), version-specific [release notes](releases/) |
| Why was a design chosen or rejected? | [NR design index](dlssnr/README.md), [reviews](superpowers/reviews/) |

## User references

[Config.md](../Config.md) and `OptiScaler.ini` describe configuration. The root
[Features](../Features.md), [Spoofing](../Spoofing.md), [Issues](../Issues.md) and
[Changelog](../Changelog.md) retain the upstream context; do not confuse upstream release notes with Retoque
qualification. External DLL telemetry is documented in [ExternalDlssgTelemetry](guides/ExternalDlssgTelemetry.md).

## Code-adjacent references

| Module | Entry |
|---|---|
| Neural Rendering | [NR module](../retoque/dlssnr/README.md), [forwarder](../retoque/dlssnr/forwarder/README.md) |
| Synthesized motion | [Estimator and provenance](../retoque/shaders/synth_motion/README.md) |
| LSFG | [Runtime/presenter](../retoque/lsfg/README.md), [study tooling](../tools/lsfg-study/README.md) |
| Optional translations | [Text packs](../retoque/translations/README.md) |

## Linux, Proton and integration records

The [maintenance integration index](maintenance.md#upstream-integration) links the dated upstream merges.
Additional focused records: [non-graphics Vulkan presentation](linux-proton/vulkan-overlay-nongraphics-present.md),
[fork audit fixes](linux-proton/fork-fixes-20261016.md), [nvapi kept loaded after the startup checks](linux-proton/nvapi-keep-loaded-20261030.md),
[FG swapchain lock and lifetime fixes](linux-proton/fork-fixes-20261030.md), [Dynamic MFG retry wait, NR pause guard and menu key polling](linux-proton/fork-ports-menu-pause-backoff-20261030.md),
[game camera forwarding](linux-proton/streamline-game-camera.md) and
[Unreal plugin DLSS search](linux-proton/unreal-dlss-search-20261030.md).

## Historical evidence

The [old maintainer guide](history/maintainer-notes-20261029.md) and
[old comparison roadmap](dlssnr/roadmap-20261029-history.md) are dated snapshots, not alternative live instructions.
The [forwarder investigation](history/FORWARDER_INVESTIGATION.md) retains failed approaches and later findings.
Keep rejected experiments: they prevent repeated work. Plan/spec/review records under `superpowers/` record the
scope and evidence of individual changes, not a second roadmap.

## Keeping this index useful

Update the current authority when a status changes; add a dated evidence record rather than rewriting history.
`python3 tools/check_docs.py` checks local destinations and generated catalog freshness. It does not verify
external URLs, heading anchors or technical truth. Regenerate the catalog after changing `tests/suites.toml`.
Third-party attribution and distribution obligations remain in [Licenses](../Licenses/).
