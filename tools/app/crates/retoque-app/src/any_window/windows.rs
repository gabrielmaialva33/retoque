//! A new private directory per launch; job membership is assigned atomically by CreateProcessW.
use super::{
    process::{Child, Spawned},
    protocol::Settings,
};
use std::{
    fs::{self, File},
    io,
    os::windows::{
        ffi::OsStrExt,
        io::{AsRawHandle, FromRawHandle, OwnedHandle},
    },
    path::Path,
    sync::atomic::{AtomicBool, Ordering},
};
use windows::{
    Win32::{
        Foundation::*,
        Security::SECURITY_ATTRIBUTES,
        System::{JobObjects::*, Pipes::CreatePipe, Threading::*},
    },
    core::{PCWSTR, PWSTR},
};

pub(super) const FILES: [&str; 3] = ["RetoqueWindowHost.exe", "OptiScaler.dll", "nvngx.dll_dlssnr.dll"];
fn err(e: windows::core::Error) -> io::Error {
    io::Error::other(e.to_string())
}
fn handle(h: &OwnedHandle) -> HANDLE {
    HANDLE(h.as_raw_handle())
}
fn wide(s: &std::ffi::OsStr) -> Vec<u16> {
    s.encode_wide().chain(Some(0)).collect()
}
fn pipe() -> io::Result<(OwnedHandle, OwnedHandle)> {
    let mut r = HANDLE::default();
    let mut w = HANDLE::default();
    let security = SECURITY_ATTRIBUTES {
        nLength: size_of::<SECURITY_ATTRIBUTES>() as u32,
        bInheritHandle: true.into(),
        ..Default::default()
    };
    // SAFETY: valid output pointers; successful CreatePipe transfers two handles to these owners.
    unsafe {
        CreatePipe(&mut r, &mut w, Some(&security), 0).map_err(err)?;
        Ok((OwnedHandle::from_raw_handle(r.0), OwnedHandle::from_raw_handle(w.0)))
    }
}
struct Attributes {
    _storage: Vec<usize>,
    list: LPPROC_THREAD_ATTRIBUTE_LIST,
}
impl Drop for Attributes {
    fn drop(&mut self) {
        unsafe {
            DeleteProcThreadAttributeList(self.list);
        }
    }
}
impl Attributes {
    fn new() -> io::Result<Self> {
        let mut size = 0;
        unsafe {
            let _ = InitializeProcThreadAttributeList(None, 2, None, &mut size);
        }
        if size == 0 {
            return Err(io::Error::last_os_error());
        }
        let mut storage = vec![0usize; size.div_ceil(size_of::<usize>())];
        let list = LPPROC_THREAD_ATTRIBUTE_LIST(storage.as_mut_ptr().cast());
        unsafe {
            InitializeProcThreadAttributeList(Some(list), 2, None, &mut size).map_err(err)?;
        }
        Ok(Self {
            _storage: storage,
            list,
        })
    }
}
struct JobChild {
    process: OwnedHandle,
    job: OwnedHandle,
}
impl Child for JobChild {
    fn try_wait(&mut self) -> io::Result<Option<i32>> {
        unsafe {
            match WaitForSingleObject(handle(&self.process), 0) {
                WAIT_TIMEOUT => Ok(None),
                WAIT_OBJECT_0 => {
                    let mut code = 0;
                    GetExitCodeProcess(handle(&self.process), &mut code).map_err(err)?;
                    Ok(Some(code as i32))
                }
                _ => Err(io::Error::last_os_error()),
            }
        }
    }
    fn kill_wait(&mut self) -> io::Result<i32> {
        unsafe {
            TerminateJobObject(handle(&self.job), 1).map_err(err)?;
            if WaitForSingleObject(handle(&self.process), 5000) != WAIT_OBJECT_0 {
                return Err(io::Error::other("host termination was not confirmed"));
            }
        }
        self.try_wait()?
            .ok_or_else(|| io::Error::other("host is still running"))
    }
}
impl Drop for JobChild {
    fn drop(&mut self) {
        unsafe {
            let _ = TerminateJobObject(handle(&self.job), 1);
        }
    }
}
pub(super) fn prepare(payload: &Path, settings: Option<&Settings>, cancelled: &AtomicBool) -> io::Result<Spawned> {
    if settings.is_none() {
        for name in FILES {
            if !payload.join(name).is_file() {
                return Err(io::Error::other("host payload is incomplete"));
            }
        }
        return launch(payload, None, &["--catalog".into()]);
    }
    let folder = tempfile::Builder::new().prefix("retoque-window-").tempdir()?;
    for name in FILES {
        if cancelled.load(Ordering::Acquire) {
            return Err(io::Error::new(io::ErrorKind::Interrupted, "launch cancelled"));
        }
        let source = payload.join(name);
        if !fs::symlink_metadata(&source)?.file_type().is_file() {
            return Err(io::Error::other("host payload must contain regular files"));
        }
        fs::copy(source, folder.path().join(name))?;
    }
    let args = if let Some(s) = settings {
        if !s.model.is_file() {
            return Err(io::Error::other("catalog model is no longer present"));
        }
        fs::write(folder.path().join("OptiScaler.ini"), s.ini().map_err(io::Error::other)?)?;
        s.arguments()
    } else {
        vec!["--catalog".into()]
    };
    if cancelled.load(Ordering::Acquire) {
        return Err(io::Error::new(io::ErrorKind::Interrupted, "launch cancelled"));
    }
    let directory = folder.path().to_owned();
    launch(&directory, Some(folder), &args)
}
fn launch(directory: &Path, folder: Option<tempfile::TempDir>, args: &[String]) -> io::Result<Spawned> {
    let exe = directory.join(FILES[0]);
    let exe_w = wide(exe.as_os_str());
    let cwd = wide(directory.as_os_str());
    // Only fixed flags, validated hexadecimal handles and decimal countdowns reach argv.
    if args
        .iter()
        .any(|s| !s.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-'))
    {
        return Err(io::Error::other("invalid host arguments"));
    }
    let mut cmd = wide(std::ffi::OsStr::new(&format!(
        "\"{}\" {}",
        exe.display(),
        args.join(" ")
    )));
    let (stdin_r, stdin_w) = pipe()?;
    let (stdout_r, stdout_w) = pipe()?;
    unsafe {
        SetHandleInformation(handle(&stdin_w), HANDLE_FLAG_INHERIT.0, HANDLE_FLAGS(0)).map_err(err)?;
        SetHandleInformation(handle(&stdout_r), HANDLE_FLAG_INHERIT.0, HANDLE_FLAGS(0)).map_err(err)?;
        let job = OwnedHandle::from_raw_handle(CreateJobObjectW(None, PCWSTR::null()).map_err(err)?.0);
        let info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
            BasicLimitInformation: JOBOBJECT_BASIC_LIMIT_INFORMATION {
                LimitFlags: JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
                ..Default::default()
            },
            ..Default::default()
        };
        SetInformationJobObject(
            handle(&job),
            JobObjectExtendedLimitInformation,
            (&info as *const JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(),
            size_of_val(&info) as u32,
        )
        .map_err(err)?;
        let attributes = Attributes::new()?;
        let inherited = [handle(&stdin_r), handle(&stdout_w)];
        let jobs = [handle(&job)];
        UpdateProcThreadAttribute(
            attributes.list,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST as usize,
            Some(inherited.as_ptr().cast()),
            size_of_val(&inherited),
            None,
            None,
        )
        .map_err(err)?;
        // Unlike assigning a job after spawn, this cannot leave an unowned child if the app closes during startup.
        UpdateProcThreadAttribute(
            attributes.list,
            0,
            PROC_THREAD_ATTRIBUTE_JOB_LIST as usize,
            Some(jobs.as_ptr().cast()),
            size_of_val(&jobs),
            None,
            None,
        )
        .map_err(err)?;
        let startup = STARTUPINFOEXW {
            StartupInfo: STARTUPINFOW {
                cb: size_of::<STARTUPINFOEXW>() as u32,
                dwFlags: STARTF_USESTDHANDLES,
                hStdInput: inherited[0],
                hStdOutput: inherited[1],
                // Host diagnostics must also obey JSON lines; unexpected stderr fails closed in the parser.
                hStdError: inherited[1],
                ..Default::default()
            },
            lpAttributeList: attributes.list,
        };
        let mut process = PROCESS_INFORMATION::default();
        CreateProcessW(
            PCWSTR(exe_w.as_ptr()),
            Some(PWSTR(cmd.as_mut_ptr())),
            None,
            None,
            true,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
            None,
            PCWSTR(cwd.as_ptr()),
            &startup.StartupInfo,
            &mut process,
        )
        .map_err(err)?;
        let process_handle = OwnedHandle::from_raw_handle(process.hProcess.0);
        drop(OwnedHandle::from_raw_handle(process.hThread.0));
        drop(stdin_r);
        drop(stdout_w);
        Ok(Spawned {
            child: Box::new(JobChild {
                process: process_handle,
                job,
            }),
            input: Box::new(File::from(stdin_w)),
            output: Box::new(File::from(stdout_r)),
            folder,
        })
    }
}
