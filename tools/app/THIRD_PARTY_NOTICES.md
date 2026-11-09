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
