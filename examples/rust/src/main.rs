// Decompiles one function using the Rust bindings.
use astral::{Library, Program};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(1);
    let path = match args.next() {
        Some(path) => path,
        None => {
            eprintln!("usage: decompile <binary> [symbol]");
            std::process::exit(2);
        }
    };
    let symbol = args.next();

    let _library = Library::new(None)?;
    let program = Program::open(&path, None)?;

    println!(
        "{}, {}, {}\n",
        program.format_name(),
        program.language_id(),
        program.compiler_spec()
    );

    let function = match symbol {
        Some(name) => program.decompile_symbol(&name)?,
        None => {
            let entry = *program
                .entry_points()
                .first()
                .ok_or("the image records no entry point")?;
            program.decompile(entry, None)?
        }
    };

    println!("{}  [{}]\n", function.signature(), function.calling_convention());
    println!("{}", function.c_code());
    for call in function.callees() {
        println!("calls: {:#x} {}", call.address, call.name);
    }

    // The same function as C that compiles, rather than as a listing.
    println!("\n/* ---- compilable ---- */");
    print!("{}", program.emit_c(&[function.address()], Default::default())?);
    Ok(())
}
