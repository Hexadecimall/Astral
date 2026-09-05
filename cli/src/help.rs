//! Every usage message the command prints.
//!
//! Each returns the exit code that goes with where it was written: a help
//! someone asked for succeeds, a help printed because the command was wrong
//! does not.

use crate::out::{Sink, Stream};

/// The whole command set at a glance. A tree because the shape of the tool is
/// part of the answer: what it can do, and which options belong to what.
pub fn usage(stream: Stream) -> i32 {
    let mut out = Sink::new(stream);
    out.write(&format!(
        "astral {} - a decompiler that emits C which compiles\n\
         \n\
         usage: astral <command> [options]\n\
         \n",
        astral::version()
    ));
    out.write(concat!(
        "├─ decompile <binary>  Recover source from a binary\n",
        "│  ├─ -f, --function <name>  a named function (repeatable)\n",
        "│  ├─ -a, --address <hex>    a function at an address (repeatable)\n",
        "│  ├─ -e, --entry            the entry point (the default)\n",
        "│  ├─     --all              every function in the program\n",
        "│  ├─ -p, --pseudo-c         the decompiler's listing, not compilable C\n",
        "│  ├─ -o, --output <file>    write there instead of standard output\n",
        "│  ├─     --tui              explore it in the terminal interface\n",
        "│  ├─     --color <mode>     auto (a terminal), always, or never\n",
        "│  ├─ -j, --threads <n>      engines to decompile with (0 = one per core)\n",
        "│  ├─     --why              explain the names Astral chose\n",
        "│  ├─     --raw-names        name nothing from evidence\n",
        "│  ├─     --runtime-include  #include <astral/decompiled.h> in emitted C\n",
        "│  ├─     --no-comments      leave the decompiler's warnings out\n",
        "│  ├─ -l, --language <id>    force a language, e.g. x86:LE:64:default\n",
        "│  ├─ -r, --raw <hex>        treat the file as a flat image at this address\n",
        "│  └─ -s, --specs <dir>      SLEIGH specification root\n",
        "├─ info <binary>  Format, language, segments and symbols\n",
        "│  ├─ -l, --language <id>    force a language\n",
        "│  ├─ -r, --raw <hex>        treat the file as a flat image\n",
        "│  └─ -s, --specs <dir>      SLEIGH specification root\n",
        "├─ disassemble <binary>  Instructions, or p-code\n",
        "│  ├─ -a, --address <hex>    where to start\n",
        "│  ├─ -d, --disassemble <n>  how many instructions (default 20)\n",
        "│  ├─     --color <mode>       auto (a terminal), always, or never\n",
        "│  ├─ -p, --pcode <n>        p-code for n instructions instead\n",
        "│  ├─ -l, --language <id>    force a language\n",
        "│  └─ -r, --raw <hex>        treat the file as a flat image\n",
        // `run` is commented out in main.rs; debug does the same and more.
        // "├─ run <binary>  Run it on the emulator, without an operating system\n",
        // "│  ├─ -f, --function <name>  start there rather than at the entry point\n",
        // "│  ├─ -a, --address <hex>    start at an address\n",
        // "│  ├─     --arg <text>       an argument to hand it (repeatable)\n",
        // "│  ├─     --input <text>     what the program reads\n",
        // "│  └─     --steps <n>        how many instructions to allow\n",
        "├─ debug <binary>  Watch it run, an instruction at a time\n",
        "│  ├─ -f, --function <name>  start there rather than at the entry point\n",
        "│  ├─ -a, --address <hex>    start at an address\n",
        "│  ├─     --arg <text>       an argument to hand it (repeatable)\n",
        "│  ├─     --input <text|@f>  what the program reads\n",
        "│  ├─     --steps <n>        how many instructions to allow\n",
        "│  ├─     --command <text>   run this before reading input (repeatable)\n",
        "│  └─     --script <file>    run the commands in a file\n",
        "├─ learn  Teach it, from binaries or from source\n",
        "│  ├─ <binary>...            names, against a fingerprint of each body\n",
        "│  ├─ --source <path>...     prototypes and argument names, from C, C++,\n",
        "│  │                        Rust, Go or assembly\n",
        "│  ├─ delete <name>...       forget what was learned under a name\n",
        "│  └─ delete --all           empty the learned database\n",
        "├─ knowledge  What it knows, and where it writes what it learns\n",
        "│  └─ --path                 print the database path and nothing else\n",
        "├─ contribute database  Offer your database to the project (also: ctb)\n",
        "│  └─ --dry-run              check and report, send nothing\n",
        "├─ sleigh <in.slaspec> <out.sla>  Compile a processor specification\n",
        "├─ update  Rebuild and reinstall Astral\n",
        "│  ├─ install                build and install into the system location\n",
        "│  ├─ --check                installed against the latest release\n",
        "│  ├─ --source               build from source rather than download a build\n",
        "│  ├─ --ghidra <version>     vendor that release, then rebuild\n",
        "│  ├─ --languages <list>     processors to compile specs for, or ALL\n",
        "│  └─ --prefix <dir>         install somewhere else\n",
        "├─ languages  Processors this build can read\n",
        "├─ crap-ya-dont-need  Commands nobody asked for\n",
        "│  ├─ binary <file>          decompile it to binary. All of it.\n",
        "│  └─ binary-compile <file>  compile that back into the program\n",
        "└─ version  Print the version\n",
        "\n",
        "Run astral <command> --help for one command on its own.\n",
    ));
    stream.code()
}

pub fn decompile(stream: Stream) -> i32 {
    Sink::new(stream).write(concat!(
        "usage: astral decompile [options] <binary>\n",
        "\n",
        "Recovers source from a binary. With no function given, the entry point.\n",
        "The output is C that compiles, carrying only what the recovered code\n",
        "refers to; --pseudo-c asks for the decompiler's own listing instead.\n",
        "\n",
        "  -f, --function <name>  a named function (repeatable)\n",
        "  -a, --address <hex>    a function at an address (repeatable)\n",
        "  -e, --entry            the entry point (the default)\n",
        "      --all              every function in the program\n",
        "  -p, --pseudo-c         the decompiler's listing, not compilable C\n",
        "  -o, --output <file>    write there instead of standard output\n",
        "      --tui              explore it in the terminal interface\n",
        "      --color <mode>     colour the C: auto (a terminal), always, never\n",
        "  -j, --threads <n>      engines to decompile with (0 = one per core, 1 = no\n",
        "                         extra threads). Each runs over the same image, so a\n",
        "                         count trades a little accuracy of shared context for\n",
        "                         speed; a given count always gives the same output.\n",
        "      --why              explain the names Astral chose\n",
        "      --raw-names        name nothing from evidence\n",
        "      --runtime-include  #include <astral/decompiled.h> in emitted C\n",
        "      --no-comments      leave the decompiler's warnings out\n",
        "  -l, --language <id>    force a language, e.g. x86:LE:64:default\n",
        "  -r, --raw <hex>        treat the file as a flat image at this address\n",
        "  -s, --specs <dir>      SLEIGH specification root\n",
        "\n",
        "  astral decompile --all ./a.out > recovered.c\n",
        "  astral decompile --why -f main ./a.out\n",
    ));
    stream.code()
}

pub fn info(stream: Stream) -> i32 {
    Sink::new(stream).write(concat!(
        "usage: astral info [options] <binary>\n",
        "\n",
        "Reports what the file is: its format, the language and compiler Astral\n",
        "picked for it, its segments, and every symbol it carries. Imported\n",
        "symbols are marked, because their bodies live in another image.\n",
        "\n",
        "  -l, --language <id>  force a language rather than deriving one\n",
        "  -r, --raw <hex>      treat the file as a flat image at this address\n",
        "  -s, --specs <dir>    SLEIGH specification root\n",
    ));
    stream.code()
}

pub fn disassemble(stream: Stream) -> i32 {
    Sink::new(stream).write(concat!(
        "usage: astral disassemble [options] <binary>\n",
        "\n",
        "Instructions, or the p-code they lower to. With no address, the entry\n",
        "point.\n",
        "\n",
        "  -a, --address <hex>    where to start\n",
        "  -d, --disassemble <n>  how many instructions (default 20)\n",
        "  -p, --pcode <n>        p-code for n instructions instead\n",
        "      --raw-listing      the plain listing, without names or labels\n",
        "      --color <mode>     auto (a terminal), always, never\n",
        "  -l, --language <id>    force a language\n",
        "  -r, --raw <hex>        treat the file as a flat image at this address\n",
        "  -s, --specs <dir>      SLEIGH specification root\n",
    ));
    stream.code()
}

pub fn languages(stream: Stream) -> i32 {
    Sink::new(stream).write(concat!(
        "usage: astral languages [--specs <dir>]\n",
        "\n",
        "Every processor this build can read, as language ids. Which ones are\n",
        "present depends on the specifications compiled at build time; pass\n",
        "ASTRAL_LANGUAGES=ALL to the build for all of them.\n",
    ));
    stream.code()
}

pub fn version(stream: Stream) -> i32 {
    Sink::new(stream).write(concat!(
        "usage: astral version\n",
        "\n",
        "Astral's version.\n",
    ));
    stream.code()
}

pub fn run(stream: Stream) -> i32 {
    Sink::new(stream).write(concat!(
        "usage: astral run [options] <binary>\n",
        "\n",
        "Runs the program rather than only reading it. The instructions are\n",
        "stepped as p-code, so an arm64 program runs on an x86 machine and a\n",
        "Windows one runs on a Mac. Nothing is handed to the operating system:\n",
        "the memory is Astral's, and a call into the C library is answered by\n",
        "Astral. A binary you would not want to run still says what it does.\n",
        "\n",
        "  -f, --function <name>  start there rather than at the entry point\n",
        "  -a, --address <hex>    start at an address\n",
        "      --arg <text>       an argument to hand it (repeatable, argv[0] first)\n",
        "      --input <text>     what the program reads\n",
        "      --steps <n>        how many instructions to allow (default 2000000)\n",
        "  -l, --language <id>    force a language\n",
        "  -s, --specs <dir>      SLEIGH specification root\n",
        "\n",
        "  astral run ./crackme --arg ./crackme --arg letmein\n",
    ));
    stream.code()
}

pub fn debug(stream: Stream) -> i32 {
    Sink::new(stream).write(concat!(
        "usage: astral debug [options] <binary>\n",
        "\n",
        "Watches the program run rather than only reporting on it. It is stopped\n",
        "where it is told, and while it is stopped its registers, its memory and\n",
        "its call stack can be read and changed. Nothing is handed to the\n",
        "operating system: the process being debugged is not a process, so a\n",
        "program for another processor debugs the same as a native one and\n",
        "nothing it does can escape.\n",
        "\n",
        "  -f, --function <name>  start there rather than at the entry point\n",
        "  -a, --address <hex>    start at an address\n",
        "      --arg <text>       an argument to hand it (repeatable, argv[0] first)\n",
        "      --input <text>     what the program reads; @file reads a file\n",
        "      --steps <n>        how many instructions to allow (default 2000000)\n",
        "      --command <text>   run this before reading input (repeatable)\n",
        "      --script <file>    run the commands in a file, one per line\n",
        "  -l, --language <id>    force a language\n",
        "  -s, --specs <dir>      SLEIGH specification root\n",
        "\n",
        "commands, each with a one-letter form:\n",
        "  break, b <addr|name>   stop when it reaches there\n",
        "  delete, d <addr|name>  remove that breakpoint\n",
        "  list, l                the breakpoints set\n",
        "  run, r                 back to the first instruction, nothing executed\n",
        "  step, s                one instruction, entering any call it makes\n",
        "  next, n                one instruction, running any call to completion\n",
        "  finish, f              until the frame it is in returns\n",
        "  continue, c            until a breakpoint, or the end\n",
        "  until, u <addr|name>   until it reaches there\n",
        "  registers, i [all]     every register holding something; all for the rest\n",
        "  read, m <addr> [n]     n bytes of memory, as hex and as text\n",
        "  write, w <addr> <hex>  those bytes, there\n",
        "  stack, k               the call stack, innermost first\n",
        "  call, a <name> [args]  run one function and say what it answered\n",
        "  trace, t [n|off]       record every instruction; then the last n\n",
        "  disassemble, x [n]     n instructions from where it is\n",
        "  source, o              the decompiled C of the function it is in\n",
        "  quit, q                end the session\n",
        "\n",
        "An address is a number, a name from the program, or $register for what\n",
        "that register holds.\n",
        "\n",
        "  astral debug ./crackme --arg ./crackme --arg guess \\\n",
        "    --command 'break strcmp' --command continue --command registers\n",
    ));
    stream.code()
}
