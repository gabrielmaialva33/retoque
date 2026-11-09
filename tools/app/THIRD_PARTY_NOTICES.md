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
`webpki-roots` (MPL-2.0). Dynamic loading uses `libloading` (ISC); gzip/tar extraction uses `flate2` (MIT/Apache-2.0)
and `tar` (MIT/Apache-2.0). No async runtime is introduced.

The Windows GNU target statically links the MinGW GCC C++ runtime to avoid requiring a MinGW installation on
end-user machines. GCC's runtime is GPL-3.0 with the GCC Runtime Library Exception 3.1; retain the applicable
runtime notices. See <https://www.gnu.org/licenses/gcc-exception-3.1.html>.
