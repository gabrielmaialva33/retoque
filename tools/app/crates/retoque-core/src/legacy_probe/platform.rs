use super::*;

#[cfg(target_os = "linux")]
fn proc_bytes(pid: u32, name: &str, limit: usize) -> Option<Vec<u8>> {
    let mut b = Vec::new();
    File::open(format!("/proc/{pid}/{name}"))
        .ok()?
        .take(limit as u64 + 1)
        .read_to_end(&mut b)
        .ok()?;
    (b.len() <= limit).then_some(b)
}
#[cfg(target_os = "linux")]
pub(super) fn identity(pid: u32) -> Option<String> {
    if pid == 0 {
        return None;
    }
    let b = proc_bytes(pid, "stat", 8192)?;
    let stat = std::str::from_utf8(&b).ok()?;
    let start = stat
        .rsplit_once(')')?
        .1
        .split_whitespace()
        .nth(19)?
        .parse::<u64>()
        .ok()?;
    let path = std::fs::read_link(format!("/proc/{pid}/exe")).ok()?;
    use std::os::unix::fs::MetadataExt;
    let metadata = std::fs::metadata(format!("/proc/{pid}/exe")).ok()?;
    Some(digest(
        format!(
            "{pid}:{start}:{}:{}:{}",
            path.to_string_lossy(),
            metadata.dev(),
            metadata.ino()
        )
        .as_bytes(),
    ))
}
#[cfg(target_os = "linux")]
pub(super) fn process(pid: u32, selected: Option<&Path>, deadline: Instant) -> Process {
    let before = identity(pid);
    let mut p = Process {
        identity: before.clone(),
        ..Default::default()
    };
    if before.is_none() {
        p.issue = Some("probe.process_unreadable");
        return p;
    }
    p.image_path = std::fs::read_link(format!("/proc/{pid}/exe")).ok();
    if let Some(path) = &p.image_path {
        // /proc's executable identifies the host process, not Wine's guest PE or its launcher.
        p.architecture = image(path, deadline).architecture;
    }
    let selected_requested = selected.is_some();
    let selected = selected.and_then(|path| path.canonicalize().ok());
    p.matches_executable = selected
        .as_ref()
        .map_or(!selected_requested && p.image_path.is_some(), |s| {
            p.image_path.as_ref() == Some(s)
        });
    if let Some(b) = proc_bytes(pid, "maps", READ_LIMIT) {
        p.modules_complete = true;
        for (index, line) in b.split(|c| *c == b'\n').enumerate() {
            if index >= INVENTORY_LIMIT || Instant::now() >= deadline {
                p.modules_complete = false;
                break;
            }
            let Ok(line) = std::str::from_utf8(line) else {
                p.modules_complete = false;
                continue;
            };
            // The first five fields contain addresses/permissions; retain only the pathname internally.
            // proc maps pads fields with spaces, so use the first absolute pathname after the fixed fields.
            if let Some(at) = line.find('/') {
                let path = &line[at..];
                if selected.as_ref().is_some_and(|s| s.as_os_str() == path) {
                    p.matches_executable = true;
                }
                let name = basename(path).to_ascii_lowercase();
                if APIS.iter().any(|(_, names)| names.contains(&name.as_str())) {
                    p.modules.push(name);
                }
            }
        }
    } else {
        p.issue = Some("probe.process_unreadable");
    }
    p.stable = before.is_some() && identity(pid) == before;
    p
}

#[cfg(windows)]
mod win {
    use super::*;
    use windows_sys::Win32::{
        Foundation::{CloseHandle, FILETIME, HANDLE},
        System::{ProcessStatus::*, Threading::*},
    };
    struct Handle(HANDLE);
    impl Drop for Handle {
        fn drop(&mut self) {
            unsafe {
                CloseHandle(self.0);
            }
        }
    }
    fn open(pid: u32) -> Option<Handle> {
        let h = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) };
        (!h.is_null()).then_some(Handle(h))
    }
    fn path(h: &Handle) -> Option<PathBuf> {
        let mut b = vec![0u16; 32768];
        let mut n = b.len() as u32;
        if unsafe { QueryFullProcessImageNameW(h.0, 0, b.as_mut_ptr(), &mut n) } == 0 {
            return None;
        }
        use std::os::windows::ffi::OsStringExt;
        Some(PathBuf::from(std::ffi::OsString::from_wide(&b[..n as usize])))
    }
    fn stamp(h: &Handle, pid: u32) -> Option<String> {
        let mut times: [FILETIME; 4] = unsafe { std::mem::zeroed() };
        let [a, b, c, d] = &mut times;
        if unsafe { GetProcessTimes(h.0, a, b, c, d) } == 0 {
            return None;
        }
        Some(digest(
            format!(
                "{pid}:{}:{}:{}",
                a.dwHighDateTime,
                a.dwLowDateTime,
                path(h)?.to_string_lossy()
            )
            .as_bytes(),
        ))
    }
    pub(super) fn identity(pid: u32) -> Option<String> {
        stamp(&open(pid)?, pid)
    }
    struct Windows {
        pid: u32,
        deadline: Instant,
        visited: usize,
        selected: Vec<[u32; 2]>,
        complete: bool,
    }
    unsafe extern "system" fn window(hwnd: windows_sys::Win32::Foundation::HWND, data: isize) -> i32 {
        use windows_sys::Win32::UI::WindowsAndMessaging::*;
        // Callback data is a stack object alive for this synchronous EnumWindows call.
        let state = unsafe { &mut *(data as *mut Windows) };
        state.visited += 1;
        if state.visited > INVENTORY_LIMIT || Instant::now() >= state.deadline {
            state.complete = false;
            return 0;
        }
        let mut pid = 0;
        unsafe {
            GetWindowThreadProcessId(hwnd, &mut pid);
        }
        if pid != state.pid {
            return 1;
        } // no title, style, geometry or process query for other owners
        if unsafe { IsWindowVisible(hwnd) } == 0 || unsafe { IsIconic(hwnd) } != 0 {
            return 1;
        }
        let mut rect = unsafe { std::mem::zeroed() };
        if unsafe { GetClientRect(hwnd, &mut rect) } == 0 {
            state.complete = false;
            return 1;
        }
        if let Some(size) = client_size(rect.right, rect.bottom) {
            state.selected.push(size);
        }
        1
    }
    fn geometry(pid: u32, deadline: Instant) -> Option<[u32; 2]> {
        let mut state = Windows {
            pid,
            deadline,
            visited: 0,
            selected: Vec::new(),
            complete: true,
        };
        let ok = unsafe {
            windows_sys::Win32::UI::WindowsAndMessaging::EnumWindows(Some(window), &mut state as *mut Windows as isize)
        };
        unique_geometry(&state.selected, state.complete && ok != 0)
    }
    pub(super) fn process(pid: u32, selected: Option<&Path>, deadline: Instant) -> Process {
        let Some(h) = open(pid) else {
            return Process {
                issue: Some("probe.process_unreadable"),
                ..Default::default()
            };
        };
        let before = stamp(&h, pid);
        let mut p = Process {
            identity: before.clone(),
            image_path: path(&h),
            ..Default::default()
        };
        let mut guest = 0u16;
        let mut native = 0u16;
        if unsafe { IsWow64Process2(h.0, &mut guest, &mut native) } != 0 {
            p.architecture = match if guest == 0 { native } else { guest } {
                0x14c => Architecture::X86,
                0x8664 => Architecture::X64,
                0xaa64 => Architecture::Arm64,
                _ => Architecture::Unknown,
            };
        }
        p.matches_executable = match selected {
            None => p.image_path.is_some(),
            Some(s) => s
                .canonicalize()
                .ok()
                .zip(p.image_path.as_ref().and_then(|p| p.canonicalize().ok()))
                .is_some_and(|(a, b)| a == b),
        };
        // Fixed-size module buffer: no system-wide process or unbounded Toolhelp snapshot.
        let modules = unsafe { OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, 0, pid) };
        if modules.is_null() {
            p.issue = Some("probe.process_unreadable");
        } else {
            let modules = Handle(modules);
            let mut handles = [std::ptr::null_mut(); INVENTORY_LIMIT];
            let mut needed = 0u32;
            let capacity = std::mem::size_of_val(&handles) as u32;
            if unsafe {
                K32EnumProcessModulesEx(modules.0, handles.as_mut_ptr(), capacity, &mut needed, LIST_MODULES_ALL)
            } != 0
            {
                p.modules_complete = needed <= capacity;
                let count = (needed as usize / std::mem::size_of_val(&handles[0])).min(INVENTORY_LIMIT);
                for handle in handles.iter().take(count) {
                    if Instant::now() >= deadline {
                        p.modules_complete = false;
                        break;
                    }
                    let mut name = [0u16; 260];
                    let n = unsafe { K32GetModuleBaseNameW(modules.0, *handle, name.as_mut_ptr(), name.len() as u32) }
                        as usize;
                    if n == 0 || n >= name.len() {
                        p.modules_complete = false;
                        continue;
                    }
                    let name = String::from_utf16_lossy(&name[..n]).to_ascii_lowercase();
                    if APIS.iter().any(|(_, names)| names.contains(&name.as_str())) {
                        p.modules.push(name);
                    }
                }
            } else {
                p.issue = Some("probe.process_unreadable");
            }
        }
        p.geometry = geometry(pid, deadline);
        p.stable = before.is_some() && identity(pid) == before;
        p
    }
}
#[cfg(windows)]
pub(super) fn identity(pid: u32) -> Option<String> {
    win::identity(pid)
}
#[cfg(windows)]
pub(super) fn process(pid: u32, selected: Option<&Path>, deadline: Instant) -> Process {
    win::process(pid, selected, deadline)
}

#[cfg(windows)]
pub(super) fn capture_available() -> Option<bool> {
    // Static capability query only; no CaptureItem, session, window or device is created.
    use windows::Win32::System::WinRT::{RO_INIT_MULTITHREADED, RoInitialize, RoUninitialize};
    if unsafe { RoInitialize(RO_INIT_MULTITHREADED) }.is_err() {
        return None;
    }
    let result = windows::Graphics::Capture::GraphicsCaptureSession::IsSupported().ok();
    unsafe {
        RoUninitialize();
    }
    result
}
#[cfg(not(windows))]
pub(super) fn capture_available() -> Option<bool> {
    None
}
#[cfg(not(any(windows, target_os = "linux")))]
pub(super) fn identity(_: u32) -> Option<String> {
    None
}
#[cfg(not(any(windows, target_os = "linux")))]
pub(super) fn process(_: u32, _: Option<&Path>, _: Instant) -> Process {
    Process::default()
}

#[cfg(any(windows, test))]
fn client_size(width: i32, height: i32) -> Option<[u32; 2]> {
    (width > 0 && height > 0).then_some([width as u32, height as u32])
}
#[cfg(any(windows, test))]
fn unique_geometry(selected: &[[u32; 2]], complete: bool) -> Option<[u32; 2]> {
    (complete && selected.len() == 1).then(|| selected[0])
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn fake_window_inventory_refuses_partial_ambiguous_and_invalid_geometry() {
        assert_eq!(unique_geometry(&[[1920, 1080]], true), Some([1920, 1080]));
        assert_eq!(unique_geometry(&[[1920, 1080]], false), None);
        assert_eq!(unique_geometry(&[[1920, 1080], [20, 30]], true), None);
        assert_eq!(unique_geometry(&[], true), None);
        assert_eq!(client_size(-1, 1080), None);
        assert_eq!(client_size(1920, 0), None);
    }
}

#[cfg(all(test, target_os = "linux"))]
mod selection_tests {
    use super::*;
    #[test]
    fn unreadable_explicit_selection_is_not_an_implicit_process_match() {
        let temp = tempfile::tempdir().unwrap();
        let p = process(
            std::process::id(),
            Some(&temp.path().join("missing.exe")),
            Instant::now(),
        );
        assert!(!p.matches_executable);
    }
}
