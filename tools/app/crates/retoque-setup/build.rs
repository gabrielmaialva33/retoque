//! On the GNU Windows target, embeds the app's icon, version information and an `asInvoker` manifest, so Explorer
//! shows Retoque and Windows never elevates the per-user setup. Other targets get nothing here.
use std::{env, fs, path::PathBuf, process::Command};

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=setup.manifest");
    println!("cargo:rerun-if-changed=../retoque-app/assets/retoque.ico");
    println!("cargo:rerun-if-env-changed=WINDRES");
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        return;
    }
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let icon = manifest_dir.join("../retoque-app/assets/retoque.ico");
    let manifest = manifest_dir.join("setup.manifest");
    let version = env::var("CARGO_PKG_VERSION").unwrap();
    let mut parts: Vec<u16> = version
        .split(['.', '-', '+'])
        .take(3)
        .map(|p| p.parse().unwrap_or(0))
        .collect();
    parts.resize(4, 0);
    let numbers = parts.iter().map(u16::to_string).collect::<Vec<_>>().join(",");
    let quoted = |p: &PathBuf| p.display().to_string().replace('\\', "/");
    let rc = out.join("retoque-setup.rc");
    fs::write(
        &rc,
        format!(
            r#"1 ICON "{icon}"
1 24 "{manifest}"
1 VERSIONINFO
FILEVERSION {numbers}
PRODUCTVERSION {numbers}
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "CompanyName", "Retoque"
      VALUE "FileDescription", "Retoque Setup"
      VALUE "FileVersion", "{version}"
      VALUE "OriginalFilename", "retoque-setup.exe"
      VALUE "ProductName", "Retoque"
      VALUE "ProductVersion", "{version}"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"#,
            icon = quoted(&icon),
            manifest = quoted(&manifest),
        ),
    )
    .unwrap();
    if env::var("CARGO_CFG_TARGET_ENV").as_deref() != Ok("gnu") {
        println!("cargo:warning=retoque-setup: no resources embedded (only the GNU Windows target compiles them here)");
        return;
    }
    let candidates = env::var("WINDRES")
        .ok()
        .into_iter()
        .chain(["x86_64-w64-mingw32-windres".to_owned(), "windres".to_owned()]);
    let object = out.join("retoque-setup-resources.o");
    for windres in candidates {
        let status = Command::new(&windres)
            .args(["-O", "coff", "-i"])
            .arg(&rc)
            .arg("-o")
            .arg(&object)
            .status();
        if status.is_ok_and(|s| s.success()) {
            println!("cargo:rustc-link-arg-bin=retoque-setup={}", object.display());
            return;
        }
    }
    println!("cargo:warning=retoque-setup: windres not found, the setup has no icon or manifest (set WINDRES)");
}
