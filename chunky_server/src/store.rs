//! The blob store: one object per unique chunk, named by its hash.

use std::collections::HashMap;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use chunky::ChunkHash;

/// Chunks by hash.
pub trait BlobStore {
    /// Stores `data` and returns its hash. Storing the same chunk twice is free.
    fn put(&mut self, data: &[u8]) -> io::Result<ChunkHash>;
    /// The chunk with this hash, if the store has it.
    fn get(&self, hash: &ChunkHash) -> io::Result<Option<Vec<u8>>>;
}

/// A store in memory.
#[derive(Default)]
pub struct MemStore {
    blobs: HashMap<ChunkHash, Vec<u8>>,
}

impl MemStore {
    /// Unique chunks held.
    pub fn len(&self) -> usize {
        self.blobs.len()
    }

    /// True when nothing is stored.
    pub fn is_empty(&self) -> bool {
        self.blobs.is_empty()
    }
}

impl BlobStore for MemStore {
    fn put(&mut self, data: &[u8]) -> io::Result<ChunkHash> {
        let hash = ChunkHash::of(data);
        self.blobs.entry(hash).or_insert_with(|| data.to_vec());
        Ok(hash)
    }

    fn get(&self, hash: &ChunkHash) -> io::Result<Option<Vec<u8>>> {
        Ok(self.blobs.get(hash).cloned())
    }
}

/// A store in a directory: `ab/abcdef...` per chunk. A chunk is written under a temporary name and renamed,
/// and checked against its name when read.
pub struct DirStore {
    root: PathBuf,
}

impl DirStore {
    /// Opens (and creates) the store at `root`.
    pub fn open(root: impl AsRef<Path>) -> io::Result<Self> {
        fs::create_dir_all(root.as_ref())?;
        Ok(Self {
            root: root.as_ref().to_path_buf(),
        })
    }

    /// Every chunk in the store with its size, by hash. Entries that are not blobs (temporary files) are skipped.
    pub fn list(&self) -> io::Result<Vec<(ChunkHash, u64)>> {
        let mut out = Vec::new();
        for dir in fs::read_dir(&self.root)? {
            let dir = dir?;
            if !dir.file_type()?.is_dir() {
                continue;
            }
            for entry in fs::read_dir(dir.path())? {
                let entry = entry?;
                let name = entry.file_name();
                let Some(bytes) = name
                    .to_str()
                    .and_then(crate::keys::from_hex::<{ chunky::hash::HASH_LEN }>)
                else {
                    continue;
                };
                out.push((ChunkHash(bytes), entry.metadata()?.len()));
            }
        }
        out.sort();
        Ok(out)
    }

    fn path(&self, hash: &ChunkHash) -> PathBuf {
        let name: String = hash.0.iter().map(|b| format!("{b:02x}")).collect();
        self.root.join(&name[..2]).join(name)
    }
}

impl BlobStore for DirStore {
    fn put(&mut self, data: &[u8]) -> io::Result<ChunkHash> {
        let hash = ChunkHash::of(data);
        let path = self.path(&hash);
        if path.exists() {
            return Ok(hash);
        }
        let dir = path.parent().expect("a blob path has a parent");
        fs::create_dir_all(dir)?;
        let tmp = path.with_extension(format!("tmp{}", std::process::id()));
        fs::write(&tmp, data)?;
        fs::rename(&tmp, &path)?;
        Ok(hash)
    }

    fn get(&self, hash: &ChunkHash) -> io::Result<Option<Vec<u8>>> {
        let data = match fs::read(self.path(hash)) {
            Ok(data) => data,
            Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(None),
            Err(e) => return Err(e),
        };
        if ChunkHash::of(&data) != *hash {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "a blob does not match its name",
            ));
        }
        Ok(Some(data))
    }
}
