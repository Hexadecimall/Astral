//! Writing text out.
//!
//! Write failures are swallowed rather than raised: a decompiled program piped
//! into something that stops reading is an ordinary way for this command to
//! end, and it should not look like a crash.

use std::fs::File;
use std::io::{self, Write};
use std::path::Path;

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
    Sink::new(Stream::Err).write(&format!("astral: {message}\n"));
}

/// Prints what the library said went wrong.
pub fn library_error(error: &astral::Error) {
    Sink::new(Stream::Err).write(&format!("astral: {}\n", error.message));
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
