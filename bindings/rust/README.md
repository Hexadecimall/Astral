# astral

A decompiler that emits C which compiles, as a Rust library.

Astral is built on Ghidra's decompiler core, vendored and linked directly. There
is no JVM, no Ghidra installation and no headless scripting: open a binary, get
source back.

```rust
let _library = astral::Library::new(None)?;
let program = astral::Program::open("./a.out", None)?;
println!("{}", program.emit_c_all(Default::default())?);
```

It reads ELF, PE/COFF, Mach-O and .NET assemblies, names what a stripped binary
forgot from the evidence in it, assembles edits back into the file, and runs a
program on its own emulator rather than handing it to the operating system.

## The library it links against

This crate is a binding. The library itself is built from the Astral repository
and installed alongside, and `build.rs` finds it by looking at `ASTRAL_LIB_DIR`,
then `$ASTRAL_ROOT/lib`, then `~/.astral/lib`, then the system prefixes, then an
in-tree `build/`.

The simplest way to have one is to install a release, which puts the library,
the headers, the compiled processor specifications and this crate in one place:

```sh
astral update
```

Linking is static by default; the `shared` feature links the dynamic library
instead.

## Licence

Apache-2.0. The decompiler engine derives from Ghidra, whose licence and notices
travel with it.
