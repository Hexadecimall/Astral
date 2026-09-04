//! Options for the four commands that read a binary.
//!
//! One parser serves all of them, because the options overlap almost entirely;
//! where a letter means different things the command decides, which is why the
//! command is passed in rather than inferred afterwards.

use crate::help;
use crate::out::{error, Stream};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Command {
    Info,
    Decompile,
    Disassemble,
    Languages,
}

impl Command {
    /// The usage message this command prints for itself.
    pub fn usage(self, stream: Stream) -> i32 {
        match self {
            Command::Info => help::info(stream),
            Command::Decompile => help::decompile(stream),
            Command::Disassemble => help::disassemble(stream),
            Command::Languages => help::languages(stream),
        }
    }
}

/// When to colour output that goes to standard output.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ColorMode {
    Auto,
    Always,
    Never,
}

pub struct Options {
    pub color: ColorMode,
    pub path: Option<String>,
    pub language: Option<String>,
    pub specs: Option<String>,
    pub output: Option<String>,
    pub functions: Vec<String>,
    pub addresses: Vec<u64>,
    pub raw_base: u64,
    pub raw: bool,
    pub all: bool,
    /// Ask for the decompiler's own listing rather than compilable C.
    pub pseudo_c: bool,
    pub tui: bool,
    pub why: bool,
    pub raw_names: bool,
    pub c_options: astral::COptions,
    pub disassemble: usize,
    pub pcode: usize,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            color: ColorMode::Auto,
            path: None,
            language: None,
            specs: None,
            output: None,
            functions: Vec::new(),
            addresses: Vec::new(),
            raw_base: 0,
            raw: false,
            all: false,
            pseudo_c: false,
            tui: false,
            why: false,
            raw_names: false,
            c_options: astral::COptions::default(),
            disassemble: 0,
            pcode: 0,
        }
    }
}

/// A number written the way C reads one: `0x` for hex, a leading zero for
/// octal, anything else decimal. The whole string has to be consumed.
pub fn parse_number(text: &str) -> Option<u64> {
    let text = text.trim_start();
    let (digits, radix) = if let Some(rest) = text.strip_prefix("0x").or(text.strip_prefix("0X")) {
        (rest, 16)
    } else if text.len() > 1 && text.starts_with('0') {
        (&text[1..], 8)
    } else {
        (text, 10)
    };
    if digits.is_empty() {
        return None;
    }
    u64::from_str_radix(digits, radix).ok()
}

/// A count, read the way `atoi` reads one: leading digits, and zero when there
/// are none.
fn parse_count(text: &str) -> usize {
    let digits: String = text
        .trim_start()
        .chars()
        .take_while(|c| c.is_ascii_digit())
        .collect();
    digits.parse().unwrap_or(0)
}

/// Parses what follows the command name. `Err` carries an exit code, and the
/// message that goes with it has already been printed.
pub fn parse(command: Command, arguments: &[String]) -> Result<Options, i32> {
    let mut options = Options::default();
    let mut index = 0;

    while index < arguments.len() {
        let argument = arguments[index].as_str();
        // Reads the value belonging to the option just seen.
        let value = |name: &str, index: &mut usize| -> Result<String, i32> {
            if *index + 1 >= arguments.len() {
                error(&format!("{name} needs a value"));
                return Err(2);
            }
            *index += 1;
            Ok(arguments[*index].clone())
        };

        match argument {
            "--help" | "-h" => return Err(command.usage(Stream::Out)),
            "-f" | "--function" => {
                let name = value("--function", &mut index)?;
                options.functions.push(name);
            }
            "-a" | "--address" => {
                let text = value("--address", &mut index)?;
                match parse_number(&text) {
                    Some(address) => options.addresses.push(address),
                    None => return Err(help::decompile(Stream::Err)),
                }
            }
            "--all" => options.all = true,
            "-e" | "--entry" => options.addresses.clear(),
            "-o" | "--output" => options.output = Some(value("--output", &mut index)?),
            "--tui" => options.tui = true,
            "--color" | "--colour" => {
                options.color = match value("--color", &mut index)?.as_str() {
                    "auto" => ColorMode::Auto,
                    "always" | "yes" | "force" => ColorMode::Always,
                    "never" | "no" | "none" => ColorMode::Never,
                    other => {
                        error(&format!("--color wants auto, always or never, not {other}"));
                        return Err(2);
                    }
                };
            }
            "--no-color" | "--no-colour" => options.color = ColorMode::Never,
            "--why" => options.why = true,
            "--raw-names" => options.raw_names = true,
            "--runtime-include" => options.c_options.self_contained = false,
            "--no-comments" => options.c_options.comments = false,
            "-l" | "--language" => options.language = Some(value("--language", &mut index)?),
            "-r" | "--raw" => {
                let text = value("--raw", &mut index)?;
                match parse_number(&text) {
                    Some(base) => options.raw_base = base,
                    None => return Err(help::decompile(Stream::Err)),
                }
                options.raw = true;
            }
            "-d" | "--disassemble" => {
                let text = value("--disassemble", &mut index)?;
                options.disassemble = parse_count(&text);
            }
            // The one letter that differs: a listing for decompile, a p-code
            // count everywhere else.
            "-p" if command == Command::Decompile => options.pseudo_c = true,
            "--pseudo-c" => options.pseudo_c = true,
            "-p" | "--pcode" => {
                let text = value("--pcode", &mut index)?;
                options.pcode = parse_count(&text);
            }
            "-s" | "--specs" => options.specs = Some(value("--specs", &mut index)?),
            _ if argument.starts_with('-') && !argument.is_empty() => {
                error(&format!("unknown option {argument}"));
                return Err(command.usage(Stream::Err));
            }
            _ => options.path = Some(argument.to_string()),
        }
        index += 1;
    }

    Ok(options)
}
