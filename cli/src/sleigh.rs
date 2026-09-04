//! Compiling a processor specification.
//!
//! The compiler is part of the library, so this is a call rather than another
//! program to find on the path.

use std::path::Path;

use crate::out::{library_error, Sink, Stream};

const HELP: &str = concat!(
    "usage: astral sleigh <input.slaspec> <output.sla>\n",
    "\n",
    "Compiles a SLEIGH processor specification into the .sla the decompiler\n",
    "loads at run time.\n",
);

pub fn usage(stream: Stream) -> i32 {
    Sink::new(stream).write(HELP);
    stream.code()
}

pub fn run(arguments: &[String]) -> i32 {
    if arguments.iter().any(|a| a == "--help" || a == "-h") {
        return usage(Stream::Out);
    }
    if arguments.len() != 2 {
        return usage(Stream::Err);
    }
    match astral::compile_sleigh(Path::new(&arguments[0]), Path::new(&arguments[1])) {
        Ok(()) => 0,
        Err(failure) => {
            library_error(&failure);
            1
        }
    }
}
