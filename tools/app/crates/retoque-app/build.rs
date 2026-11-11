//! On a Windows target, embeds the executable's icon and version information (assets/retoque.ico), so Explorer,
//! shortcuts and the task manager show Retoque. Other targets get nothing here; the window icon is set at run time.
use std::{env, fs, path::PathBuf, process::Command};

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=assets/retoque.ico");
    println!("cargo:rerun-if-env-changed=WINDRES");
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        return;
    }
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let icon = manifest.join("assets").join("retoque.ico");
    let version = env::var("CARGO_PKG_VERSION").unwrap();
    let mut parts: Vec<u16> = version
        .split(['.', '-', '+'])
        .take(3)
        .map(|p| p.parse().unwrap_or(0))
        .collect();
    parts.resize(4, 0);
    let numbers = parts.iter().map(u16::to_string).collect::<Vec<_>>().join(",");
    let rc = out.join("retoque-app.rc");
    fs::write(
        &rc,
        format!(
            r#"1 ICON "{icon}"
1 VERSIONINFO
FILEVERSION {numbers}
PRODUCTVERSION {numbers}
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "CompanyName", "Retoque"
      VALUE "FileDescription", "Retoque"
      VALUE "FileVersion", "{version}"
      VALUE "OriginalFilename", "retoque-app.exe"
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
            icon = icon.display().to_string().replace('\\', "/"),
        ),
    )
    .unwrap();
    if env::var("CARGO_CFG_TARGET_ENV").as_deref() != Ok("gnu") {
        println!("cargo:warning=retoque-app: no icon embedded (only the GNU Windows target compiles resources here)");
        return;
    }
    let candidates = env::var("WINDRES")
        .ok()
        .into_iter()
        .chain(["x86_64-w64-mingw32-windres".to_owned(), "windres".to_owned()]);
    let object = out.join("retoque-app-resources.o");
    for windres in candidates {
        let status = Command::new(&windres)
            .args(["-O", "coff", "-i"])
            .arg(&rc)
            .arg("-o")
            .arg(&object)
            .status();
        if status.is_ok_and(|s| s.success()) {
            println!("cargo:rustc-link-arg-bin=retoque-app={}", object.display());
            return;
        }
    }
    println!("cargo:warning=retoque-app: windres not found, the executable has no icon (set WINDRES)");
}
