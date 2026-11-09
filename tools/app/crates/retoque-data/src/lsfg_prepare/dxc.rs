//! Minimal audited COM ABI for the pinned DXC release (dxcapi.h). No COM registration or subprocess.
//! Library handles outlive all interface objects. Each compile releases its result and blobs before unload.
use super::{Error, Result};
use libloading::Library;
use std::{
    ffi::c_void,
    path::{Path, PathBuf},
    ptr,
};
#[cfg(windows)]
type Wide = u16;
#[cfg(not(windows))]
type Wide = u32; // WinAdapter.h uses native wchar_t on Linux.
#[repr(C)]
struct Guid {
    a: u32,
    b: u16,
    c: u16,
    d: [u8; 8],
}
const CLASS: Guid = Guid {
    a: 0x73e22d93,
    b: 0xe6ce,
    c: 0x47f3,
    d: [0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0],
};
const COMPILER: Guid = Guid {
    a: 0x228b4687,
    b: 0x5a6a,
    c: 0x4730,
    d: [0x90, 0x0c, 0x97, 0x02, 0xb2, 0x20, 0x3f, 0x54],
};
const RESULT: Guid = Guid {
    a: 0x58346cda,
    b: 0xdde7,
    c: 0x4497,
    d: [0x94, 0x61, 0x6f, 0x87, 0xaf, 0x5e, 0x06, 0x59],
};
#[repr(C)]
struct Buffer {
    ptr: *const c_void,
    size: usize,
    encoding: u32,
}
type Object = *mut c_void;
type Create = unsafe extern "system" fn(*const Guid, *const Guid, *mut Object) -> i32;
type Release = unsafe extern "system" fn(Object) -> u32;
type Compile =
    unsafe extern "system" fn(Object, *const Buffer, *const *const Wide, u32, Object, *const Guid, *mut Object) -> i32;
type Status = unsafe extern "system" fn(Object, *mut i32) -> i32;
type GetBlob = unsafe extern "system" fn(Object, *mut Object) -> i32;
type BlobPtr = unsafe extern "system" fn(Object) -> *const u8;
type BlobSize = unsafe extern "system" fn(Object) -> usize;
struct Com(Object);
impl Com {
    // SAFETY: only obtained from successful DXC calls for the exact interfaces above. Slot signatures are from
    // dxcapi.h; IUnknown (0..2), Compiler3::Compile (3), OperationResult (3..5), Blob (3..4).
    unsafe fn slot<T: Copy>(&self, n: usize) -> T {
        unsafe {
            let vtable = *(self.0 as *const *const *const c_void);
            std::mem::transmute_copy(&*vtable.add(n))
        }
    }
    unsafe fn blob(&self) -> Result<Vec<u8>> {
        unsafe {
            let ptr = self.slot::<BlobPtr>(3)(self.0);
            let len = self.slot::<BlobSize>(4)(self.0);
            if ptr.is_null() || len > 4 * 1024 * 1024 {
                return Err(Error::Failed("Invalid DXC output size".into()));
            }
            Ok(std::slice::from_raw_parts(ptr, len).to_vec())
        }
    }
}
impl Drop for Com {
    fn drop(&mut self) {
        unsafe {
            self.slot::<Release>(2)(self.0);
        }
    }
}
fn checked(hr: i32, raw: Object) -> Result<Com> {
    if hr < 0 || raw.is_null() {
        Err(Error::Failed(format!("DXC HRESULT 0x{:08x}", hr as u32)))
    } else {
        Ok(Com(raw))
    }
}
pub const LIBRARIES: [&str; 2] = if cfg!(windows) {
    ["dxcompiler.dll", "dxil.dll"]
} else {
    ["libdxcompiler.so", "libdxil.so"]
};
const LIBRARY_HASHES: [&str; 2] = if cfg!(windows) {
    [
        "5888d3e590f5bfd8743484e7f31313996734adec5741506271a5c6368cbe8ba9",
        "0cf5fa4b90f74550166108b3870c883f4a85243976b246b1da3ea90ad5b92015",
    ]
} else {
    [
        "72832c39c5dfdf537586ca6500de94ef28cbf69d148ab3abe573da6ea209bad0",
        "51f6a6eb5c77391a86c03b82a79affdb7fa5ea9a3a67be37f5f573ca112ad931",
    ]
};
pub(super) fn verified_libraries(dir: &Path) -> bool {
    dir.is_absolute()
        && LIBRARIES.iter().zip(LIBRARY_HASHES).all(|(name, hash)| {
            super::read_bounded(&dir.join(name), 64 * 1024 * 1024).is_ok_and(|bytes| super::digest(&bytes) == hash)
        })
}
pub fn find(executable_dir: &Path, data_dir: &Path) -> Option<PathBuf> {
    [
        executable_dir.to_path_buf(),
        data_dir.join("dxc").join(super::download::RELEASE),
    ]
    .into_iter()
    .find(|p| verified_libraries(p))
}
pub struct Dxc {
    compiler: Com,
    _compiler_library: Library,
    _validator: Library,
}
impl Dxc {
    /// Explicit absolute paths only: never consult PATH, the CWD, or a system DXC installation.
    pub fn load(dir: &Path) -> Result<Self> {
        if !verified_libraries(dir) {
            return Err(Error::MissingCompiler);
        }
        unsafe {
            // Windows: constrain dependency resolution to this directory and System32.
            #[cfg(windows)]
            fn load(path: &Path) -> std::result::Result<Library, libloading::Error> {
                unsafe { libloading::os::windows::Library::load_with_flags(path, 0x100 | 0x800).map(Into::into) }
            }
            #[cfg(not(windows))]
            fn load(path: &Path) -> std::result::Result<Library, libloading::Error> {
                unsafe { Library::new(path) }
            }
            let validator = load(&dir.join(LIBRARIES[1])).map_err(|e| Error::Failed(e.to_string()))?;
            let library = load(&dir.join(LIBRARIES[0])).map_err(|e| Error::Failed(e.to_string()))?;
            let create = library
                .get::<Create>(b"DxcCreateInstance\0")
                .map_err(|e| Error::Failed(e.to_string()))?;
            let mut raw = ptr::null_mut();
            let hr = create(&CLASS, &COMPILER, &mut raw);
            Ok(Self {
                compiler: checked(hr, raw)?,
                _compiler_library: library,
                _validator: validator,
            })
        }
    }
    pub fn compile(&self, hlsl: &str) -> Result<Vec<u8>> {
        // Fixed arguments, no includes, source names or user-controlled compiler switches.
        let args: Vec<Vec<Wide>> = ["-T", "cs_6_0", "-E", "main", "-O3"]
            .iter()
            .map(|s| s.bytes().map(|b| b as Wide).chain(std::iter::once(0)).collect())
            .collect();
        let pointers: Vec<_> = args.iter().map(|a| a.as_ptr()).collect();
        let buffer = Buffer {
            ptr: hlsl.as_ptr().cast(),
            size: hlsl.len(),
            encoding: 65001,
        };
        unsafe {
            let mut raw = ptr::null_mut();
            let hr = self.compiler.slot::<Compile>(3)(
                self.compiler.0,
                &buffer,
                pointers.as_ptr(),
                pointers.len() as u32,
                ptr::null_mut(),
                &RESULT,
                &mut raw,
            );
            let result = checked(hr, raw)?;
            let mut status = 0;
            let hr = result.slot::<Status>(3)(result.0, &mut status);
            if hr < 0 {
                return Err(Error::Failed("DXC GetStatus failed".into()));
            }
            if status < 0 {
                let mut errors = ptr::null_mut();
                let hr = result.slot::<GetBlob>(5)(result.0, &mut errors);
                let message = checked(hr, errors)
                    .and_then(|b| b.blob())
                    .map(|b| {
                        String::from_utf8_lossy(&b)
                            .trim_end_matches('\0')
                            .chars()
                            .take(2000)
                            .collect()
                    })
                    .unwrap_or_else(|_| format!("DXC compile failed: {status}"));
                return Err(Error::Failed(message));
            }
            let mut blob = ptr::null_mut();
            let hr = result.slot::<GetBlob>(4)(result.0, &mut blob);
            let bytes = checked(hr, blob)?.blob()?;
            if bytes.len() < 32 || !bytes.starts_with(b"DXBC") {
                return Err(Error::Failed("DXC did not produce a DXIL container".into()));
            }
            Ok(bytes)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn only_pinned_libraries_are_loadable() {
        let temp = tempfile::tempdir().unwrap();
        for name in LIBRARIES {
            std::fs::write(temp.path().join(name), b"unofficial compiler").unwrap();
        }
        assert!(!verified_libraries(temp.path()));
        assert!(find(temp.path(), temp.path()).is_none());
        assert!(matches!(Dxc::load(temp.path()), Err(Error::MissingCompiler)));
    }
}
