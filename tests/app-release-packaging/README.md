# App release packaging

`python3 tests/app-release-packaging/run.py` runs the synthetic contract tests of the Windows app release scripts in
`tools/app/scripts/`: `test_make_app_release.py` (the app archive: payload checksums, forbidden and unsafe members,
the embedded version marker, merged release `SHA256SUMS`) and `test_make_setup.py` (`Retoque-Setup-<version>.exe`:
the appended bundle and its trailer, `release.json` that matches the app's `Release::validate_kind(true)`, checksum
cross-checks, stub checks and the merged output `SHA256SUMS`).

Everything is synthetic and local: no executable is run, nothing is uploaded or published. The Rust side of the
setup (trailer parsing, bundle verification, extraction with the app's unpacker, the app's `--setup` decisions) is
covered by `cargo test --workspace` in `tools/app`. Neither proves the setup or the app on Windows; see
[APP-UPDATE.md](../../docs/releases/APP-UPDATE.md#validation-boundary).
