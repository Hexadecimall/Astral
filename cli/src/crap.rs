//! `astral crap-ya-dont-need` — the commands nobody asked for.
//!
//! Every other command in Astral exists because someone needed it. These do
//! not. `binary` decompiles a program to the only representation guaranteed to
//! be faithful, one byte per line, and `binary-compile` turns that back into
//! the program it came from. The round trip is exact, which is the only
//! defensible thing about any of it.

use std::io::{Read, Write};

use crate::out::{error, print, Stream};

pub fn usage(stream: Stream) -> i32 {
    let text = concat!(
        "usage: astral crap-ya-dont-need <thing> [options] <file>\n",
        "\n",
        "Things you do not need:\n",
        "\n",
        "  binary <file>          decompile it to binary. All of it. One byte\n",
        "                         per line, which for anything real is a lot of\n",
        "                         lines. This is the highest-fidelity decompiler\n",
        "                         in existence and the least useful.\n",
        "  binary-compile <file>  compile that back into the program it was.\n",
        "                         Byte for byte, so it still runs.\n",
        "\n",
        "  -o, --output <file>    write there rather than to standard output\n",
        "\n",
        "  astral crap-ya-dont-need binary /bin/echo -o echo.binary\n",
        "  astral crap-ya-dont-need binary-compile echo.binary -o echo\n",
    );
    print(text);
    stream.code()
}

pub fn run(arguments: &[String]) -> i32 {
    let thing = match arguments.first() {
        Some(thing) => thing.as_str(),
        None => return usage(Stream::Err),
    };
    let rest = &arguments[1..];

    match thing {
        "--help" | "-h" => usage(Stream::Out),
        "binary" => match parse(rest) {
            Ok((input, output)) => to_binary(&input, output.as_deref()),
            Err(code) => code,
        },
        "binary-compile" => match parse(rest) {
            Ok((input, output)) => from_binary(&input, output.as_deref()),
            Err(code) => code,
        },
        other => {
            error(&format!("no such thing '{other}'; there are only two"));
            usage(Stream::Err)
        }
    }
}

/// The file to work on, and where the answer goes.
fn parse(arguments: &[String]) -> Result<(String, Option<String>), i32> {
    let mut input = None;
    let mut output = None;
    let mut index = 0;
    while index < arguments.len() {
        match arguments[index].as_str() {
            "--help" | "-h" => return Err(usage(Stream::Out)),
            "-o" | "--output" => {
                index += 1;
                match arguments.get(index) {
                    Some(path) => output = Some(path.clone()),
                    None => {
                        error("--output needs a file");
                        return Err(2);
                    }
                }
            }
            other if other.starts_with('-') => {
                error(&format!("unknown option {other}"));
                return Err(usage(Stream::Err));
            }
            other => input = Some(other.to_string()),
        }
        index += 1;
    }
    match input {
        Some(path) => Ok((path, output)),
        None => {
            error("name a file");
            Err(usage(Stream::Err))
        }
    }
}

fn to_binary(path: &str, output: Option<&str>) -> i32 {
    let bytes = match std::fs::read(path) {
        Ok(bytes) => bytes,
        Err(problem) => {
            error(&format!("cannot read {path}: {problem}"));
            return 1;
        }
    };

    // Eight characters and a newline for every byte in the file, built once
    // rather than a line at a time: a real program is megabytes of this.
    let mut text = String::with_capacity(bytes.len() * 9 + 256);
    text.push_str("# Decompiled by Astral\n");
    for byte in &bytes {
        for bit in (0..8).rev() {
            text.push(if byte >> bit & 1 == 1 { '1' } else { '0' });
        }
        text.push('\n');
    }

    match output {
        None => print(&text),
        Some(path) => {
            if let Err(problem) = std::fs::write(path, text.as_bytes()) {
                error(&format!("cannot write {path}: {problem}"));
                return 1;
            }
            print(&format!("{} lines written to {path}\n", bytes.len()));
        }
    }
    0
}

fn from_binary(path: &str, output: Option<&str>) -> i32 {
    let mut text = String::new();
    if path == "-" {
        if let Err(problem) = std::io::stdin().read_to_string(&mut text) {
            error(&format!("cannot read standard input: {problem}"));
            return 1;
        }
    } else {
        match std::fs::read_to_string(path) {
            Ok(read) => text = read,
            Err(problem) => {
                error(&format!("cannot read {path}: {problem}"));
                return 1;
            }
        }
    }

    let mut bytes = Vec::new();
    for (number, line) in text.lines().enumerate() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if line.len() != 8 || line.chars().any(|c| c != '0' && c != '1') {
            error(&format!("line {} is not a byte: {line}", number + 1));
            return 1;
        }
        bytes.push(u8::from_str_radix(line, 2).unwrap_or(0));
    }
    if bytes.is_empty() {
        error("there is nothing here to compile");
        return 1;
    }

    let Some(path) = output else {
        error("say where it goes with --output; this is not something to print");
        return 1;
    };
    let mut file = match std::fs::File::create(path) {
        Ok(file) => file,
        Err(problem) => {
            error(&format!("cannot write {path}: {problem}"));
            return 1;
        }
    };
    if let Err(problem) = file.write_all(&bytes) {
        error(&format!("cannot write {path}: {problem}"));
        return 1;
    }
    drop(file);

    // It was a program before it was a million lines, so let it be one again.
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o755));
    }

    print(&format!("{} bytes written to {path}\n", bytes.len()));
    0
}
