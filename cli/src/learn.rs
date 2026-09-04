//! Teaching Astral, from binaries that still carry symbols or from source.
//!
//! Every named body becomes a fingerprint, so a stripped program containing the
//! same code is recognised; source contributes the prototypes that turn a
//! readable result into a compilable one.

use crate::out::{error, library_error, print, Sink, Stream};

const HELP: &str = concat!(
    "usage: astral learn [--source] <path>...\n",
    "       astral learn delete <name>...\n",
    "       astral learn delete --all\n",
    "\n",
    "  <binary>          record every named function against its own bytes,\n",
    "                    so the same code is recognised where symbols are gone\n",
    "  --source <path>   read C or C++ source, or a directory of it, and record\n",
    "                    the prototypes it declares: real return types, argument\n",
    "                    types and argument names for anything with that name\n",
    "  delete <name>     forget everything learned under that name\n",
    "  delete --all      empty the learned database; built-in knowledge stays\n",
);

pub fn usage(stream: Stream) -> i32 {
    Sink::new(stream).write(HELP);
    stream.code()
}

pub fn run(arguments: &[String]) -> i32 {
    let Some(first) = arguments.first() else {
        return usage(Stream::Err);
    };
    if first == "--help" || first == "-h" {
        return usage(Stream::Out);
    }
    if first == "delete" || first == "forget" {
        return delete(&arguments[1..]);
    }
    record(arguments)
}

fn delete(arguments: &[String]) -> i32 {
    let Some(first) = arguments.first() else {
        return usage(Stream::Err);
    };

    if first == "--all" {
        if let Err(failure) = astral::forget_all() {
            library_error(&failure);
            return 1;
        }
        let records = astral::knowledge().records;
        print(&format!(
            "learned database emptied; {records} built-in record{} remain\n",
            plural(records)
        ));
        return 0;
    }

    let mut removed = 0;
    for name in arguments {
        match astral::forget(name) {
            Err(failure) => {
                library_error(&failure);
                return 1;
            }
            Ok(0) => error(&format!("nothing learned under {name}")),
            Ok(gone) => {
                print(&format!("{gone:>6}  {name}\n"));
                removed += gone;
            }
        }
    }
    print(&format!(
        "\nforgot {removed} record{}; {} remain\n",
        plural(removed),
        astral::knowledge().records
    ));
    if removed == 0 {
        1
    } else {
        0
    }
}

fn record(arguments: &[String]) -> i32 {
    // A bare path belongs to whichever of the two kinds was named last, so a
    // list of binaries and a list of sources can be given in one command.
    let mut sources: Vec<&String> = Vec::new();
    let mut binaries: Vec<&String> = Vec::new();
    let mut reading_source = false;
    for argument in arguments {
        match argument.as_str() {
            "--source" | "-S" => reading_source = true,
            "--binary" | "-B" => reading_source = false,
            _ => {
                if reading_source {
                    sources.push(argument)
                } else {
                    binaries.push(argument)
                }
            }
        }
    }

    let _library = match astral::Library::new(None) {
        Ok(library) => library,
        Err(failure) => {
            library_error(&failure);
            return 1;
        }
    };

    if !sources.is_empty() {
        let paths: Vec<&str> = sources.iter().map(|s| s.as_str()).collect();
        match astral::learn_source(&paths) {
            Ok(prototypes) => print(&format!("{prototypes:>6}  prototypes from source\n")),
            Err(failure) => {
                library_error(&failure);
                return 1;
            }
        }
    }

    let mut total = 0;
    let mut files = 0;
    let mut skipped = 0;
    for path in &binaries {
        let Ok(mut program) = astral::Program::open(path.as_str(), None) else {
            skipped += 1;
            continue;
        };
        if let Ok(learned) = program.learn_symbols() {
            if learned > 0 {
                print(&format!("{learned:>6}  {path}\n"));
                total += learned;
            }
        }
        files += 1;
    }

    if !binaries.is_empty() {
        print(&format!(
            "\nlearned {total} function{} from {files} binar{}",
            plural(total),
            if files == 1 { "y" } else { "ies" }
        ));
        if skipped != 0 {
            print(&format!(", skipped {skipped} that could not be read"));
        }
        print("\n");
    }

    let knowledge = astral::knowledge();
    print(&format!(
        "{} now holds {} record{}\n",
        knowledge.path,
        knowledge.records,
        plural(knowledge.records)
    ));
    0
}

fn plural(count: usize) -> &'static str {
    if count == 1 {
        ""
    } else {
        "s"
    }
}
