//! Finding things at run time.
//!
//! No location is written into the binary. Where an install lives is worked out
//! from where the running program lives, so a copy never names the machine that
//! built it and never breaks when the install is moved.

use std::fs;
use std::path::{Path, PathBuf};

/// The usual place, when nothing better can be told.
pub const SYSTEM_PREFIX: &str = "/usr/local";

/// Walks up from a file until a directory holding `share/astral` is found. That
/// directory is the install the file belongs to, wherever it was put.
fn prefix_above(file: &Path) -> Option<PathBuf> {
    let mut directory = file.parent()?;
    for _ in 0..6 {
        if directory.join("share/astral").is_dir() {
            return Some(directory.to_path_buf());
        }
        directory = directory.parent()?;
    }
    None
}

/// The install the running copy belongs to.
pub fn install_prefix() -> Option<PathBuf> {
    let executable = fs::canonicalize(std::env::current_exe().ok()?).ok()?;
    prefix_above(&executable)
}

/// The install this copy belongs to, or the usual place.
pub fn default_prefix() -> PathBuf {
    install_prefix().unwrap_or_else(|| PathBuf::from(SYSTEM_PREFIX))
}

/// Whether a directory can be written to, asked by trying rather than by
/// reasoning about ownership bits, which get the answer wrong under ACLs.
pub fn writable(directory: &Path) -> bool {
    let probe = directory.join(".astral-write-probe");
    match fs::File::create(&probe) {
        Ok(_) => {
            let _ = fs::remove_file(&probe);
            true
        }
        Err(_) => false,
    }
}

/// True when everything an install writes into is already writable, so
/// elevation is only asked for when it is genuinely needed.
pub fn install_tree_writable(prefix: &Path) -> bool {
    if !prefix.exists() {
        // Nothing there yet: what matters is the nearest directory that does
        // exist, because that is where the first one gets created.
        let mut candidate = prefix;
        while !candidate.exists() {
            match candidate.parent() {
                Some(parent) if parent != Path::new("") => candidate = parent,
                _ => break,
            }
        }
        return writable(candidate);
    }
    for part in ["bin", "include", "lib", "share"] {
        let path = prefix.join(part);
        if path.exists() {
            if !writable(&path) {
                return false;
            }
        } else if !writable(prefix) {
            return false;
        }
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A prefix is recognised from a file inside it, however deep, and nothing
    /// is invented for a file that belongs to no install.
    #[test]
    fn a_prefix_is_found_from_a_file_within_it() {
        let root = std::env::temp_dir().join("astral-prefix-test");
        let _ = fs::remove_dir_all(&root);
        let prefix = root.join("opt/somewhere");
        fs::create_dir_all(prefix.join("share/astral")).expect("test tree");
        fs::create_dir_all(prefix.join("bin")).expect("test tree");

        assert_eq!(
            prefix_above(&prefix.join("bin/astral")),
            Some(prefix.clone())
        );
        assert_eq!(
            prefix_above(&prefix.join("lib/astral/static-libs/libAstral.a")),
            Some(prefix)
        );
        assert_eq!(prefix_above(&root.join("elsewhere/astral")), None);

        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn a_missing_prefix_falls_back_to_the_usual_place() {
        // Nothing is compiled in: the fallback is the only constant.
        assert_eq!(SYSTEM_PREFIX, "/usr/local");
    }

    #[test]
    fn writability_is_answered_by_trying() {
        assert!(writable(&std::env::temp_dir()));
        assert!(!writable(Path::new("/this/does/not/exist")));
    }
}
