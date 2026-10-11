//! The release key on disk: a 32-byte Ed25519 seed as one line of hex, in a file only its owner can read.
//!
//! The seed is never logged or printed by anything in this crate; [`public_hex`] is the only thing that leaves it.

use std::fs;
use std::io::{self, Write};
use std::path::Path;

use ed25519_dalek::SigningKey;

/// A fresh random seed from the operating system.
pub fn generate_seed() -> io::Result<[u8; 32]> {
    let mut seed = [0u8; 32];
    getrandom::fill(&mut seed).map_err(|e| io::Error::other(format!("no randomness: {e}")))?;
    Ok(seed)
}

/// Writes `seed` to a new file `path` with mode 600. Refuses to overwrite a file.
pub fn write_seed(path: &Path, seed: &[u8; 32]) -> io::Result<()> {
    let mut options = fs::OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    std::os::unix::fs::OpenOptionsExt::mode(&mut options, 0o600);
    let mut file = options.open(path)?;
    writeln!(file, "{}", to_hex(seed))?;
    file.sync_all()
}

/// Reads a seed file. On Unix a file that group or others can access is refused: a key that was readable has to
/// be treated as published.
pub fn read_seed(path: &Path) -> io::Result<[u8; 32]> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mode = fs::metadata(path)?.permissions().mode();
        if mode & 0o077 != 0 {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                format!(
                    "{} has mode {:o}: a release key must be readable by its owner only (chmod 600)",
                    path.display(),
                    mode & 0o777
                ),
            ));
        }
    }
    let text = fs::read_to_string(path)?;
    from_hex::<32>(text.trim()).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("{} is not a chunky key file (64 hex digits)", path.display()),
        )
    })
}

/// The public key of `seed` in hex: what goes into the bootloader.
pub fn public_hex(seed: &[u8; 32]) -> String {
    to_hex(&SigningKey::from_bytes(seed).verifying_key().to_bytes())
}

/// Lower-case hex.
pub fn to_hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// Parses exactly `N` bytes of hex.
pub fn from_hex<const N: usize>(text: &str) -> Option<[u8; N]> {
    if text.len() != 2 * N || !text.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    let mut out = [0u8; N];
    for (byte, pair) in out.iter_mut().zip(text.as_bytes().chunks(2)) {
        *byte = u8::from_str_radix(std::str::from_utf8(pair).ok()?, 16).ok()?;
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_round_trip_and_garbage() {
        let bytes = [0xAB; 32];
        assert_eq!(from_hex::<32>(&to_hex(&bytes)), Some(bytes));
        assert_eq!(from_hex::<32>("zz"), None);
        assert_eq!(from_hex::<2>("+1+2"), None);
        assert_eq!(from_hex::<2>("é1"), None);
        assert_eq!(from_hex::<32>(""), None);
    }
}
