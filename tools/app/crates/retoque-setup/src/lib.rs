//! The Windows setup (`Retoque-Setup-<version>.exe`) is this crate's small executable with the app's release bundle
//! appended to it by `tools/app/scripts/make_setup.py`:
//!
//! ```text
//! [stub exe][retoque-app-<v>-windows-x64.zip][SHA256SUMS][release.json][trailer]
//! ```
//!
//! The trailer is fixed size, at the very end: the u64 little-endian lengths of the zip, `SHA256SUMS` and
//! `release.json`, the SHA-256 of the zip, then [`MAGIC`]. Everything here is pure file handling, so it is built and
//! tested on every platform; only `main.rs` touches Windows.
//!
//! At run time the setup reads the bundle from its own file, checks every length and the zip hash, checks that the
//! release metadata, `SHA256SUMS` and zip agree (the same check the app's update applies to a download), then
//! unpacks the zip with the app's own validated unpacker into a fresh private folder. It never writes elsewhere.
use retoque_data::{
    app_release,
    payload_fetch::{self, Release},
};
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::{self, Read, Seek, SeekFrom, Write},
    path::{Path, PathBuf},
};

/// Identifies the trailer and its layout version. The last bytes of a setup file.
pub const MAGIC: [u8; 10] = *b"RTQSETUP1\0";
/// Three u64 lengths, the zip's SHA-256 and the magic.
pub const TRAILER_LEN: usize = 3 * 8 + 32 + MAGIC.len();
/// The same ceiling the app applies to a downloaded app archive.
pub const MAX_ZIP: u64 = payload_fetch::MAX_DOWNLOAD;
pub const MAX_SUMS: u64 = payload_fetch::MAX_SUMS_BYTES;
pub const MAX_RELEASE_JSON: u64 = app_release::MAX_SETUP_RELEASE;

/// The names the bundle's parts get in the setup's private folder; the app's `--setup <dir>` reads them there.
pub const ZIP_NAME: &str = app_release::SETUP_ZIP;
pub const SUMS_NAME: &str = app_release::SETUP_SUMS;
pub const RELEASE_NAME: &str = app_release::SETUP_RELEASE;

fn invalid(text: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, text)
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Trailer {
    pub zip_len: u64,
    pub sums_len: u64,
    pub release_len: u64,
    pub zip_sha256: [u8; 32],
}

impl Trailer {
    pub fn new(zip: &[u8], sums: &[u8], release_json: &[u8]) -> Self {
        Self {
            zip_len: zip.len() as u64,
            sums_len: sums.len() as u64,
            release_len: release_json.len() as u64,
            zip_sha256: Sha256::digest(zip).into(),
        }
    }

    pub fn encode(&self) -> [u8; TRAILER_LEN] {
        let mut out = [0u8; TRAILER_LEN];
        out[0..8].copy_from_slice(&self.zip_len.to_le_bytes());
        out[8..16].copy_from_slice(&self.sums_len.to_le_bytes());
        out[16..24].copy_from_slice(&self.release_len.to_le_bytes());
        out[24..56].copy_from_slice(&self.zip_sha256);
        out[56..].copy_from_slice(&MAGIC);
        out
    }

    /// Parse and bound a trailer. Every length must be non-zero and within the app's own download limits.
    pub fn decode(bytes: &[u8]) -> io::Result<Self> {
        if bytes.len() != TRAILER_LEN || bytes[56..] != MAGIC {
            return Err(invalid("this file carries no Retoque setup bundle"));
        }
        let number = |at: usize| u64::from_le_bytes(bytes[at..at + 8].try_into().unwrap());
        let trailer = Self {
            zip_len: number(0),
            sums_len: number(8),
            release_len: number(16),
            zip_sha256: bytes[24..56].try_into().unwrap(),
        };
        if !(1..=MAX_ZIP).contains(&trailer.zip_len)
            || !(1..=MAX_SUMS).contains(&trailer.sums_len)
            || !(1..=MAX_RELEASE_JSON).contains(&trailer.release_len)
        {
            return Err(invalid("setup bundle part has an invalid size"));
        }
        Ok(trailer)
    }

    /// Where the zip starts in a file of `file_len` bytes. The parts must fit after a non-empty stub.
    pub fn zip_offset(&self, file_len: u64) -> io::Result<u64> {
        // Bounded by decode, so the sum cannot overflow.
        let bundle = self.zip_len + self.sums_len + self.release_len + TRAILER_LEN as u64;
        file_len
            .checked_sub(bundle)
            .filter(|stub| *stub > 0)
            .ok_or_else(|| invalid("setup bundle is truncated"))
    }
}

/// The stub followed by the bundle and its trailer: what `make_setup.py` writes, for tests.
pub fn append(stub: &[u8], zip: &[u8], sums: &[u8], release_json: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(stub.len() + zip.len() + sums.len() + release_json.len() + TRAILER_LEN);
    for part in [stub, zip, sums, release_json] {
        out.extend_from_slice(part);
    }
    out.extend_from_slice(&Trailer::new(zip, sums, release_json).encode());
    out
}

#[derive(Debug)]
pub struct Bundle {
    pub zip: Vec<u8>,
    pub sums: Vec<u8>,
    pub release_json: Vec<u8>,
}

fn read_exact_len(file: &mut impl Read, len: u64) -> io::Result<Vec<u8>> {
    let mut out = Vec::with_capacity(len as usize);
    file.take(len).read_to_end(&mut out)?;
    if out.len() as u64 != len {
        return Err(invalid("setup bundle is truncated"));
    }
    Ok(out)
}

/// Read the bundle from a setup file (its own executable at run time). Only the bounded parts are read.
pub fn read_bundle<F: Read + Seek>(file: &mut F) -> io::Result<Bundle> {
    let file_len = file.seek(SeekFrom::End(0))?;
    if file_len < TRAILER_LEN as u64 {
        return Err(invalid("this file carries no Retoque setup bundle"));
    }
    file.seek(SeekFrom::Start(file_len - TRAILER_LEN as u64))?;
    let trailer = Trailer::decode(&read_exact_len(file, TRAILER_LEN as u64)?)?;
    file.seek(SeekFrom::Start(trailer.zip_offset(file_len)?))?;
    let zip = read_exact_len(file, trailer.zip_len)?;
    if <[u8; 32]>::from(Sha256::digest(&zip)) != trailer.zip_sha256 {
        return Err(invalid(
            "setup bundle is damaged: the app archive does not match its SHA-256",
        ));
    }
    let sums = read_exact_len(file, trailer.sums_len)?;
    let release_json = read_exact_len(file, trailer.release_len)?;
    Ok(Bundle {
        zip,
        sums,
        release_json,
    })
}

impl Bundle {
    /// The release metadata, accepted only when it, `SHA256SUMS` and the zip agree as an app update requires.
    pub fn release(&self) -> io::Result<Release> {
        let release: Release =
            serde_json::from_slice(&self.release_json).map_err(|_| invalid("setup release metadata is not valid"))?;
        app_release::verify_bundle(&release, &self.zip, &self.sums)?;
        Ok(release)
    }

    /// Write the three parts and unpack the zip into `dir`, a fresh folder this process created. Returns the app.
    pub fn extract(&self, dir: &Path) -> io::Result<PathBuf> {
        self.release()?;
        if fs::read_dir(dir)?.next().is_some() {
            return Err(io::Error::new(
                io::ErrorKind::AlreadyExists,
                "setup folder is not empty",
            ));
        }
        for (name, bytes) in [
            (ZIP_NAME, &self.zip),
            (SUMS_NAME, &self.sums),
            (RELEASE_NAME, &self.release_json),
        ] {
            let mut file = fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(dir.join(name))?;
            file.write_all(bytes)?;
            file.sync_all()?;
        }
        app_release::unpack_into(&self.zip, dir)?;
        let app = dir.join("payload").join(app_release::EXE);
        if !fs::symlink_metadata(&app).is_ok_and(|m| m.is_file()) {
            return Err(invalid("setup bundle has no app"));
        }
        Ok(app)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn fixture() -> (Vec<u8>, Vec<u8>, Vec<u8>) {
        let (release, zip, sums) = app_release::synthetic_app_fixture(None);
        (zip, sums, serde_json::to_vec(&release).unwrap())
    }

    #[test]
    fn trailer_round_trip() {
        let (zip, sums, json) = fixture();
        let trailer = Trailer::new(&zip, &sums, &json);
        let encoded = trailer.encode();
        assert_eq!(&encoded[TRAILER_LEN - MAGIC.len()..], &MAGIC);
        assert_eq!(Trailer::decode(&encoded).unwrap(), trailer);
        let file = append(b"MZ stub", &zip, &sums, &json);
        assert_eq!(trailer.zip_offset(file.len() as u64).unwrap(), 7);
        let bundle = read_bundle(&mut Cursor::new(&file)).unwrap();
        assert_eq!((bundle.zip, bundle.sums, bundle.release_json), (zip, sums, json));
    }

    #[test]
    fn wrong_magic_truncation_and_bad_sizes_are_refused() {
        let (zip, sums, json) = fixture();
        let file = append(b"MZ stub", &zip, &sums, &json);
        // No bundle at all, or a cut-off file.
        assert!(read_bundle(&mut Cursor::new(b"MZ stub only")).is_err());
        assert!(read_bundle(&mut Cursor::new(&file[..file.len() - 1])).is_err());
        let mut magic = file.clone();
        *magic.last_mut().unwrap() = b'X';
        assert!(read_bundle(&mut Cursor::new(&magic)).is_err());
        // The bundle must leave room for a stub before it.
        let no_stub = append(b"", &zip, &sums, &json);
        assert!(read_bundle(&mut Cursor::new(&no_stub)).is_err());
        let trailer = Trailer::new(&zip, &sums, &json);
        for (index, bad) in [
            (0, 0),
            (0, MAX_ZIP + 1),
            (1, 0),
            (1, MAX_SUMS + 1),
            (2, 0),
            (2, MAX_RELEASE_JSON + 1),
        ] {
            let mut encoded = trailer.encode();
            encoded[index * 8..index * 8 + 8].copy_from_slice(&u64::to_le_bytes(bad));
            assert!(Trailer::decode(&encoded).is_err(), "{index} {bad}");
        }
        // A length larger than the file is truncation, never a read before the start.
        let mut huge = trailer.clone();
        huge.zip_len = MAX_ZIP;
        assert!(huge.zip_offset(file.len() as u64).is_err());
        assert!(Trailer::decode(&trailer.encode()[1..]).is_err());
    }

    #[test]
    fn hash_mismatch_and_disagreeing_metadata_are_refused() {
        let (zip, sums, json) = fixture();
        let mut file = append(b"MZ stub", &zip, &sums, &json);
        file[7 + zip.len() / 2] ^= 1;
        let error = read_bundle(&mut Cursor::new(&file)).unwrap_err();
        assert!(error.to_string().contains("SHA-256"), "{error}");
        // The trailer's own hash is right, but SHA256SUMS lists another zip.
        let wrong_sums = format!("{}  retoque-app-1.2.3-windows-x64.zip\n", "0".repeat(64)).into_bytes();
        let mut release: Release = serde_json::from_slice(&json).unwrap();
        release.sums_size = wrong_sums.len() as u64;
        let json = serde_json::to_vec(&release).unwrap();
        let bundle = read_bundle(&mut Cursor::new(append(b"MZ", &zip, &wrong_sums, &json))).unwrap();
        assert!(bundle.release().unwrap_err().to_string().contains("SHA256SUMS"));
        // Metadata for the payload asset instead of the app asset.
        release.asset = "retoque-payload-1.2.3-x64.zip".into();
        let json = serde_json::to_vec(&release).unwrap();
        let bundle = read_bundle(&mut Cursor::new(append(b"MZ", &zip, &sums, &json))).unwrap();
        assert!(bundle.release().is_err());
        let bundle = read_bundle(&mut Cursor::new(append(b"MZ", &zip, &sums, b"{"))).unwrap();
        assert!(bundle.release().is_err());
    }

    #[test]
    fn extract_writes_only_inside_its_folder() {
        let (zip, sums, json) = fixture();
        let bundle = read_bundle(&mut Cursor::new(append(b"MZ stub", &zip, &sums, &json))).unwrap();
        let root = tempfile::tempdir().unwrap();
        let dir = root.path().join("setup");
        fs::create_dir(&dir).unwrap();
        let app = bundle.extract(&dir).unwrap();
        assert_eq!(app, dir.join("payload").join(app_release::EXE));
        assert_eq!(fs::read(dir.join(ZIP_NAME)).unwrap(), zip);
        assert_eq!(fs::read(dir.join(SUMS_NAME)).unwrap(), sums);
        assert_eq!(fs::read(dir.join(RELEASE_NAME)).unwrap(), json);
        for required in payload_fetch::REQUIRED_FILES {
            assert!(dir.join("payload").join(required).is_file(), "{required}");
        }
        let mut names: Vec<_> = fs::read_dir(root.path())
            .unwrap()
            .map(|e| e.unwrap().file_name())
            .collect();
        names.sort();
        assert_eq!(names, ["setup"]);
        // A second extraction into the same folder is refused rather than mixing two bundles.
        assert!(bundle.extract(&dir).is_err());
    }

    #[test]
    fn unpack_refuses_traversal_and_forbidden_members() {
        for bad in [
            "../outside.txt",
            "Licenses/../../outside.txt",
            "nvngx_dlssnr.dll",
            "updater.exe",
        ] {
            let (release, zip, sums) = app_release::synthetic_app_fixture(Some(bad));
            let json = serde_json::to_vec(&release).unwrap();
            let bundle = read_bundle(&mut Cursor::new(append(b"MZ", &zip, &sums, &json))).unwrap();
            let root = tempfile::tempdir().unwrap();
            let dir = root.path().join("setup");
            fs::create_dir(&dir).unwrap();
            assert!(bundle.extract(&dir).is_err(), "{bad}");
            assert!(!root.path().join("outside.txt").exists());
            assert!(!dir.join("payload").join(app_release::EXE).exists(), "{bad}");
        }
    }
}
