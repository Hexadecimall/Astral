//! `astral patch` — write edits back into a binary.
//!
//! Astral's other commands read a program; this one changes it. The edits it
//! takes here need no assembler and no compiler: no-op an instruction, or drop
//! exact bytes at an address. Richer edits (assemble a new instruction, compile
//! an edited function) arrive with the assembler and are reached from the TUI.

use std::io::{IsTerminal, Write};

use astral::{Library, Program};

use crate::out::{error, paint, print, tint, Stream};

/// One edit named on the command line, resolved later against the program.
enum Edit {
    Nop { at: String, count: usize },
    Bytes { at: String, hex: String },
    Invert { at: String },
    Return { at: String, value: u64 },
}

struct Args {
    binary: Option<String>,
    language: Option<String>,
    edits: Vec<Edit>,
    output: Option<String>,
    in_place: bool,
    list: bool,
    dry_run: bool,
}

pub fn run(arguments: &[String]) -> i32 {
    if matches!(arguments.first().map(String::as_str), Some("--help" | "-h")) {
        return usage(Stream::Out);
    }
    let args = match parse(arguments) {
        Ok(args) => args,
        Err(code) => return code,
    };
    let Some(binary) = args.binary.as_deref() else {
        error("patch needs a binary");
        return 2;
    };
    if args.edits.is_empty() {
        error("nothing to patch: give at least one --nop or --set");
        return 2;
    }

    let library = match Library::new(None) {
        Ok(library) => library,
        Err(failure) => {
            crate::out::library_error(&failure);
            return 1;
        }
    };
    let mut program = match Program::open(binary, args.language.as_deref()) {
        Ok(program) => program,
        Err(failure) => {
            crate::out::library_error(&failure);
            return 1;
        }
    };

    // Apply every edit, resolving symbol names to addresses as needed.
    for edit in &args.edits {
        let outcome = match edit {
            Edit::Nop { at, count } => resolve(&program, at)
                .and_then(|addr| program.patch_nop(addr, *count).map_err(|e| e.to_string())),
            Edit::Bytes { at, hex } => match (resolve(&program, at), parse_hex(hex)) {
                (Ok(addr), Ok(bytes)) => program
                    .patch_bytes(addr, &bytes, "raw bytes")
                    .map_err(|e| e.to_string()),
                (Err(e), _) | (_, Err(e)) => Err(e),
            },
            Edit::Invert { at } => resolve(&program, at)
                .and_then(|addr| program.patch_invert(addr).map_err(|e| e.to_string())),
            Edit::Return { at, value } => resolve(&program, at)
                .and_then(|addr| program.patch_return(addr, *value).map_err(|e| e.to_string())),
        };
        if let Err(message) = outcome {
            error(&message);
            return 1;
        }
    }

    let count = program.patch_count();
    print(&format!(
        "{} {} queued.\n",
        count,
        if count == 1 { "patch" } else { "patches" }
    ));
    if args.list {
        print(&program.patch_serialize());
    }
    if args.dry_run {
        print(&tint(paint::DIM, "dry run: nothing written.\n"));
        return 0;
    }

    let _ = library; // held for the program's lifetime
    write_out(&program, binary, &args)
}

/// Decides where the patched bytes go and writes them.
fn write_out(program: &Program, binary: &str, args: &Args) -> i32 {
    // An explicit destination skips the question.
    if let Some(path) = &args.output {
        return finish(program, path, false);
    }
    if args.in_place {
        return finish(program, binary, true);
    }

    let default_new = format!("{binary}.patched");
    // Not a terminal: the safe default, never touch the original unasked.
    if !std::io::stdin().is_terminal() || !std::io::stdout().is_terminal() {
        return finish(program, &default_new, false);
    }
    match ask(binary, &default_new) {
        Choice::New => finish(program, &default_new, false),
        Choice::InPlace => finish(program, binary, true),
        Choice::Cancel => {
            print(&tint(paint::DIM, "cancelled.\n"));
            0
        }
    }
}

fn finish(program: &Program, path: &str, in_place: bool) -> i32 {
    if in_place {
        // Keep a copy before overwriting the original the first time.
        let backup = format!("{path}.bak");
        if !std::path::Path::new(&backup).exists() {
            let _ = std::fs::copy(path, &backup);
        }
    }
    match program.write_patched(path) {
        Ok(()) => {
            print(&format!("{} {}\n", tint(paint::GREEN, "wrote"), path));
            0
        }
        Err(failure) => {
            error(&failure.to_string());
            1
        }
    }
}

enum Choice {
    New,
    InPlace,
    Cancel,
}

/// The three-way apply prompt. A `.` remembers the answer for next time.
fn ask(binary: &str, default_new: &str) -> Choice {
    let remembered = read_remembered();
    if let Some(choice) = remembered {
        return choice;
    }
    print(&format!("Apply patch to {}?\n", tint(paint::BOLD, binary)));
    print(&format!("  [n] make a new file ({default_new})\n"));
    print("  [p] patch the file in place\n");
    print("  [.] patch in place and don't ask again\n");
    let mut out = std::io::stdout();
    let _ = out.write_all(b"> ");
    let _ = out.flush();
    let mut line = String::new();
    if std::io::stdin().read_line(&mut line).is_err() {
        return Choice::New;
    }
    match line.trim() {
        "p" | "P" => Choice::InPlace,
        "." => {
            remember();
            Choice::InPlace
        }
        "" | "n" | "N" => Choice::New,
        _ => Choice::Cancel,
    }
}

fn remember_path() -> Option<std::path::PathBuf> {
    let home = std::env::var_os("HOME")?;
    Some(std::path::Path::new(&home).join(".astral").join("patch-apply"))
}

fn remember() {
    if let Some(path) = remember_path() {
        if let Some(parent) = path.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        let _ = std::fs::write(path, "in-place\n");
    }
}

fn read_remembered() -> Option<Choice> {
    let text = std::fs::read_to_string(remember_path()?).ok()?;
    match text.trim() {
        "in-place" => Some(Choice::InPlace),
        "new" => Some(Choice::New),
        _ => None,
    }
}

/// A hex or symbol address, plus an optional `:count`.
fn resolve(program: &Program, spec: &str) -> Result<u64, String> {
    let text = spec.trim();
    if let Some(hex) = text.strip_prefix("0x").or_else(|| text.strip_prefix("0X")) {
        return u64::from_str_radix(hex, 16).map_err(|_| format!("bad address {spec}"));
    }
    // A bare number is decimal; anything else is a symbol name.
    if let Ok(value) = text.parse::<u64>() {
        return Ok(value);
    }
    program
        .symbols()
        .into_iter()
        .find(|s| s.name == text)
        .map(|s| s.address)
        .ok_or_else(|| format!("no symbol named {spec}"))
}

fn parse_hex(text: &str) -> Result<Vec<u8>, String> {
    let cleaned: String = text.chars().filter(|c| !c.is_whitespace()).collect();
    if cleaned.len() % 2 != 0 {
        return Err(format!("odd number of hex digits in {text}"));
    }
    (0..cleaned.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&cleaned[i..i + 2], 16).map_err(|_| format!("bad hex {text}")))
        .collect()
}

/// Splits `<addr>` or `<addr>:<count>`.
fn split_count(spec: &str) -> (String, usize) {
    match spec.rsplit_once(':') {
        Some((addr, count)) => (addr.to_string(), count.parse().unwrap_or(1)),
        None => (spec.to_string(), 1),
    }
}

fn parse(arguments: &[String]) -> Result<Args, i32> {
    let mut args = Args {
        binary: None,
        language: None,
        edits: Vec::new(),
        output: None,
        in_place: false,
        list: false,
        dry_run: false,
    };
    let mut it = arguments.iter();
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--nop" => {
                let Some(spec) = it.next() else {
                    error("--nop needs an address");
                    return Err(2);
                };
                let (at, count) = split_count(spec);
                args.edits.push(Edit::Nop { at, count });
            }
            "--set" => {
                let Some(spec) = it.next() else {
                    error("--set needs <address>=<hex>");
                    return Err(2);
                };
                let Some((at, hex)) = spec.split_once('=') else {
                    error("--set wants <address>=<hex>, e.g. --set 0x1000=1f2003d5");
                    return Err(2);
                };
                args.edits.push(Edit::Bytes {
                    at: at.to_string(),
                    hex: hex.to_string(),
                });
            }
            "--invert" => {
                let Some(spec) = it.next() else {
                    error("--invert needs an address");
                    return Err(2);
                };
                args.edits.push(Edit::Invert { at: spec.clone() });
            }
            "--ret" => {
                let Some(spec) = it.next() else {
                    error("--ret needs an address, optionally =<value>");
                    return Err(2);
                };
                let (at, value) = match spec.split_once('=') {
                    Some((a, v)) => {
                        let parsed = if let Some(h) = v.strip_prefix("0x").or_else(|| v.strip_prefix("0X")) {
                            u64::from_str_radix(h, 16)
                        } else {
                            v.parse::<u64>()
                        };
                        match parsed {
                            Ok(value) => (a.to_string(), value),
                            Err(_) => {
                                error(&format!("bad return value {v}"));
                                return Err(2);
                            }
                        }
                    }
                    None => (spec.clone(), 0),
                };
                args.edits.push(Edit::Return { at, value });
            }
            "--asm" => {
                error("assembling instructions is not built yet; use --set <addr>=<hex> for now");
                return Err(2);
            }
            "-o" | "--output" => {
                let Some(path) = it.next() else {
                    error("-o needs a path");
                    return Err(2);
                };
                args.output = Some(path.clone());
            }
            "--in-place" => args.in_place = true,
            "--list" => args.list = true,
            "--dry-run" => args.dry_run = true,
            "-l" | "--language" => {
                let Some(id) = it.next() else {
                    error("--language needs an id");
                    return Err(2);
                };
                args.language = Some(id.clone());
            }
            other if other.starts_with('-') => {
                error(&format!("unknown patch option {other}"));
                return Err(2);
            }
            other => {
                if args.binary.is_none() {
                    args.binary = Some(other.to_string());
                } else {
                    error(&format!("unexpected argument {other}"));
                    return Err(2);
                }
            }
        }
    }
    Ok(args)
}

pub fn usage(stream: Stream) -> i32 {
    let mut w = crate::out::Sink::new(stream);
    w.write(&format!("{}\n\n", tint(paint::BOLD, "astral patch — write edits into a binary")));
    w.write("usage: astral patch <binary> [edits] [output]\n\n");
    w.write("edits\n");
    w.write("  --nop <addr>[:<n>]      replace n instructions (default 1) with no-ops\n");
    w.write("  --set <addr>=<hex>      write exact bytes, e.g. --set 0x1000=1f2003d5\n");
    w.write("  --invert <addr>         flip a conditional branch (b.cond, cbz, jcc)\n");
    w.write("  --ret <addr>[=<value>]  make the function at addr return value (default 0)\n\n");
    w.write("output\n");
    w.write("  -o, --output <path>     write the patched binary here\n");
    w.write("  --in-place              overwrite the original (keeps a .bak)\n");
    w.write("  --list                  print the patch set before writing\n");
    w.write("  --dry-run               show what would change, write nothing\n\n");
    w.write("<addr> is 0x-hex, decimal, or a symbol name. With neither -o nor\n");
    w.write("--in-place, Astral asks where to write.\n");
    stream.code()
}
