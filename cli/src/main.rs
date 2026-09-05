// The astral command.
//
// Subcommands rather than a wall of flags: what you are asking for comes first,
// and the options that follow only have to make sense for that one job.
//
// Everything is in this one program. The terminal interface, the updater and
// the specification compiler are modules rather than siblings found on the
// path, so there is nothing to keep in step and nothing to lose.

mod contribute;
mod help;
mod knowledge;
mod learn;
mod options;
mod out;
mod patch;
mod paths;
mod program;
mod sleigh;
mod update;

use options::Command;
use out::{error, print, Stream};

fn main() {
    let arguments: Vec<String> = std::env::args().skip(1).collect();
    std::process::exit(dispatch(&arguments));
}

fn dispatch(arguments: &[String]) -> i32 {
    let Some(command) = arguments.first().map(String::as_str) else {
        return help::usage(Stream::Err);
    };
    let rest = &arguments[1..];

    match command {
        "--help" | "-h" | "help" => match rest.first().map(String::as_str) {
            // `astral help <command>` is the same as `astral <command> --help`.
            Some(wanted) => topic(wanted),
            None => help::usage(Stream::Out),
        },
        "--version" | "-V" | "version" => {
            if matches!(rest.first().map(String::as_str), Some("--help" | "-h")) {
                return help::version(Stream::Out);
            }
            print(&format!(
                "astral {} (Astral engine)\n",
                astral::version()
            ));
            0
        }
        "update" => update::run_command(rest),
        "sleigh" => sleigh::run(rest),
        "knowledge" => knowledge::run(rest),
        "learn" => learn::run(rest),
        "patch" => patch::run(rest),
        // Con-Tri-Bute, for anyone who types it often.
        "contribute" | "ctb" => contribute::run(rest),
        "info" => with_binary(Command::Info, rest),
        "run" => with_binary(Command::Run, rest),
        "decompile" => with_binary(Command::Decompile, rest),
        "disassemble" | "dis" => with_binary(Command::Disassemble, rest),
        "languages" => with_binary(Command::Languages, rest),
        _ => {
            error(&format!("unknown command '{command}'\n"));
            help::usage(Stream::Err)
        }
    }
}

fn topic(wanted: &str) -> i32 {
    match wanted {
        "decompile" => help::decompile(Stream::Out),
        "info" => help::info(Stream::Out),
        "disassemble" | "dis" => help::disassemble(Stream::Out),
        "languages" => help::languages(Stream::Out),
        "version" => help::version(Stream::Out),
        "knowledge" => knowledge::usage(),
        "learn" => learn::usage(Stream::Out),
        "patch" => patch::usage(Stream::Out),
        "contribute" | "ctb" => contribute::usage(Stream::Out),
        "update" => update::usage(Stream::Out),
        "sleigh" => sleigh::usage(Stream::Out),
        _ => {
            error(&format!("no such command '{wanted}'\n"));
            help::usage(Stream::Err)
        }
    }
}

fn with_binary(command: Command, arguments: &[String]) -> i32 {
    let options = match options::parse(command, arguments) {
        Ok(options) => options,
        Err(code) => return code,
    };

    if command == Command::Languages {
        return program::languages(&options);
    }

    let Some(path) = options.path.clone() else {
        error(&format!("{} needs a binary", name_of(command)));
        return 2;
    };
    if options.raw && options.language.is_none() {
        error("a raw image needs --language too");
        return 2;
    }

    if options.tui {
        // In this process: the interface is a module, not another program.
        return astral_tui::run(vec![path]);
    }

    match command {
        Command::Info => program::info(&options),
        Command::Disassemble => program::disassemble(&options),
        Command::Run => program::run(&options),
        _ => program::decompile(&options),
    }
}

fn name_of(command: Command) -> &'static str {
    match command {
        Command::Run => "run",
        Command::Info => "info",
        Command::Decompile => "decompile",
        Command::Disassemble => "disassemble",
        Command::Languages => "languages",
    }
}
