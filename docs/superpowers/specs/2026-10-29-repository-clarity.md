# Repository clarity and traceability

Status: scope approved in conversation on 2026-10-29 (steps 1 and 2 only).

## Intent

Make Retoque's documentation authoritative and its tests/releases traceable without changing rendering, defaults, source layout or upstream integration boundaries.

## Approaches

A wholesale directory migration would create upstream conflicts and invalidate test slices. A documentation-only cleanup would leave the release/evidence gap. The approved approach is incremental: retain paths, clarify document authority and add small tools to the existing runner and packager.

## Scope and interfaces

- Keep `OptiScaler/`, `retoque/`, stable suite IDs and `AGENTS.md -> CLAUDE.md`.
- Replace the long agent entrypoint with a concise safety/routing guide. Preserve its full previous content as a dated historical reference, not a competing current instruction source.
- Add current contributor, architecture, capability and release-evidence guides; refresh the roadmap using source-backed records. Unverified hardware results remain unverified.
- Extend the existing runner with comma-separated `--domain`, `--group-by domain` and deterministic `--list --format markdown`. Preserve the default all-tier listing and host-only execution. `--only` still overrides tier; domain intersects the selection, then `--skip` applies. Reject unknown domains, including in list mode.
- Generate `tests/CATALOG.md` from `suites.toml`. A stdlib documentation check verifies local Markdown/HTML destinations and catalog freshness, without network requests or claims about external links/anchors.
- Produce a JSON sidecar for each CI release zip, as a required post-packaging step: archive and member hashes, packaging checkout identity, declared build configuration/toolchain, and a sanitized source-test summary. The standalone PowerShell packager stays Python-free; local inventory uses the explicit manifest command. A package hash is binary identity, not proof of its source or runtime qualification.
- Require a complete, clean, same-commit host PASS before the CI draft job. A partial or stale report cannot qualify a release. GPU and native Windows runtime qualification stay explicitly `NOT_RUN` unless separately provided and reviewed; this change does not automate those claims.
- Preserve manual publication and existing packaging safety rules. Never redistribute user models, expose local paths in public summaries, or infer that `-SkipBuild` outputs were compiled from the current checkout.

## Non-goals

No C++/HLSL edits, renderer refactor, default changes, GPU/Wine workload, installation in games, automatic promotion, commits, merge or push in this task. Do not disrupt other agents or the main checkout.

## Acceptance

Focused RED/GREEN tests for selection, docs checks and release evidence. Run the full host tier where prerequisites permit and report every failure/skip honestly. Check generated catalog, local links, privacy, workflow syntax and unchanged renderer/INI. Native Windows packaging and GitHub Actions remain unexecuted unless explicitly run; do not equate synthetic package tests with a real DLL release.
