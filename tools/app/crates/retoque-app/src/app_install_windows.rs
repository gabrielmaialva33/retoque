//! Windows integration. All entry points are fixed to this user's Programs/Retoque folder.
use super::*;
use ::windows::{
    Win32::{
        Foundation::{CloseHandle, WAIT_OBJECT_0},
        System::{
            Com::{
                CLSCTX_INPROC_SERVER, COINIT_APARTMENTTHREADED, CoCreateInstance, CoInitializeEx, CoTaskMemFree,
                CoUninitialize, IPersistFile,
            },
            Threading::{OpenProcess, PROCESS_SYNCHRONIZE, WaitForSingleObject},
        },
        UI::{
            Shell::{
                FOLDERID_Desktop, FOLDERID_LocalAppData, FOLDERID_Programs, IShellLinkW, KF_FLAG_DEFAULT,
                SHGetKnownFolderPath, ShellLink,
            },
            WindowsAndMessaging::{
                IDYES, MB_DEFBUTTON2, MB_ICONERROR, MB_ICONQUESTION, MB_YESNO, MessageBoxW, SW_SHOWNORMAL,
            },
        },
    },
    core::{GUID, Interface, PCWSTR},
};
use std::{
    os::windows::{ffi::OsStrExt, process::CommandExt},
    process::Command,
};
use winreg::{
    RegKey,
    enums::{HKEY_CURRENT_USER, KEY_READ, REG_CREATED_NEW_KEY},
};

fn wide(text: impl AsRef<std::ffi::OsStr>) -> Vec<u16> {
    text.as_ref().encode_wide().chain(Some(0)).collect()
}
fn win_error(e: ::windows::core::Error) -> io::Error {
    io::Error::other(e.to_string())
}
fn folder(id: &GUID) -> io::Result<PathBuf> {
    unsafe {
        let ptr = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, None).map_err(win_error)?;
        let path = PathBuf::from(std::ffi::OsString::from_wide(ptr.as_wide()));
        CoTaskMemFree(Some(ptr.0.cast()));
        Ok(path)
    }
}
use std::os::windows::ffi::OsStringExt;

pub fn paths() -> io::Result<Paths> {
    Paths::new(
        &folder(&FOLDERID_LocalAppData)?,
        &folder(&FOLDERID_Programs)?,
        &folder(&FOLDERID_Desktop)?,
    )
}

fn check_registry_absent() -> io::Result<()> {
    match RegKey::predef(HKEY_CURRENT_USER).open_subkey_with_flags(UNINSTALL_KEY, KEY_READ) {
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(e),
        Ok(_) => Err(io::Error::new(
            io::ErrorKind::AlreadyExists,
            "Retoque uninstall registration already exists",
        )),
    }
}

pub fn prepare() -> io::Result<InstallPlan> {
    check_registry_absent()?;
    InstallPlan::prepare(paths()?, &std::env::current_exe()?)
}

fn register(paths: &Paths, version: &str, bytes: u64, new: bool) -> io::Result<()> {
    let (key, disposition) = RegKey::predef(HKEY_CURRENT_USER).create_subkey(UNINSTALL_KEY)?;
    if new && disposition != REG_CREATED_NEW_KEY {
        return Err(io::Error::other("registration appeared during install"));
    }
    let result = (|| {
        for (name, value) in registry_values(paths, version, bytes) {
            match value {
                RegistryValue::Text(text) => key.set_value(name, &text)?,
                RegistryValue::Dword(number) => key.set_value(name, &number)?,
            }
        }
        Ok(())
    })();
    if result.is_err() && new {
        // We created this key in this call; even a failure on the first value must remove it.
        drop(key);
        RegKey::predef(HKEY_CURRENT_USER).delete_subkey_all(UNINSTALL_KEY)?;
    }
    result
}

struct Com;
impl Com {
    fn start() -> io::Result<Self> {
        unsafe {
            CoInitializeEx(None, COINIT_APARTMENTTHREADED).ok().map_err(win_error)?;
        }
        Ok(Self)
    }
}
impl Drop for Com {
    fn drop(&mut self) {
        unsafe {
            CoUninitialize();
        }
    }
}

fn shortcut(target: &Path, link_path: &Path) -> io::Result<()> {
    plain_path(link_path)?;
    // Reserve the name without overwriting a preexisting shortcut.
    let reserved = fs::OpenOptions::new().write(true).create_new(true).open(link_path)?;
    drop(reserved);
    let result = (|| unsafe {
        let link: IShellLinkW = CoCreateInstance(&ShellLink, None, CLSCTX_INPROC_SERVER).map_err(win_error)?;
        let exe = wide(target);
        let cwd = wide(target.parent().unwrap());
        link.SetPath(PCWSTR(exe.as_ptr())).map_err(win_error)?;
        link.SetWorkingDirectory(PCWSTR(cwd.as_ptr())).map_err(win_error)?;
        link.SetIconLocation(PCWSTR(exe.as_ptr()), 0).map_err(win_error)?;
        link.SetShowCmd(SW_SHOWNORMAL).map_err(win_error)?;
        let persist: IPersistFile = link.cast().map_err(win_error)?;
        persist.Save(PCWSTR(wide(link_path).as_ptr()), true).map_err(win_error)
    })();
    if result.is_err() {
        let _ = fs::remove_file(link_path);
    }
    result
}

pub fn install(plan: &InstallPlan) -> io::Result<()> {
    check_registry_absent()?;
    plan.check_destination()?;
    let _com = Com::start()?;
    plan.copy()?;
    let mut links = Vec::new();
    let result = (|| {
        for link in &plan.paths.shortcuts {
            fs::create_dir_all(link.parent().unwrap())?;
            shortcut(&plan.paths.exe(), link)?;
            links.push(link);
        }
        register(&plan.paths, version(), plan.bytes(), true)
    })();
    if let Err(error) = result {
        for link in links {
            let _ = fs::remove_file(link);
        }
        fs::remove_dir_all(&plan.paths.install)?;
        return Err(error);
    }
    Ok(())
}

pub fn relaunch() -> io::Result<()> {
    let p = paths()?;
    Command::new(p.exe()).current_dir(&p.install).spawn()?;
    Ok(())
}

struct Lock {
    file: fs::File,
}
impl Lock {
    fn take(install: &Path) -> io::Result<Self> {
        Self::open(install, false)
    }
    fn open(install: &Path, wait: bool) -> io::Result<Self> {
        let path = install.join(".retoque-app.lock");
        plain_path(&path)?;
        let file = fs::OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(&path)?;
        if wait {
            file.lock()?;
        } else {
            file.try_lock().map_err(io::Error::other)?;
        }
        Ok(Self { file })
    }
}
impl Drop for Lock {
    fn drop(&mut self) {
        let _ = self.file.unlock();
    }
}

/// The staged helper is a copy of this trusted executable, not a command from release metadata.
pub fn schedule_update(staged: app_release::Staged) -> io::Result<()> {
    let p = paths()?;
    if !p.is_installed(&std::env::current_exe()?) {
        return Err(io::Error::other("install the app before updating it"));
    }
    staged.manifest.verify(&staged.dir.path().join("payload"))?;
    replacement_steps(&p.install, staged.dir.path())?;
    let helper = staged.dir.path().join("updater.exe");
    fs::copy(std::env::current_exe()?, &helper)?;
    Command::new(helper)
        .arg("--apply-app-update")
        .arg(std::process::id().to_string())
        .current_dir(&p.install)
        .creation_flags(0x08000000 | 0x00000008)
        .spawn()?;
    let _ = staged.dir.keep();
    Ok(())
}

fn apply_update(pid: u32) -> io::Result<()> {
    let p = paths()?;
    let me = std::env::current_exe()?;
    let stage = me.parent().ok_or_else(|| io::Error::other("missing update staging"))?;
    replacement_steps(&p.install, stage)?;
    if me.file_name() != Some(std::ffi::OsStr::new("updater.exe")) {
        return Err(io::Error::other("invalid update helper"));
    }
    let _lock = Lock::take(&p.install)?;
    // Capture the process handle before waiting; PID reuse cannot turn a successful wait into another process.
    unsafe {
        match OpenProcess(PROCESS_SYNCHRONIZE, false, pid) {
            Ok(handle) => {
                let result = WaitForSingleObject(handle, 120_000);
                let _ = CloseHandle(handle);
                if result != WAIT_OBJECT_0 {
                    return Err(io::Error::other("app did not exit; update was not applied"));
                }
            }
            Err(e) if e.code().0 as u32 == 0x80070057 => {} // parent already exited
            Err(e) => return Err(win_error(e)),
        }
    }
    let manifest_path = stage.join("manifest.json");
    plain_path(&manifest_path)?;
    let bytes = fs::read(&manifest_path)?;
    if bytes.len() > 2 * 1024 * 1024 {
        return Err(io::Error::other("update manifest too large"));
    }
    let manifest: app_release::Manifest = serde_json::from_slice(&bytes)?;
    if !app_release::newer(&manifest.release.version, version()) {
        return Err(io::Error::other("app update is not newer"));
    }
    manifest.verify(&stage.join("payload"))?;
    let steps = replacement_steps(&p.install, stage)?;
    replace(&steps, &stage.join("journal.json"))?;
    let result = (|| {
        register(
            &p,
            &manifest.release.version,
            manifest.files.values().map(|f| f.size).sum(),
            false,
        )?;
        // The new process waits for our lock before cleanup. A failed spawn can still roll back under this lock.
        fs::write(stage.join("committed"), b"1")?;
        Command::new(p.exe())
            .arg("--finish-app-update")
            .current_dir(&p.install)
            .spawn()?;
        Ok(())
    })();
    if let Err(error) = result {
        let _ = fs::remove_file(stage.join("committed"));
        rollback(&steps)?;
        fs::remove_file(stage.join("journal.json"))?;
        let bytes = ROOTS
            .iter()
            .filter_map(|name| fs::metadata(p.install.join(name)).ok())
            .map(|m| m.len())
            .sum();
        register(&p, version(), bytes, false)?;
        return Err(error);
    }
    Ok(())
}

fn message(text: &str, confirm: bool) -> bool {
    unsafe {
        MessageBoxW(
            None,
            PCWSTR(wide(text).as_ptr()),
            PCWSTR(wide("Retoque").as_ptr()),
            if confirm {
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2
            } else {
                MB_ICONERROR
            },
        ) == IDYES
    }
}

fn uninstall() -> io::Result<()> {
    let p = paths()?;
    if !p.is_installed(&std::env::current_exe()?) {
        return Err(io::Error::other("uninstall must run from the installed app"));
    }
    plain_path(&p.install)?;
    let lang = crate::i18n::Lang::from_system();
    if !message(crate::i18n::t(lang, "app_install.uninstall_body"), true) {
        return Ok(());
    }
    let lock = Lock::take(&p.install)?;
    // Refuse a junction anywhere before cmd's recursive removal.
    fn check_tree(path: &Path) -> io::Result<()> {
        plain_path(path)?;
        if path.is_dir() {
            for item in fs::read_dir(path)? {
                check_tree(&item?.path())?;
            }
        }
        Ok(())
    }
    check_tree(&p.install)?;
    let ready = p.install.join(".retoque-uninstall-ready");
    plain_path(&ready)?;
    if ready.try_exists()? {
        fs::remove_file(&ready)?;
    }
    let script = p.install.join(format!(".retoque-uninstall-{}.cmd", std::process::id()));
    plain_path(&script)?;
    let mut file = fs::OpenOptions::new().write(true).create_new(true).open(&script)?;
    use io::Write;
    // Paths are passed through environment variables, never pasted into batch source. Delayed expansion is off.
    file.write_all(b"@echo off\r\nsetlocal DisableDelayedExpansion\r\n:wait\r\ntasklist /FI \"PID eq %RETOQUE_UNINSTALL_PID%\" /NH | findstr /R /C:\"[ ]%RETOQUE_UNINSTALL_PID%[ ]\" >nul\r\nif errorlevel 1 goto remove\r\nping -n 2 127.0.0.1 >nul\r\ngoto wait\r\n:remove\r\nif not exist \"%RETOQUE_UNINSTALL_DIR%\\.retoque-uninstall-ready\" exit /b 1\r\ncd /d \"%SystemRoot%\"\r\nrmdir /s /q \"%RETOQUE_UNINSTALL_DIR%\"\r\n")?;
    file.sync_all()?;
    drop(file);
    // Start waiting before removing registration; if spawn fails the user can retry via Apps & features.
    let spawn = Command::new(system_exe("cmd.exe")?)
        .raw_arg("/D /Q /V:OFF /C \"\"%RETOQUE_UNINSTALL_SCRIPT%\"\"")
        .env("RETOQUE_UNINSTALL_SCRIPT", &script)
        .env("RETOQUE_UNINSTALL_DIR", &p.install)
        .env("RETOQUE_UNINSTALL_PID", std::process::id().to_string())
        .current_dir(std::env::var_os("SystemRoot").ok_or_else(|| io::Error::other("missing SystemRoot"))?)
        .creation_flags(0x08000000 | 0x00000008)
        .spawn();
    if let Err(e) = spawn {
        let _ = fs::remove_file(script);
        return Err(e);
    }
    for link in &p.shortcuts {
        plain_path(link)?;
        match fs::remove_file(link) {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::NotFound => {}
            Err(e) => return Err(e),
        }
    }
    match RegKey::predef(HKEY_CURRENT_USER).delete_subkey_all(UNINSTALL_KEY) {
        Ok(()) => {}
        Err(e) if e.kind() == io::ErrorKind::NotFound => {}
        Err(e) => return Err(e),
    }
    fs::write(ready, b"1")?;
    drop(lock);
    Ok(())
}

fn system_exe(name: &str) -> io::Result<PathBuf> {
    let root = std::env::var_os("SystemRoot").ok_or_else(|| io::Error::other("missing SystemRoot"))?;
    Ok(PathBuf::from(root).join("System32").join(name))
}

fn cleanup(wait: bool) -> io::Result<()> {
    let p = paths()?;
    if !p.is_installed(&std::env::current_exe()?) {
        return Ok(());
    }
    let _lock = Lock::open(&p.install, wait)?;
    let old = p.install.join(OLD_EXE);
    plain_path(&old)?;
    let mut committed = Vec::new();
    for entry in fs::read_dir(&p.install)? {
        let entry = entry?;
        if entry.file_name().to_string_lossy().starts_with(".retoque-update-") {
            let stage = entry.path();
            plain_path(&stage)?;
            if stage.join("committed").is_file() {
                committed.push(stage);
            } else if stage.join("journal.json").exists() {
                return Err(io::Error::other(
                    "interrupted app update; keep the program folder and its backup for recovery",
                ));
            }
        }
    }
    if !committed.is_empty() && old.exists() {
        fs::remove_file(&old)?;
    }
    for stage in committed {
        if fs::remove_dir_all(&stage).is_err() && stage.is_dir() {
            // Windows may still hold updater.exe. Preserve the retry marker even if deletion was partial.
            fs::write(stage.join("committed"), b"1")?;
        }
    }
    Ok(())
}

/// Called before opening the GUI or initializing app data. Returns true for an app-management invocation.
pub fn startup() -> bool {
    let args: Vec<_> = std::env::args().skip(1).collect();
    let result = match args.first().map(String::as_str) {
        Some("--uninstall-app") if args.len() == 1 => uninstall(),
        Some("--apply-app-update") if args.len() == 2 => {
            args[1].parse::<u32>().map_err(io::Error::other).and_then(apply_update)
        }
        _ => {
            if let Err(e) = cleanup(args.as_slice() == ["--finish-app-update"]) {
                message(
                    &format!(
                        "{}\n\n{}",
                        crate::i18n::t(crate::i18n::Lang::from_system(), "app_install.failed"),
                        e
                    ),
                    false,
                );
                return true;
            }
            return false;
        }
    };
    if let Err(e) = result {
        let lang = crate::i18n::Lang::from_system();
        // Native fallback has a localized action and an explicit technical detail, never a silent failure.
        message(
            &format!("{}\n\n{}", crate::i18n::t(lang, "app_install.failed"), e),
            false,
        );
    }
    true
}
