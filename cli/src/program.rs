//! The commands that open a binary: info, disassemble and decompile.

use astral::{Function, Library, Program};

use crate::options::Options;
use crate::out::{error, library_error, paint, print, stdout_colour, tint, Destination};

/// A program and the specification tree it was read with.
///
/// The fields are declared in the order they have to be released: the program
/// first, then the global state the library holds.
struct Opened {
    program: Program,
    _library: Library,
}

fn open(options: &Options) -> Option<Opened> {
    let library = match Library::new(options.specs.as_ref().map(std::path::Path::new)) {
        Ok(library) => library,
        Err(failure) => {
            library_error(&failure);
            return None;
        }
    };
    let path = options.path.as_deref().unwrap_or_default();
    let opened = if options.raw {
        // The parser has already refused a raw image with no language.
        Program::open_raw(path, options.language.as_deref().unwrap_or(""), options.raw_base)
    } else {
        Program::open(path, options.language.as_deref())
    };
    let mut program = match opened {
        Ok(program) => program,
        Err(failure) => {
            library_error(&failure);
            return None;
        }
    };
    if let Some(count) = options.threads {
        program.set_threads(count);
    }
    if options.raw_names {
        program.set_auto_naming(false);
    }
    Some(Opened {
        program,
        _library: library,
    })
}

/// The addresses a command was asked for: those named, those given, or the
/// entry point when nothing narrowed it down.
fn wanted_addresses(program: &Program, options: &Options) -> Vec<u64> {
    let symbols = program.symbols();
    let mut wanted = options.addresses.clone();
    for name in &options.functions {
        match symbols.iter().find(|symbol| &symbol.name == name) {
            Some(symbol) => wanted.push(symbol.address),
            None => error(&format!("no symbol named {name}")),
        }
    }
    if wanted.is_empty() && options.all {
        wanted.extend(
            symbols
                .iter()
                // An import's body lives in another image, so there is nothing
                // here to recover.
                .filter(|symbol| symbol.is_function && !symbol.is_import)
                .map(|symbol| symbol.address),
        );
    }
    if wanted.is_empty() {
        if let Some(entry) = program.entry_points().first() {
            wanted.push(*entry);
        }
    }
    wanted
}

pub fn info(options: &Options) -> i32 {
    let Some(opened) = open(options) else {
        return 1;
    };
    let program = &opened.program;

    print(&format!(
        "file       {}\n",
        options.path.as_deref().unwrap_or_default()
    ));
    print(&format!("{}     {}\n", tint(paint::DIM, "format"), program.format_name()));
    print(&format!("{}   {}\n", tint(paint::DIM, "language"), program.language_id()));
    print(&format!("{}   {}\n", tint(paint::DIM, "compiler"), program.compiler_spec()));
    print(&format!(
        "endian     {}\n",
        if program.is_big_endian() {
            "big"
        } else {
            "little"
        }
    ));
    print(&format!("pointer    {} bytes\n", program.pointer_size()));
    print(&format!("image base 0x{:x}\n", program.image_base()));
    for entry in program.entry_points() {
        print(&format!("entry      0x{entry:x}\n"));
    }

    let segments = program.segments();
    print(&format!("\nsegments ({})\n", segments.len()));
    for segment in &segments {
        print(&format!(
            "  {:<24} 0x{:012x} {:>10} {}{}\n",
            segment.name,
            segment.address,
            segment.size,
            if segment.executable { "x" } else { "-" },
            if segment.writable { "w" } else { "-" },
        ));
    }

    let symbols = program.symbols();
    let imports = symbols.iter().filter(|s| s.is_import).count();
    let functions = symbols
        .iter()
        .filter(|s| !s.is_import && s.is_function)
        .count();
    print(&format!(
        "\nsymbols ({}: {} functions, {} imported)\n",
        symbols.len(),
        functions,
        imports
    ));
    for symbol in &symbols {
        print(&format!(
            "  0x{:012x} {}{}{}\n",
            symbol.address,
            if symbol.is_function { "" } else { "(data) " },
            if symbol.is_import { "(import) " } else { "" },
            symbol.name,
        ));
    }
    0
}

pub fn disassemble(options: &Options) -> i32 {
    let Some(opened) = open(options) else {
        return 1;
    };
    let program = &opened.program;

    let wanted = wanted_addresses(program, options);
    let Some(&start) = wanted.first() else {
        error("nothing to disassemble; pass --address");
        return 1;
    };

    let as_pcode = options.pcode > 0;
    let count = if as_pcode {
        options.pcode
    } else if options.disassemble > 0 {
        options.disassemble
    } else {
        20
    };
    let text = if as_pcode {
        program.pcode(start, count)
    } else {
        program.disassemble(start, count)
    };
    match text {
        Ok(text) => {
            let text = match (stdout_colour(options.color), as_pcode) {
                (false, _) => text,
                (true, true) => astral_tui::ansi_pcode(&text),
                (true, false) => astral_tui::ansi_assembly(&text),
            };
            print(&text);
            0
        }
        Err(failure) => {
            library_error(&failure);
            1
        }
    }
}

/// The names Astral chose, and why, as comments above the body they belong to.
fn report_naming(out: &mut Destination, function: &Function) {
    let reason = function.naming_reason();
    if !reason.is_empty() {
        out.write(&format!("/* named {reason} */\n"));
    }
    for (from, to) in function.applied_renames() {
        out.write(&format!("/* {from} is now {to} */\n"));
    }
    for comment in function.comments() {
        out.write(&format!("/* note: {comment} */\n"));
    }
}

pub fn decompile(options: &Options) -> i32 {
    let Some(opened) = open(options) else {
        return 1;
    };
    let program = &opened.program;

    let mut out = match Destination::open(options.output.as_deref()) {
        Ok(out) => out,
        Err(_) => {
            error(&format!(
                "cannot write {}",
                options.output.as_deref().unwrap_or_default()
            ));
            return 1;
        }
    };

    // Colour only on the way to a terminal: a file or a pipe gets the plain C.
    let colour = options.output.is_none() && stdout_colour(options.color);
    let paint_c = |text: String| if colour { astral_tui::ansi_c(&text) } else { text };

    let wanted = wanted_addresses(program, options);
    let mut code = 0;
    if wanted.is_empty() {
        error("nothing to decompile; pass --function or --address");
        code = 1;
    } else if options.pseudo_c {
        // The decompiler's own listing: readable, but not a translation unit.
        for address in wanted {
            match program.decompile(address, None) {
                Ok(function) => {
                    if options.why {
                        report_naming(&mut out, &function);
                    }
                    out.write(&paint_c(function.c_code()));
                }
                Err(failure) => {
                    error(&format!("0x{address:x}: {}", failure.message));
                    code = 1;
                }
            }
        }
    } else {
        let mut emit = options.c_options;
        // The explanations are off in emitted C unless they were asked for.
        emit.explain = options.why;
        match program.emit_c(&wanted, emit) {
            Ok(text) => out.write(&paint_c(text)),
            Err(failure) => {
                library_error(&failure);
                code = 1;
            }
        }
    }

    if let Some(path) = options.output.as_deref() {
        drop(out);
        crate::out::Sink::new(crate::out::Stream::Err).write(&format!("written to {path}\n"));
    }
    code
}

pub fn languages(options: &Options) -> i32 {
    let library = match Library::new(options.specs.as_ref().map(std::path::Path::new)) {
        Ok(library) => library,
        Err(failure) => {
            library_error(&failure);
            return 1;
        }
    };
    for language in library.languages() {
        print(&format!("{:<40} {}\n", language.id, language.description));
    }
    0
}
