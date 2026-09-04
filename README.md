# Astral

A decompiler library for C, C++ and Rust that emits **C which compiles**.

Astral is built on Ghidra's decompiler core, vendored and linked directly. There
is no JVM, no Ghidra installation and no headless scripting. Link `-lAstral`,
hand it a binary, get source back.

```c
#include <astral/astral.h>

astral_init(NULL);
astral_program *program = astral_program_open("./a.out", NULL);
char *source = astral_emit_c_all(program, ASTRAL_C_DEFAULT);
puts(source);            /* feed this to a compiler; it builds */
```

## Naming what the binary forgot

A stripped binary has no names, but it is not silent about its intent. It prints
text, it calls library functions whose meaning is fixed, and it uses each value
in a particular shape. Astral reads that evidence and names accordingly.

```c
$ astral decompile --why -a 0x100000460 ./stripped
/* named verifyPassword: its text mentions "password" */
/* param_1 is now ptr */

xunknown8 verifyPassword(char *ptr)
{
  int4 iVar1;
  iVar1 = strcmp(ptr,"hunter2");
  if (iVar1 != 0) {
    puts("password incorrect");
    return 0;
  }
  return 1;
}
```

Nothing is invented. Where the evidence is weak Astral proposes nothing, because
a placeholder is honest and a wrong name is not. `--why` always says what the
evidence was, and `--raw-names` turns the whole thing off.

The rules live in a knowledge base built into the library, in a plain text
format you can read and extend:

| Record | Means |
|---|---|
| `verb fopen openFile` | a function whose dominant call is `fopen` |
| `idiom socket+connect openConnection` | one that calls both |
| `lit password verifyPassword` | one whose text mentions it |
| `role zero_accum sum` | a value set to zero then added to |
| `proto printf ...` | the real prototype of a library function |
| `note halt_baddata ...` | an explanation to attach where it appears |
| `sig <hash> <length> <name>` | a name you chose for that exact body |

## Teaching it in bulk

Most binaries on a machine still have symbols. `astral learn` turns them into a
signature database, so a stripped program containing any of the same code comes
back named.

```sh
$ astral learn /usr/local/bin/*
learned 45407 functions from 25 binaries
```

That takes about a second. A body is recognised by its own bytes with
link-dependent parts masked out, and matching does not depend on knowing where
the function ends: every length the database holds is tried in a single pass,
and the longest match wins because it is the most specific.

## Teaching it from source

A binary says a function exists and what its body does. The source that built it
says what the function is *for*. Reading both is what turns a listing into
something you can compile.

```sh
$ astral learn --source ./src --binary ./build/thing
```

```c
int4 func_0x0001000004a8(int4 *ptr, uint4 param_2)   /* before */
int4 total_of(int4 *v, int4 n)                       /* after  */
```

Real name, real argument names, and `n` correctly signed. Prototypes are applied
at the moment a function is named, so a body recognised by its fingerprint picks
up the types and argument names its author wrote. Types that cannot be expressed
exactly are skipped rather than guessed: a wrong prototype is worse than none.

`astral learn delete <name>` forgets one thing; `delete --all` empties the
database. Both rewrite it, so forgetting is as durable as learning was.

## Renaming teaches it

Rename anything and the name propagates: the function is rebuilt under it, so
it reaches the definition and every call site that refers to it.

The choice is also recorded, against a fingerprint of the function's body rather
than its address. The same code in a different program comes back named, even
stripped, even in a later session:

```sh
$ astral decompile -a 0x1000 ./build-with-symbols   # you rename it checkSecret
$ astral decompile -a 0x1000 ./stripped-release
/* named checkSecret: you named this body before */
```

Fingerprints mask the parts of an instruction that depend on where it was
linked, so relocation does not defeat them, and a fingerprint claimed by two
different names is discarded rather than guessed at. `astral knowledge` reports
what is loaded and where your own database lives.

## Contributing what you have taught it

```sh
$ astral ctb database
Checking for violations... None.
Submitting... Success.
https://github.com/Hexadecimall/Astral/issues/12
```

Astral reads the policy the repository publishes in `contrib/policy.astral`,
which says whether submissions are open and which record kinds may travel.
Everything else is dropped locally, before the network is touched. `--dry-run`
checks and reports without sending.

It arrives as a pull request carrying the whole database, into
`contrib/databases`, and every file there is compiled into the knowledge base
at build time. Merging one is all it takes for those names to reach everyone.

No account is needed. It takes whichever route is open, in this order: a
submission service the policy names, a GitHub token this machine already has,
or, for almost everyone, the database written to a file and GitHub's upload
page opened in the browser, which forks and opens the pull request for you.

```sh
$ astral ctb database
Checking for violations... None.
Submitting... ready in your browser.
https://github.com/Hexadecimall/Astral/upload/main/contrib/databases
Upload: ~/<hash>.astral
```

What travels is a fingerprint and the name someone chose for it. Records
mentioning a path or an address never leave the machine. Bear in mind that the
names themselves come from binaries you have taught it, and a public repository
is a public place.

## Real C, not a listing

Ghidra's output reads like C but does not compile. It names types by byte width
(`int4`, `undefined8`), calls operations with no C spelling (`CONCAT44`,
`SUB84`, `ZEXT48`), writes parts of variables as `value._8_8_`, returns arrays
from functions, and leaves everything it references undeclared.

Astral keeps that listing available and adds a translation unit a compiler
accepts:

| Ghidra emits | What it means | What Astral emits |
|---|---|---|
| `int4`, `undefined8`, `unkbyte9` | sized machine types | real `stdint.h` typedefs |
| `CONCAT44`, `SUB84`, `ZEXT48`, `CARRY4` | p-code operations | generated `static inline` definitions, per width |
| `value._8_8_ = x` | write eight bytes at offset eight | `ASTRAL_STORE`, which copies bytes rather than converting the number |
| `xunknown1 [16] f(void)` | value returned in two registers | a struct of that size, which C can return |
| `func_0x1234(a, b)` | call to an unnamed target | a prototype matching the call site |
| `iRam000100c068` | an unnamed global | an `extern` with the type its name encodes |
| `xunknown8 main(void)` | recovered entry point | `int main(void)`, as C requires |

Imported functions are resolved to their real names, their arguments typed from
a table of library prototypes, and the header that declares them included. That
is what turns an address into a string:

```c
$ astral decompile -f main ./hello
/* Decompiled by Astral. Reverse engineer only what you have the right to. */

#include <stdio.h>

int main(void)
{
  printf("Hello, World!\n");
  return 0;
}
```

The claim is tested rather than asserted. The suite compiles the emitted unit,
links it, runs it, and checks it behaves like the original program.

```sh
astral decompile --all ./a.out > recovered.c
cc -std=c11 -c recovered.c          # this is the point
```

Three limits worth knowing. Variadic arguments past the format string are not
recovered, so `printf("%s", name)` comes back as `printf("%s")`. A function that
reads absolute addresses needs the original image's data, which is not emitted,
so it compiles but will fault if run alone. And where the decompiler's reading
of the machine code is wrong, the C is faithfully wrong in the same way.

## What is in here

The decompiler engine in `engine/` is derived from Ghidra's C++ decompiler
(Apache-2.0) and maintained as first-party source, modified in place to fit
Astral. Everything Ghidra normally supplies from its Java
side, plus the parts it has no answer for, live in `src/`:

| Piece | Where | Replaces |
|---|---|---|
| ELF reader | `src/loader_elf.cc` | `ElfLoader` and friends |
| PE/COFF reader | `src/loader_pe.cc` | `PeLoader`, export and import parsing |
| Mach-O reader | `src/loader_macho.cc` | `MachoLoader`, universal binaries, import stubs |
| Language selection | `src/langmap.cc` | the processor `.opinion` files |
| Image model | `src/image.cc` | `Memory`, `MemoryBlock` |
| Decompiler session | `src/session.cc` | `DecompInterface` |
| Library prototypes | `src/libc_protos.cc` | Ghidra's data-type archives |
| Real-C emitter | `src/creal.cc` | nothing upstream; this is new |
| C ABI | `src/capi.cc` | — |
| C++ API | `src/cpp_api.cc` | — |

Each loader turns a file into the same `BinaryImage`: segments, symbols, entry
points and an architecture hint. The session binds that image to a
`SleighArchitecture`, so the decompiler sees a normal program.

## Building

Needs CMake 3.20+, a C++17 compiler and zlib. Cargo as well, for the interface.

```sh
./install.sh                 # build and install where the system expects it
./install.sh --user          # under ~/.astral instead, no elevation
./install.sh --languages ALL # every processor Ghidra ships
./install.sh --uninstall
```

The location depends on what the system allows: `/usr/local` on a macOS with
System Integrity Protection, `/usr` without it and on Linux, and
`C:\Program Files\Astral` on Windows. It builds as you and asks for elevation
only for the install step, and only when something it must write to is not
already writable.

```
<prefix>/
  bin/       astral - one program; the interface is linked into it
  include/   astral/astral.h, astral/astral.hpp, astral/decompiled.h
  lib/astral/dynamic-libs/   libAstral.dylib
  lib/astral/static-libs/    libAstral.a
  share/astral/             specs, and the Rust crate
```

Or drive CMake yourself:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel
```

Specs for x86, AARCH64, ARM, MIPS, PowerPC, RISC-V, JVM, SPARC, 68000, Z80, PIC,
8051, SuperH4, Xtensa, Toy and DATA are compiled by default. Choose differently
with `-DASTRAL_LANGUAGES="x86;AARCH64"`, or `ALL` for every processor Ghidra
ships.

## Installing and updating

`astral update` downloads the newest release, builds it and installs it. No
source tree is needed: the one this copy was built from may not exist on the
machine at all, and no path is compiled in.

```sh
astral update                      # fetch the newest release and install it
astral update install              # into /usr/local rather than over this copy
astral update --check              # what is installed against what is newest
astral update --release v0.2.0     # a particular release
astral update --local              # build a source tree you are standing in
astral update --ghidra 12.1.3      # vendor that Ghidra release first
astral update --languages ALL      # every processor's specs
```

The `install` subcommand targets `/usr/local` and checks each destination
directory it will write to. It builds as the current user either way, and asks
for sudo only for the install step, and only when something it must write to is
not already writable.

## Using it

### C

```sh
cc main.c -I<prefix>/include -L<prefix>/lib/astral/static-libs -lAstral -lz -lc++
```

`astral_init(NULL)` finds the specs through the compiled-in install path, or
through `ASTRAL_SPECS` when that is set. Handles are opaque; accessor strings
live as long as their handle, while `astral_disassemble`, `astral_pcode` and
`astral_emit_c` hand back strings the caller releases with
`astral_string_free`. Failures return null or a negative `astral_status` and
leave a message in `astral_last_error()`.

### C++

`astral/astral.hpp` is compiled into the same library, so `-lAstral` is all it
needs.

```cpp
astral::Library library;                        // init/shutdown, scoped
auto program  = astral::Program::open("./a.out");
auto function = program.decompile("main");
std::cout << function.c_code();                 // the listing
std::cout << program.emit_c({function.address()});   // C that compiles
```

Handles are move-only; failures raise `astral::Error`.

### Rust

```toml
[dependencies]
astral = { path = "/usr/local/share/astral/rust" }
```

```rust
let _library = astral::Library::new(None)?;
let program = astral::Program::open("./a.out", None)?;
println!("{}", program.emit_c_all(Default::default())?);
```

`build.rs` looks for the library in `ASTRAL_LIB_DIR`, then `$ASTRAL_ROOT/lib`,
then `~/.astral/lib`, then the system prefixes, then an in-tree `build/`. It links
statically; enable the `shared` feature for the dynamic library. `generate.sh`
regenerates the raw declarations with bindgen.

## Command line

One program. `astral --help` prints the whole tree; every subcommand has its
own `--help`.

```sh
astral info ./a.out                    # format, language, segments, symbols
astral decompile ./a.out               # the entry point, as C that compiles
astral decompile -f main ./a.out       # one function by name; repeatable
astral decompile -a 0x100004000 ./a.out  # by address; repeatable
astral decompile --all ./a.out         # the whole program
astral decompile -p -f main ./a.out    # the decompiler's listing instead
astral decompile --why -f main ./a.out # explain every name it chose
astral decompile --tui ./a.out         # explore it in the terminal
astral disassemble -d 20 -a 0x1000 ./a.out
astral disassemble -p 20 -a 0x1000 ./a.out     # p-code instead
astral decompile --raw 0x8000000 -l "ARM:LE:32:v8" firmware.bin
astral languages                       # what this build can read
astral knowledge                       # what it knows, and has learned
astral learn ./thing                   # names, from a binary that has symbols
astral learn --source ./src            # prototypes, from the source
astral learn delete --all              # forget everything you taught it
astral ctb database                    # offer it back to the project
astral sleigh in.slaspec out.sla       # compile a processor specification
astral update                          # fetch and install the newest release
astral version
```

Compilable C is what `decompile` produces. `-p` or `--pseudo-c` gives the
decompiler's raw listing, which reads as C but does not build.

Imported stubs are named but never decompiled: their bodies belong to another
image, so `--all` covers this program's own code.

## The interface

`astral decompile --tui <binary>` opens a terminal interface over the same
library, in the same process. A filterable symbol list, the decompiled function
beside it, and a details pane of parameters, locals and callees.

| Key | Does |
|---|---|
| `↑` `↓` `j` `k` `PgUp` `PgDn` `g` `G` | move |
| `Tab` | cycle panes |
| `1` `2` `3` `4`, `v` | decompiled C, compilable C, disassembly, p-code |
| `Enter` | decompile the selection, or jump to a callee |
| `/` | filter by name or address |
| `r` | rename, which also teaches it |
| `x` | who calls this |
| `s` | save the pane to a file |
| `b` | toggle the details pane |
| `Backspace` `h` | back |
| `?` | help |
| `q` | quit |

The first frame is always painted before any decompiling starts, so it never
looks hung, and the terminal is restored even if the process is killed.

## Tests

```sh
tests/run_tests.sh              # against ./build
```

The suite generates ELF and PE fixtures with `tests/make_fixtures.py`, compiles
a Mach-O with the host compiler, then checks loading, language selection, symbol
recovery, decompilation, raw mode, p-code and error reporting for each. The
real-C checks go further: they compile the emitted unit, link and run it against
the behaviour of the original source, and rebuild a whole program from its own
decompilation to confirm it still prints what it printed before.

## Licensing

The decompiler engine in `engine/` and the SLEIGH specifications in `sleigh/`
derive from Ghidra (Apache-2.0); the licence and notices travel with them in
`engine/LICENSE` and `engine/NOTICE`. The rest of `src/`,
`include/`, `tools/`, `bindings/` and `tests/` is new work in this repository.
