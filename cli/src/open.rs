//! `astral open` — the window, from the command line.
//!
//! The same word on every platform, because "which of these three ways does
//! this machine want" is not a question anyone should have to hold. What
//! differs underneath is only how a windowed program is started: macOS has an
//! application bundle and a launcher that knows about it, Linux and Windows
//! have a binary to run.
//!
//! Where the window is found follows the same rule as everything else here:
//! from where this copy of `astral` is, never from a path written in at build
//! time. A tree that was moved still works, and a copy never names the machine
//! that built it.

use crate::out::{error, print, Sink, Stream};
use crate::paths;
use std::path::{Path, PathBuf};
use std::process::Command;

/// The GUI belonging to this install, if it can be found.
///
/// Beside the command first, since that is where an install puts it, then the
/// application bundle on macOS, then the build tree, so this works in a
/// checkout without installing anything.
fn find_window() -> Option<PathBuf> {
    let mut looked = Vec::new();

    if let Ok(executable) = std::env::current_exe() {
        if let Some(directory) = executable.parent() {
            for name in ["astral-gui", "astral-gui.exe"] {
                looked.push(directory.join(name));
            }
            // A build tree keeps the window under gui/ rather than beside
            // the command, so a checkout works without installing anything.
            looked.push(directory.join("gui/astral-gui"));
            looked.push(directory.join("gui/astral-gui.exe"));
        }
    }
    if let Some(prefix) = paths::install_prefix() {
        looked.push(prefix.join("bin/astral-gui"));
        looked.push(prefix.join("bin/astral-gui.exe"));
    }
    looked.into_iter().find(|candidate| candidate.is_file())
}

/// The application bundle, which is what a Mac should open: it carries the Qt
/// frameworks, the icon and the name, and `open` gives it a dock entry rather
/// than a process owned by this terminal.
#[cfg(target_os = "macos")]
fn find_bundle() -> Option<PathBuf> {
    let mut looked = vec![
        PathBuf::from("/Applications/Astral.app"),
    ];
    if let Ok(home) = std::env::var("HOME") {
        looked.push(PathBuf::from(home).join("Applications/Astral.app"));
    }
    if let Some(prefix) = paths::install_prefix() {
        looked.push(prefix.join("share/astral/applications/Astral.app"));
    }
    if let Ok(executable) = std::env::current_exe() {
        if let Some(build) = executable.parent() {
            looked.push(build.join("gui/Astral.app"));
        }
    }
    looked.into_iter().find(|candidate| candidate.is_dir())
}

const HELP: &str = concat!(
            "astral open [binary] [--wait]\n",
            "\n",
            "Opens the window, on whatever this machine is. A binary named here\n",
            "is opened in it, the same as passing it to the application.\n",
            "\n",
            "      --wait             stay until the window closes\n",
            "  -h, --help             this\n",
);

pub fn usage(stream: Stream) -> i32 {
    Sink::new(stream).write(HELP);
    stream.code()
}

pub fn run(arguments: &[String]) -> i32 {
    let mut subject: Option<String> = None;
    let mut wait = false;
    for argument in arguments {
        match argument.as_str() {
            "-h" | "--help" => return usage(Stream::Out),
            "--wait" => wait = true,
            other if other.starts_with('-') => {
                error(&format!("astral open: no such option {other}\n"));
                return usage(Stream::Err);
            }
            other => {
                if subject.is_some() {
                    error("astral open: one binary at a time\n");
                    return 2;
                }
                subject = Some(other.to_string());
            }
        }
    }

    if let Some(path) = &subject {
        if !Path::new(path).exists() {
            error(&format!("astral open: there is no {path}\n"));
            return 1;
        }
    }

    // macOS opens the bundle through the launcher, which is what puts it in
    // the dock and gives it its name. Falling through to the bare binary would
    // work and would look like something else.
    // macOS opens the bundle through the launcher, which is what gives it a
    // dock entry and its name. Falling through to the bare binary would work
    // and would look like something else.
    #[cfg(target_os = "macos")]
    if let Some(bundle) = find_bundle() {
        let mut command = Command::new("/usr/bin/open");
        if wait {
            command.arg("-W");
        }
        command.arg("-a").arg(&bundle);
        if let Some(path) = &subject {
            command.arg("--args").arg(path);
        }
        return match command.status() {
            Ok(status) if status.success() => 0,
            Ok(status) => {
                error(&format!(
                    "astral open: the window would not start ({status})\n"
                ));
                1
            }
            Err(problem) => {
                error(&format!("astral open: {problem}\n"));
                1
            }
        };
    }

    let Some(window) = find_window() else {
        error(concat!(
            "astral open: no window is installed beside this command.\n",
            "Install Astral with the GUI, or build it: cmake --build build\n"
        ));
        return 1;
    };

    let mut command = Command::new(&window);
    if let Some(path) = &subject {
        command.arg(path);
    }
    if wait {
        return match command.status() {
            Ok(status) => status.code().unwrap_or(0),
            Err(problem) => {
                error(&format!("astral open: {problem}\n"));
                1
            }
        };
    }
    // Otherwise it outlives the shell that asked for it, the way a windowed
    // program should.
    match command.spawn() {
        Ok(_) => {
            print(&format!("opened {}\n", window.display()));
            0
        }
        Err(problem) => {
            error(&format!("astral open: {problem}\n"));
            1
        }
    }
}
