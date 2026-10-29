# Release identity and validation

A release has three distinct identities: the packaging checkout, the archive/member bytes and the evidence that
actually exercised those bytes. None may silently stand in for another. Publication is always a human action.

## Artifact contract

A distributable CI draft contains `Retoque-vX.Y.Z.zip` and `Retoque-vX.Y.Z.zip.manifest.json`.
The version-1 `retoque-release-manifest` sidecar records:

- Full packaging checkout commit and dirty state, plus declared build configuration/mode/toolchain.
- Archive SHA-256/size and each member's relative name, uncompressed size and SHA-256.
- A sanitized complete host-test summary and hash of its input report, when supplied.
- Explicit `NOT_RUN` runtime qualification. The tool does not invent GPU, native Windows, game or quality approval.

The sidecar does not contain absolute local paths, raw logs, model binaries or a signature/authenticity claim.
Hashing an archive identifies its bytes; it does not prove those bytes were compiled from the packaging checkout.
This matters especially for `-SkipBuild`, which can package earlier outputs. Build mode/toolchain are reported
metadata, not independently verified compiler provenance.

## Source-test gate

The CI draft job depends on a host-check job for the same workflow checkout. That job runs the full registered
host tier strictly and uploads its report. The manifest gate rejects wrong schema/commit/run identity, dirty or
unknown source state, nonterminal/non-PASS results, partial/duplicate suite selections, mismatching counts and
non-host/non-PASS per-suite records. A selected subset cannot certify the complete host tier.
The producer exports the exact artifact name and report run ID. A retry of only the draft job uses that identity,
not a newly constructed artifact name based on the consumer's attempt number.


Host PASS certifies only those source-level contracts. It is not evidence that the new Windows build's renderer
ran. Different GPU/runtime results need their own exact binary identities and coverage records.

## Commands

The existing PowerShell packager remains usable without Python. It assembles the archive, not a qualification.
Complete the release identity step with Python 3.11+ (stdlib only):

```bash
python3 tools/release_manifest.py --archive release/Retoque-v0.3.0.zip --version retoque-v0.3.0
python3 tools/release_manifest.py --archive release/Retoque-v0.3.0.zip --version retoque-v0.3.0 --host-report artifacts/host/summary.json --require-host
```

The first command leaves host evidence `NOT_SUPPLIED`; it is suitable for local inventory, not the CI release gate.
An explicitly supplied invalid report always fails, even without `--require-host`. The default sidecar path is the
archive filename plus `.manifest.json`. `--configuration`, `--build-mode` and `--msbuild-version` label the declared
build context; `existing-output` is the conservative default. CI supplies the build metadata and expected report
run ID, and requires the report before it creates/uploads a draft.

Keep the report outside tracked source paths so producing or downloading it cannot itself dirty the checkout.
Archive validation refuses traversal/ambiguous paths, duplicate case-insensitive names, symbolic links, oversized
members and any `nvngx_dlssnr.dll`. User-supplied models never become package evidence.

## Before publishing

Review source-test evidence, build output and the sidecar against the actual zip. Test extracted installers on
the supported operating systems. Run the relevant [route matrix](../maintenance.md#validation-route-matrix) on
those exact DLLs, preserving hashes, model/driver/runtime/settings, outcome and limitations. Record visual/performance
judgment separately from successful API execution. Attach or link a reviewed, sanitized runtime report when available;
this tool deliberately does not auto-promote old rendering reports to current-binary PASS.

Version-specific notes under this directory must state new behavior, compatibility caveats and untested areas.
A draft, nightly artifact or host-green commit is not automatically a qualified release. An upload failure may leave
a partial draft; inspect it manually rather than retrying with an operation that overwrites or publishes it.

## Maintenance

Schema changes require regression tests in `tests/release-evidence/`. Keep hashes stable and paths relative.
Retain rejected/partial evidence for diagnosis, but never attach unsanitized test logs as public release summaries.
The docs checker verifies links and the generated test catalog; it does not validate a release's scientific claims.
