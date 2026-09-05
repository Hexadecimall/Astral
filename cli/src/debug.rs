//! An interactive debugger over the emulator.
//!
//! The program never reaches the operating system: its memory and registers are
//! Astral's and its library calls are answered by Astral, so a binary for
//! another processor is debugged the same as a native one and nothing it does
//! can escape the machine watching it.
//!
//! A session is a loop of one-line commands. Anything given with `--command` or
//! in a `--script` is run first, in order, which is how a session is driven
//! without a terminal.

use std::io::{BufRead, IsTerminal, Write};

use astral::{Debugger, Library, Program, State};

use crate::help;
use crate::options::parse_number;
use crate::out::{error, print, Stream};

/// Everything a session was told before it started.
struct Settings {
    path: String,
    language: Option<String>,
    specs: Option<String>,
    entry_name: Option<String>,
    entry: u64,
    arguments: Vec<String>,
    input: String,
    step_limit: u64,
    /// Commands to run before standard input is read, in the order given.
    queued: Vec<String>,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            path: String::new(),
            language: None,
            specs: None,
            entry_name: None,
            entry: 0,
            arguments: Vec::new(),
            input: String::new(),
            step_limit: 0,
            queued: Vec::new(),
        }
    }
}

/// `--input @file` reads the file; anything else is the text itself.
fn text_or_file(value: &str) -> Result<String, i32> {
    match value.strip_prefix('@') {
        Some(path) => std::fs::read_to_string(path).map_err(|_| {
            error(&format!("cannot read {path}"));
            2
        }),
        None => Ok(value.to_string()),
    }
}

fn parse(arguments: &[String]) -> Result<Settings, i32> {
    let mut settings = Settings::default();
    let mut index = 0;
    while index < arguments.len() {
        let argument = arguments[index].as_str();
        let value = |name: &str, index: &mut usize| -> Result<String, i32> {
            if *index + 1 >= arguments.len() {
                error(&format!("{name} needs a value"));
                return Err(2);
            }
            *index += 1;
            Ok(arguments[*index].clone())
        };

        match argument {
            "--help" | "-h" => return Err(help::debug(Stream::Out)),
            "-f" | "--function" => settings.entry_name = Some(value("--function", &mut index)?),
            "-a" | "--address" => {
                let text = value("--address", &mut index)?;
                match parse_number(&text) {
                    Some(address) => settings.entry = address,
                    None => {
                        error(&format!("--address wants a number, not {text}"));
                        return Err(2);
                    }
                }
            }
            "--arg" => settings.arguments.push(value("--arg", &mut index)?),
            "--input" => {
                let text = value("--input", &mut index)?;
                settings.input = text_or_file(&text)?;
            }
            "--steps" => {
                let text = value("--steps", &mut index)?;
                match text.parse::<u64>() {
                    Ok(count) => settings.step_limit = count,
                    Err(_) => {
                        error(&format!("--steps wants a count, not {text}"));
                        return Err(2);
                    }
                }
            }
            "--command" => settings.queued.push(value("--command", &mut index)?),
            "--script" => {
                let path = value("--script", &mut index)?;
                let text = std::fs::read_to_string(&path).map_err(|_| {
                    error(&format!("cannot read {path}"));
                    2
                })?;
                settings
                    .queued
                    .extend(text.lines().map(str::to_string));
            }
            "-l" | "--language" => settings.language = Some(value("--language", &mut index)?),
            "-s" | "--specs" => settings.specs = Some(value("--specs", &mut index)?),
            _ if argument.starts_with('-') => {
                error(&format!("unknown option {argument}"));
                return Err(help::debug(Stream::Err));
            }
            _ => settings.path = argument.to_string(),
        }
        index += 1;
    }
    if settings.path.is_empty() {
        error("debug needs a binary");
        return Err(2);
    }
    Ok(settings)
}

/// A program, the symbols read out of it once, and the debugger over it.
struct Session {
    // The debugger reads the program, so it is declared first and therefore
    // released first.
    debugger: Debugger,
    program: Program,
    /// Function symbols by address, sorted, so the name containing an address
    /// is one search.
    functions: Vec<(u64, u64, String)>,
    names: Vec<(String, u64)>,
    /// Whether every instruction is being recorded. Off until `trace` asks
    /// for it: a line per instruction is millions of them on a real program.
    tracing: bool,
    /// Set when `quit` was asked for, or when a script ran out with no
    /// terminal to fall back to.
    done: bool,
}

impl Session {
    /// The name of whatever contains an address, and where that starts.
    fn containing(&self, address: u64) -> Option<(u64, &str)> {
        let mut found: Option<(u64, u64, &str)> = None;
        for (at, size, name) in &self.functions {
            if *at <= address {
                found = Some((*at, *size, name.as_str()));
            } else {
                break;
            }
        }
        // A symbol that says how long it is settles whether the address is
        // really inside it; without one the nearest is the best there is.
        match found {
            Some((at, size, name)) if size == 0 || address < at + size => Some((at, name)),
            _ => None,
        }
    }

    /// An address written as a number, `$register` for what one holds, or the
    /// name of something in the program.
    fn resolve(&self, text: &str) -> Option<u64> {
        if let Some(name) = text.strip_prefix('$') {
            return self.debugger.register(name).ok();
        }
        if let Some(address) = parse_number(text) {
            return Some(address);
        }
        self.names
            .iter()
            .find(|(name, _)| name == text)
            .map(|(_, address)| *address)
    }

    /// What every stop prints: why it stopped, anything the program wrote, and
    /// the instruction it is about to run.
    fn report(&self, state: &State) {
        if !state.output.is_empty() {
            print(&state.output);
            if !state.output.ends_with('\n') {
                print("\n");
            }
        }
        if !state.calls.is_empty() {
            print(&format!("-- called: {}\n", state.calls.join(" ")));
        }
        print(&format!("-- {}\n", state.reason));
        if !state.live {
            return;
        }
        let name = if state.function.is_empty() {
            String::new()
        } else {
            format!(" <{}>", state.function)
        };
        match self.program.disassemble_readable(state.address, 1) {
            Ok(text) => print(&format!("{}{}\n", text.trim_end(), name)),
            Err(_) => print(&format!("0x{:012x}{}\n", state.address, name)),
        }
    }
}

// ------------------------------------------------------------------ commands

fn show_breakpoints(session: &Session) {
    let all = session.debugger.breakpoints();
    if all.is_empty() {
        print("no breakpoints\n");
        return;
    }
    for address in all {
        let name = session
            .containing(address)
            .map(|(_, name)| format!(" <{name}>"))
            .unwrap_or_default();
        print(&format!("0x{address:012x}{name}\n"));
    }
}

fn show_registers(session: &Session, everything: bool) {
    // An architecture names hundreds of registers, most of which a user-mode
    // program never touches. What it is holding something in is the useful
    // list; `registers all` is there for when it is not.
    let all = session.debugger.registers();
    let wanted: Vec<(String, u64)> = if everything {
        all
    } else {
        all.into_iter()
            .filter(|(name, value)| {
                *value != 0 || matches!(name.as_str(), "sp" | "pc" | "RSP" | "RIP")
            })
            .collect()
    };
    if wanted.is_empty() {
        print("every register is zero\n");
        return;
    }
    let mut column = 0;
    for (name, value) in wanted {
        print(&format!("{name:<8} 0x{value:016x}  "));
        column += 1;
        if column % 3 == 0 {
            print("\n");
        }
    }
    if column % 3 != 0 {
        print("\n");
    }
}

fn show_memory(session: &Session, address: u64, count: usize) {
    let bytes = session.debugger.read(address, count);
    if bytes.is_empty() {
        print("nothing is mapped there\n");
        return;
    }
    for (row, chunk) in bytes.chunks(16).enumerate() {
        let hex: Vec<String> = chunk.iter().map(|byte| format!("{byte:02x}")).collect();
        let text: String = chunk
            .iter()
            .map(|&byte| {
                if (0x20..0x7f).contains(&byte) {
                    byte as char
                } else {
                    '.'
                }
            })
            .collect();
        print(&format!(
            "0x{:012x}  {:<47}  {}\n",
            address + (row * 16) as u64,
            hex.join(" "),
            text
        ));
    }
}

fn show_stack(session: &Session) {
    let frames = session.debugger.stack();
    if frames.is_empty() {
        print("no frames\n");
        return;
    }
    for (depth, frame) in frames.iter().enumerate() {
        let name = if frame.function.is_empty() {
            "?".to_string()
        } else {
            frame.function.clone()
        };
        print(&format!(
            "#{depth} 0x{:012x} in {name} (frame 0x{:x})\n",
            frame.address, frame.frame_pointer
        ));
    }
}

/// The decompiled C of whatever the program is in, with the current line marked
/// when the listing says which address it came from.
fn show_source(session: &Session, address: u64) {
    let Some((start, name)) = session.containing(address) else {
        print("there is no function here to decompile\n");
        return;
    };
    match session.program.decompile(start, None) {
        Ok(function) => {
            let marker = format!("{address:x}");
            for line in function.c_code().lines() {
                let here = line.contains(&marker);
                print(&format!("{} {line}\n", if here { "->" } else { "  " }));
            }
        }
        Err(failure) => error(&format!("{name}: {}", failure.message)),
    }
}

/// Runs one line. Returns false when the session should end.
fn run_command(session: &mut Session, line: &str) -> bool {
    let line = line.trim();
    if line.is_empty() || line.starts_with('#') {
        return true;
    }
    let mut words = line.split_whitespace();
    let word = words.next().unwrap_or_default();
    let rest: Vec<&str> = words.collect();
    let address = session.debugger.state().address;

    match word {
        "quit" | "q" => return false,
        "help" | "h" | "?" => {
            help::debug(Stream::Out);
        }
        "break" | "b" => match rest.first().and_then(|text| session.resolve(text)) {
            Some(at) => {
                if session.debugger.add_breakpoint(at).is_ok() {
                    print(&format!("breakpoint at 0x{at:012x}\n"));
                }
            }
            None => error("break wants an address or a name"),
        },
        "delete" | "d" => match rest.first().and_then(|text| session.resolve(text)) {
            Some(at) => {
                let _ = session.debugger.remove_breakpoint(at);
                print(&format!("deleted 0x{at:012x}\n"));
            }
            None => error("delete wants an address or a name"),
        },
        "list" | "l" => show_breakpoints(session),
        "run" | "r" => match session.debugger.start() {
            Ok(state) => session.report(&state),
            Err(failure) => error(&failure.message),
        },
        "step" | "s" => match session.debugger.step() {
            Ok(state) => session.report(&state),
            Err(failure) => error(&failure.message),
        },
        "next" | "n" => match session.debugger.step_over() {
            Ok(state) => session.report(&state),
            Err(failure) => error(&failure.message),
        },
        "finish" | "f" => match session.debugger.step_out() {
            Ok(state) => session.report(&state),
            Err(failure) => error(&failure.message),
        },
        "continue" | "c" => match session.debugger.go() {
            Ok(state) => session.report(&state),
            Err(failure) => error(&failure.message),
        },
        "until" | "u" => match rest.first().and_then(|text| session.resolve(text)) {
            Some(at) => match session.debugger.run_to(at) {
                Ok(state) => session.report(&state),
                Err(failure) => error(&failure.message),
            },
            None => error("until wants an address or a name"),
        },
        "registers" | "i" => show_registers(session, rest.first() == Some(&"all")),
        "read" | "m" => {
            let at = rest.first().and_then(|text| session.resolve(text));
            let count = rest
                .get(1)
                .and_then(|text| parse_number(text))
                .unwrap_or(16);
            match at {
                Some(at) => show_memory(session, at, count as usize),
                None => error("read wants an address and a count"),
            }
        }
        "write" | "w" => {
            let at = rest.first().and_then(|text| session.resolve(text));
            let digits: String = rest[1..].concat();
            match (at, decode_hex(&digits)) {
                (Some(at), Some(bytes)) => match session.debugger.write(at, &bytes) {
                    Ok(()) => print(&format!("wrote {} bytes at 0x{at:012x}\n", bytes.len())),
                    Err(failure) => error(&failure.message),
                },
                _ => error("write wants an address and an even number of hex digits"),
            }
        }
        "stack" | "k" => show_stack(session),
        "call" | "a" => match rest.first().and_then(|text| session.resolve(text)) {
            Some(at) => {
                let given: Vec<String> = rest[1..].iter().map(|one| one.to_string()).collect();
                match session.debugger.call(at, &given) {
                    Ok((result, output)) => {
                        if !output.is_empty() {
                            print(&output);
                            if !output.ends_with('\n') {
                                print("\n");
                            }
                        }
                        print(&format!(
                            "-- returned {} (0x{result:x})\n",
                            result as i32
                        ));
                    }
                    Err(failure) => error(&failure.message),
                }
            }
            None => error("call wants an address or a name"),
        },
        "trace" | "t" => {
            if rest.first().map(|text| *text) == Some("off") {
                let _ = session.debugger.set_trace(false);
                session.tracing = false;
                print("no longer recording\n");
                return true;
            }
            // Recording cannot be asked for after the fact, so the first `trace`
            // turns it on and the ones after it show what has been kept since.
            if !session.tracing {
                if let Err(failure) = session.debugger.set_trace(true) {
                    error(&failure.message);
                    return true;
                }
                session.tracing = true;
                print("recording every instruction from here; trace again to see them\n");
                return true;
            }
            let recorded = session.debugger.trace();
            if recorded.is_empty() {
                print("nothing has run since recording started\n");
                return true;
            }
            // The last n of them, since what led up to where it stopped is what
            // a trace is read for.
            let count = rest
                .first()
                .and_then(|text| parse_number(text))
                .unwrap_or(40) as usize;
            let from = recorded.len().saturating_sub(count.max(1));
            let raw = recorded[from..].join("\n");
            match session.program.readable_trace(&raw) {
                Ok(text) => print(&text),
                Err(failure) => error(&failure.message),
            }
        }
        "disassemble" | "x" => {
            let count = rest
                .first()
                .and_then(|text| parse_number(text))
                .unwrap_or(10) as usize;
            match session.program.disassemble_readable(address, count) {
                Ok(text) => print(&text),
                Err(failure) => error(&failure.message),
            }
        }
        "source" | "o" => show_source(session, address),
        other => error(&format!("no such command '{other}'; try help")),
    }
    true
}

/// A run of hex digits, with spaces allowed between the pairs.
fn decode_hex(text: &str) -> Option<Vec<u8>> {
    let digits: String = text.chars().filter(|c| !c.is_whitespace()).collect();
    let digits = digits.strip_prefix("0x").unwrap_or(&digits);
    if digits.is_empty() || digits.len() % 2 != 0 {
        return None;
    }
    (0..digits.len() / 2)
        .map(|i| u8::from_str_radix(&digits[i * 2..i * 2 + 2], 16).ok())
        .collect()
}

// -------------------------------------------------------------------- entry

pub fn run(arguments: &[String]) -> i32 {
    let settings = match parse(arguments) {
        Ok(settings) => settings,
        Err(code) => return code,
    };

    let library = match Library::new(settings.specs.as_ref().map(std::path::Path::new)) {
        Ok(library) => library,
        Err(failure) => {
            crate::out::library_error(&failure);
            return 1;
        }
    };
    let mut program = match Program::open(&settings.path, settings.language.as_deref()) {
        Ok(program) => program,
        Err(failure) => {
            crate::out::library_error(&failure);
            return 1;
        }
    };

    let symbols = program.symbols();
    let mut functions: Vec<(u64, u64, String)> = symbols
        .iter()
        .filter(|symbol| symbol.is_function && !symbol.is_import)
        .map(|symbol| (symbol.address, symbol.size, symbol.name.clone()))
        .collect();
    functions.sort_by_key(|(address, _, _)| *address);
    let names: Vec<(String, u64)> = symbols
        .iter()
        .map(|symbol| (symbol.name.clone(), symbol.address))
        .collect();

    // Where to begin: a named function, an address, or the program's own entry.
    let mut entry = settings.entry;
    if let Some(name) = settings.entry_name.as_deref() {
        match names.iter().find(|(known, _)| known == name) {
            Some((_, address)) => entry = *address,
            None => {
                error(&format!("no symbol named {name}"));
                return 1;
            }
        }
    }

    // argv[0] is the program itself when nothing else was said.
    let mut given = settings.arguments.clone();
    if given.is_empty() {
        given.push(settings.path.clone());
    }

    let debugger = match program.debug(entry, &given, &settings.input, settings.step_limit) {
        Ok(debugger) => debugger,
        Err(failure) => {
            crate::out::library_error(&failure);
            return 1;
        }
    };

    let mut session = Session {
        program,
        debugger,
        functions,
        names,
        tracing: false,
        done: false,
    };

    print(&format!(
        "astral debug {} ({})\n",
        settings.path,
        session.program.language_id()
    ));
    let state = session.debugger.state();
    session.report(&state);

    for line in settings.queued.clone() {
        if !run_command(&mut session, &line) {
            session.done = true;
            break;
        }
    }

    // Standard input carries on where the queued commands left off, unless it
    // is not a terminal and nothing is waiting there.
    let interactive = std::io::stdin().is_terminal();
    if !session.done {
        let stdin = std::io::stdin();
        let mut line = String::new();
        loop {
            if interactive {
                print("(astral) ");
                let _ = std::io::stdout().flush();
            }
            line.clear();
            match stdin.lock().read_line(&mut line) {
                Ok(0) | Err(_) => break,
                Ok(_) => {}
            }
            if !run_command(&mut session, &line) {
                break;
            }
        }
    }

    drop(session);
    drop(library);
    0
}
