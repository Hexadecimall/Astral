//! Writing text out.
//!
//! Write failures are swallowed rather than raised: a decompiled program piped
//! into something that stops reading is an ordinary way for this command to
//! end, and it should not look like a crash.

use std::fs::File;
use std::io::{self, IsTerminal, Write};
use std::path::Path;

/// Whether to colour a stream.
///
/// Colour is decoration, never information: everything it says is also said by
/// the words. So it is dropped whenever it might not land — piped output, a
/// terminal that says it is dumb, or NO_COLOR set to anything at all.
pub fn colour_wanted(stream: Stream) -> bool {
    if std::env::var_os("NO_COLOR").is_some_and(|v| !v.is_empty()) {
        return false;
    }
    if std::env::var("TERM").map(|t| t == "dumb").unwrap_or(false) {
        return false;
    }
    match stream {
        Stream::Out => io::stdout().is_terminal(),
        Stream::Err => io::stderr().is_terminal(),
    }
}

/// The terminal's own palette rather than fixed colours, so output sits in
/// whatever theme the reader chose.
pub mod paint {
    pub const RESET: &str = "\x1b[0m";
    pub const BOLD_RED: &str = "\x1b[1;31m";
    pub const BOLD: &str = "\x1b[1m";
    pub const DIM: &str = "\x1b[2m";
    pub const CYAN: &str = "\x1b[36m";
    pub const YELLOW: &str = "\x1b[33m";
    pub const GREEN: &str = "\x1b[32m";
}

/// Wraps text in a colour, or leaves it alone when colour is not wanted.
pub fn colour(stream: Stream, code: &str, text: &str) -> String {
    if colour_wanted(stream) {
        format!("{code}{text}{}", paint::RESET)
    } else {
        text.to_string()
    }
}

/// Colours for standard output.
pub fn tint(code: &str, text: &str) -> String {
    colour(Stream::Out, code, text)
}

/// Which of the two standard streams a message belongs on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Stream {
    Out,
    Err,
}

impl Stream {
    /// The exit code that goes with printing there: help someone asked for
    /// succeeds, help printed because the command was wrong does not.
    pub fn code(self) -> i32 {
        match self {
            Stream::Out => 0,
            Stream::Err => 2,
        }
    }
}

pub struct Sink {
    stream: Stream,
}

impl Sink {
    pub fn new(stream: Stream) -> Self {
        Sink { stream }
    }

    pub fn write(&mut self, text: &str) {
        let _ = match self.stream {
            Stream::Out => io::stdout().write_all(text.as_bytes()),
            Stream::Err => io::stderr().write_all(text.as_bytes()),
        };
        let _ = match self.stream {
            Stream::Out => io::stdout().flush(),
            Stream::Err => io::stderr().flush(),
        };
    }
}

/// Standard output, or a file when `--output` named one.
pub enum Destination {
    Stdout,
    File(File),
}

impl Destination {
    pub fn open(path: Option<&str>) -> io::Result<Self> {
        match path {
            None => Ok(Destination::Stdout),
            Some(path) => Ok(Destination::File(File::create(Path::new(path))?)),
        }
    }

    pub fn write(&mut self, text: &str) {
        let _ = match self {
            Destination::Stdout => io::stdout().write_all(text.as_bytes()),
            Destination::File(file) => file.write_all(text.as_bytes()),
        };
    }
}

/// A message on the command's own behalf, in the shape every other one takes.
pub fn error(message: &str) {
    let label = colour(Stream::Err, paint::BOLD_RED, "astral:");
    Sink::new(Stream::Err).write(&format!("{label} {message}\n"));
}

/// Prints what the library said went wrong.
pub fn library_error(error: &astral::Error) {
    let label = colour(Stream::Err, paint::BOLD_RED, "astral:");
    Sink::new(Stream::Err).write(&format!("{label} {}\n", error.message));
}

/// A note that something is being done, or has been. Never an error.
pub fn note(message: &str) {
    Sink::new(Stream::Err).write(&format!("{}\n", colour(Stream::Err, paint::DIM, message)));
}

/// Writes to standard output, ignoring a reader that has gone away.
///
/// Flushed every time, because progress lines that end mid-sentence have to be
/// on screen before the slow work they announce begins.
pub fn print(text: &str) {
    let mut out = io::stdout();
    let _ = out.write_all(text.as_bytes());
    let _ = out.flush();
}

/// Whether standard output should carry colour under a `--color` mode.
/// `always` overrides a pipe and NO_COLOR alike, because it was asked for by
/// name; `auto` is the usual terminal test.
pub fn stdout_colour(mode: crate::options::ColorMode) -> bool {
    use crate::options::ColorMode;
    match mode {
        ColorMode::Always => true,
        ColorMode::Never => false,
        ColorMode::Auto => colour_wanted(Stream::Out),
    }
}
