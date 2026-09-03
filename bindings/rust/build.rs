// Locates libastral and emits the link flags for it.
//
// Search order for the library directory:
//   1. ASTRAL_LIB_DIR
//   2. <ASTRAL_ROOT>/lib
//   3. ~/.astral/lib  (where a plain cmake --install puts it)
//   4. ../../build       (an in-tree cmake build)

use std::env;
use std::path::{Path, PathBuf};

// Every place an install may have put the library, most specific first.
fn with_layout(root: &Path) -> Vec<PathBuf> {
    vec![
        root.join("lib/astral/static-libs"),
        root.join("lib/astral/dynamic-libs"),
        root.join("lib"),
    ]
}

fn candidate_dirs() -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    if let Ok(dir) = env::var("ASTRAL_LIB_DIR") {
        dirs.push(PathBuf::from(dir));
    }
    if let Ok(root) = env::var("ASTRAL_ROOT") {
        dirs.extend(with_layout(Path::new(&root)));
    }
    if let Ok(home) = env::var("HOME") {
        dirs.extend(with_layout(&Path::new(&home).join(".astral")));
    }
    dirs.extend(with_layout(Path::new("/usr/local")));
    dirs.extend(with_layout(Path::new("/usr")));
    if let Ok(manifest) = env::var("CARGO_MANIFEST_DIR") {
        dirs.push(Path::new(&manifest).join("../../build"));
    }
    dirs
}

fn main() {
    println!("cargo:rerun-if-env-changed=ASTRAL_LIB_DIR");
    println!("cargo:rerun-if-env-changed=ASTRAL_ROOT");

    let shared = env::var("CARGO_FEATURE_SHARED").is_ok();
    let names: &[&str] = if shared {
        &["libAstral.dylib", "libAstral.so"]
    } else {
        &["libAstral.a"]
    };

    let mut found = None;
    for dir in candidate_dirs() {
        if names.iter().any(|name| dir.join(name).exists()) {
            found = Some(dir);
            break;
        }
    }

    let dir = match found {
        Some(dir) => dir,
        None => {
            println!(
                "cargo:warning=libAstral not found; set ASTRAL_LIB_DIR \
                 to the directory holding it (build it with astral-update)"
            );
            return;
        }
    };

    println!("cargo:rustc-link-search=native={}", dir.display());
    if shared {
        println!("cargo:rustc-link-lib=dylib=Astral");
    } else {
        println!("cargo:rustc-link-lib=static=Astral");
    }
    println!("cargo:rustc-link-lib=dylib=z");

    // The library is C++, so its runtime has to come along.
    let target = env::var("TARGET").unwrap_or_default();
    if target.contains("apple") {
        println!("cargo:rustc-link-lib=dylib=c++");
    } else if target.contains("linux") {
        println!("cargo:rustc-link-lib=dylib=stdc++");
    }
}
