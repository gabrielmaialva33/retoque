# Additional application dependencies

## nucleo-matcher 0.3.1

The application's synchronous fuzzy search uses the unmodified `nucleo-matcher` crate, under the
Mozilla Public License 2.0 (MPL-2.0). It does not use the `nucleo` thread pool.

- Source for the exact crate: <https://crates.io/crates/nucleo-matcher/0.3.1>
- Source and license: <https://github.com/helix-editor/nucleo>
- License text: <https://www.mozilla.org/MPL/2.0/>

Preserve the upstream source and license notices when distributing. The MPL-covered portions remain
available under MPL-2.0, and are additionally distributed with this GPL-3.0-only Larger Work under the
terms of MPL section 3.3. The crate is not marked Incompatible With Secondary Licenses.
See <https://www.mozilla.org/en-US/MPL/2.0/FAQ/#q14> for Mozilla's explanation of this combination.

## SPIRV-Cross and Rust bindings

The automatic LSFG preparation pipeline statically links the vendored SPIRV-Cross sources shipped by
`spirv-cross-sys` 0.7.4+aa217ae through `spirv-cross2` 0.7.2. Only the HLSL API feature is enabled. The bindings
are available under MIT OR Apache-2.0; this distribution selects Apache-2.0. SPIRV-Cross itself is Apache-2.0.
Preserve their copyright, license and attribution notices with distributions.

- Binding source and license: <https://github.com/SnowflakePowered/spirv-cross2>
- Exact crate: <https://crates.io/crates/spirv-cross2/0.7.2>
- Vendored source crate: <https://crates.io/crates/spirv-cross-sys/0.7.4+aa217ae>
- SPIRV-Cross source and license: <https://github.com/KhronosGroup/SPIRV-Cross>
- Apache License 2.0: <https://www.apache.org/licenses/LICENSE-2.0>

## Microsoft DirectX Shader Compiler v1.8.2505.1

DXC is an external dynamically loaded compiler, separately distributed from the application's GPL code.
The kit stages unmodified official `dxcompiler.dll` and `dxil.dll`; Linux uses `libdxcompiler.so` and `libdxil.so`.
No NVIDIA or Lossless Scaling files are included. The release's `ReleaseNotes.md` license table assigns `LICENSE-LLVM.txt` (University of Illinois/NCSA) to all files
except `d3d12shader.h` (MIT), including the compiler and DXIL validator libraries. This is the pinned release's
redistribution scope, not an assumption that older proprietary `dxil.dll` releases had the same terms. The archive
also contains legacy `LICENSE-MS.txt`; preserve it as shipped without relabeling any third-party file GPL. The kit
helper retains all Windows license files and the release notes in `Licenses/DXC`; the downloader retains those
present in each platform archive alongside the libraries. Preserve the NCSA copyright, conditions and disclaimer
in redistributed binary documentation and do not use the authors' names to endorse this application.

- Pinned official release: <https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2505.1>
- Source license: <https://github.com/microsoft/DirectXShaderCompiler/blob/v1.8.2505.1/LICENSE.TXT>
- Official release/redistribution guidance: <https://github.com/microsoft/DirectXShaderCompiler/wiki/Releases>
- Windows asset: `dxc_2025_07_14.zip`, SHA-256 `9ad895a6b039e3a8f8c22a1009f866800b840a74b50db9218d13319e215ea8a4`
- Linux asset: `linux_dxc_2025_07_14.x86_64.tar.gz`, SHA-256 `f2213da1fc99dc8778c8823078e16ba97c7f80f86a1d4520ab1adf4b462bc48c`

The download implementation uses `ureq` (MIT/Apache-2.0) with `rustls` (Apache-2.0/ISC/MIT) and Mozilla-derived
`webpki-roots` (CDLA-Permissive-2.0). Dynamic loading uses `libloading` (ISC); gzip/tar extraction uses `flate2` (MIT/Apache-2.0)
and `tar` (MIT/Apache-2.0). No async runtime is introduced.

The Windows GNU target statically links the MinGW GCC C++ runtime to avoid requiring a MinGW installation on
end-user machines. GCC's runtime is GPL-3.0 with the GCC Runtime Library Exception 3.1; retain the applicable
runtime notices. See <https://www.gnu.org/licenses/gcc-exception-3.1.html>.

## Official dlssg_for_sm86 downloads (optional; not embedded in the application)

The app can fetch `sdli1995/dlssg_for_sm86`'s official GitHub release only after confirmation.
The first-run cache builder is an explicit opt-in and stores the unmodified official archive and
its GitHub SHA-256 digest. The app verifies it again before import. Never commit these binaries.

The upstream 0.4.1 `THIRD_PARTY_NOTICES.txt` attributes MIT to the project's own C++ source,
Detours, nlohmann/json and d3dx12, but explicitly excludes its binary kernel images from that grant.
It describes NVIDIA's unmodified DLSS-G runtime, derived FG/SR kernel packs and SR snippet as
third-party material retaining its own terms. The XeSS/MFG-Unlocker material also has separate
notices; not all of it is covered by the wrapper's MIT license. This is **not** a claim that the
complete release is MIT, GPL-compatible source, or freely redistributable under the app's license.
The application performs a user-directed download from upstream, does not link this payload into
itself, and preserves license/notice texts in `sm86-releases/<version>/notices` in app data.

- Official source: <https://github.com/sdli1995/dlssg_for_sm86>
- Upstream notices: <https://github.com/sdli1995/dlssg_for_sm86/blob/main/THIRD_PARTY_NOTICES.txt>
- Runtime layout, including optional SR kernels: <https://github.com/sdli1995/dlssg_for_sm86/blob/main/README.en.md>

## Synchronous HTTPS dependencies

`ureq` 3.4.2 is MIT/Apache-2.0; its `rustls` feature uses `rustls` (Apache-2.0/ISC/MIT),
`webpki-roots` (CDLA-Permissive-2.0), and `ring` (Apache-2.0 AND ISC; additional MIT notices for once_cell code). The distributed application retains the
licenses of these Rust dependencies. No async runtime, telemetry, or native-TLS dependency is added.
Sources and license texts are included in the corresponding crates' source packages:
<https://crates.io/crates/ureq/3.4.2>, <https://github.com/rustls/rustls>,
<https://github.com/rustls/webpki-roots>, <https://github.com/briansmith/ring>.

### NVIDIA DLSS catalog (external download only)

Source: https://github.com/NVIDIA/DLSS/tree/v310.9.16 . NVIDIA RTX SDKs license:
https://github.com/NVIDIA/DLSS/blob/v310.9.16/LICENSE.txt . SR/RR/FG DLLs are not bundled or hosted by Retoque.
The app obtains them directly from NVIDIA after confirmation, pins their hashes and preserves the complete license
in the user's catalog. Distribution as a standalone SDK is restricted; application distribution has additional
conditions, and the supplement specifies NVIDIA GPU interoperability and notification before commercial release.
These files are not relicensed under Retoque's GPL license. This note records upstream terms, not a legal opinion.

## ReShade (optional user-selected component)

ReShade by Patrick Mours: https://github.com/crosire/reshade/tree/v6.8.0 — BSD-3-Clause,
https://github.com/crosire/reshade/blob/v6.8.0/LICENSE.md. The exact license text is embedded in
`retoque-data/src/reshade.rs` and saved beside each imported/downloaded DLL. Binaries are fetched only from the
pinned official `https://reshade.me/downloads/ReShade_Setup_6.8.0_Addon.exe` after confirmation or imported from a
user file. Retoque does not ship the installer/DLL or any third-party add-on. Individual add-ons retain their
own licenses. No code is taken from or linked to RenoDX add-on binaries.
