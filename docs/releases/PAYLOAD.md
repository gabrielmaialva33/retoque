# App payload release archive (`retoque-payload-<version>-x64.zip`)

Beside the full script-installer bundle (`Retoque-vX.Y.Z.zip` documented in [VALIDATION.md](VALIDATION.md)),
`gabrielmaialva33/retoque` releases may attach a payload-only archive for `retoque-app`:

- `retoque-payload-<version>-x64.zip` (where `<version>` is the numeric/prerelease version stripped of the
  `retoque-v` or `v` tag prefix, e.g. `retoque-payload-0.1.0-x64.zip` for tag `retoque-v0.1.0`)
- `SHA256SUMS` (`sha256sum` line for `retoque-payload-<version>-x64.zip`)

Publication is always manual. The packaging script prints the `gh release create` command it would run and never
executes it automatically.

## Archive contents and allowlist

The archive contains only the files `retoque-core` and the app require to install or update Retoque:

- `OptiScaler.dll` (built x64 proxy DLL)
- `nvngx.dll_dlssnr.dll` (built x64 NR forwarder DLL; its PE `VERSIONINFO` fixed file version must match `OptiScaler.dll`, and it must export `dlssnr_call_create`, `dlssnr_call_evaluate`, `dlssnr_call_set_extras` and `dlssnr_abi_version`)
- `OptiScaler.ini` (configured with release defaults matching `tools/package_release.ps1`: `LogToFile=true`, `LogLevel=2`, `ShortcutKey=0x24`, with no `Enabled=true` key switched on by default)
- `Licenses/` (including `Licenses/RenoDX_ATTRIBUTION.txt`)
- `translations/README.md` and `translations/optional/pt-BR.lang` (inactive optional dictionary; never an active `OptiScaler.lang` at the archive root)
- `OptiScaler/` (optional support directory when present in the build output)
- `SHA256SUMS` (member checksums inside the archive, in addition to the release-level `SHA256SUMS` asset)

## Forbidden files

Neither the packager nor the app unpacker ever accepts:

- NVIDIA NR models (`nvngx_dlssnr*.dll`, `DlssNrModels/`) or NGX runtimes (`_nvngx.dll`, `nvngx_dlss.dll`, `nvngx_dlssd.dll`, `nvngx_dlssg.dll`)
- `dlssg_for_sm86` files (`*sm86*`, `version.dll`, `dlssg_sm86.ini`, `dlssg_sm86/`)
- Lossless Scaling files or compiled shader caches (`lossless.dll`, `lsfg-vk.dll`, `*.cso`, `*.spv`)
- Build intermediates or legacy artifacts (`*.pdb`, `*.lib`, `*.exp`, `*.ilk`, `*latewarp*`)
- Active localization overrides (`OptiScaler.lang`)
- Symbolic links, encrypted entries, path traversal (`..`, leading `/`, `\`, `:`), Windows reserved names, or case-insensitive path collisions

## Packaging command

```bash
bash tools/app/scripts/make-payload-release.sh --build-dir x64/out --out release/payload --version retoque-v0.1.0
```

The script verifies required files, the OptiScaler marker, the four forwarder exports, matching PE `VERSIONINFO`
file versions between `OptiScaler.dll` and `nvngx.dll_dlssnr.dll`, the release INI defaults and the forbidden-file
guard before writing `retoque-payload-<version>-x64.zip` and `SHA256SUMS`.

## App verification and unpacking

When the user confirms checking or downloading from `gabrielmaialva33/retoque` in `retoque-app`
(`retoque_data::payload_fetch`):

1. Only HTTPS requests to `api.github.com/repos/gabrielmaialva33/retoque/releases`,
   `github.com/gabrielmaialva33/retoque/releases/download/...`, and GitHub's release asset CDNs
   (`release-assets.githubusercontent.com`, `objects.githubusercontent.com` on asset redirects) are allowed.
2. When no release is published yet (HTTP 404 or empty release list), the app states so plainly and keeps the
   existing local sources (beside the app or a user-chosen folder).
3. Before unpacking, the archive's SHA-256 is verified against both `SHA256SUMS` and the GitHub asset `digest`
   (`sha256:...`).
4. Only the allowlisted paths above are unpacked into app data (`payload-releases/<version>/payload`). Game
   folders are never written during download; updating an installed game always goes through the normal reviewed
   `Plan`.
