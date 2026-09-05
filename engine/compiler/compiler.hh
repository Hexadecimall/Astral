// Turning C back into the machine code it stands for.
//
// This is what closes the loop: Astral reads a binary and writes C, a person
// changes the C, and this writes the bytes that change the binary. It compiles
// the C Astral emits rather than the whole language, and it runs nothing else
// to do it: the text goes to the assembler already in the engine.
#ifndef ASTRAL_COMPILER_COMPILER_HH
#define ASTRAL_COMPILER_COMPILER_HH

#include "assembler/assembler.hh"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace astral_internal {
namespace compiler {

// What the program already holds, so compiled code can reach it. A call to a
// function the program defines, or a reference to a string it already carries,
// is a fixed address rather than something to invent.
struct Environment {
    // The address of a name the program defines, or nothing when it has none.
    std::function<std::optional<uint64_t>(const std::string &)> address_of;
    // Where these exact bytes already sit in the image, terminator included,
    // or nothing when they are not there. A literal that is already present
    // costs no room.
    std::function<std::optional<uint64_t>(const std::string &)> address_of_text;
};

struct Diagnostic {
    int line = 0;
    int column = 0;
    std::string message;
};

struct Result {
    bool ok = false;
    // The function's machine code, ready to be written at `address`.
    std::vector<uint8_t> bytes;
    // Data the code needs that the program does not already hold, and where
    // the caller decided to put it. Empty when everything resolved.
    struct Datum {
        std::string text;      // the literal, without its terminator
        uint64_t address = 0;  // where it was placed
    };
    std::vector<Datum> placed;
    // The assembly that was generated, for anyone who wants to see it.
    std::string assembly;
    std::vector<Diagnostic> diagnostics;
    // The first thing that went wrong, said plainly.
    std::string error;
};

// What the caller is willing to give the compiler beyond the function itself.
struct Options {
    // Which function in the source to compile. Empty means the only one.
    std::string function;
    // How many bytes the result has to fit inside. Zero means no limit.
    uint64_t available = 0;
    // Somewhere to put literals the program does not already hold. Returns the
    // address given, or nothing when there is no room.
    std::function<std::optional<uint64_t>(const std::string &)> place_text;
    // Keep the generated assembly in the result even when it compiled.
    bool keep_assembly = false;
    // What the program already holds at `address`, so a recompiled function
    // can be reduced to the bytes that actually differ. Empty means emit the
    // whole function.
    std::vector<uint8_t> existing;
    // Rewrite a literal in place when that is the only thing that changed,
    // rather than regenerating the function around it. Only ever done when
    // the new value fits where the old one sat.
    bool retouch_text_in_place = true;
};

// What one edit costs, once it is known what was actually edited.
//
// Recompiling a whole function to change a string, and rewriting every byte of
// it, is work nobody asked for and a patch far larger than the change. So the
// compiler is told what the code was before it was touched, and it does only
// what the difference demands.
struct Update {
    // A run of bytes to write, and where. Only what differs is here.
    struct Region {
        uint64_t address = 0;
        std::vector<uint8_t> bytes;
        // What made this necessary, for the log: "the literal at line 26" or
        // "verifyPassword was rewritten".
        std::string reason;
    };
    std::vector<Region> regions;
    // Functions that had to be generated again, and functions left alone.
    std::vector<std::string> recompiled;
    std::vector<std::string> untouched;
    // Literals whose bytes changed in place, without touching any code.
    std::vector<std::string> retouched_text;
};

// Compiles only what changed between `before` and `after`.
//
// `before` is the source as Astral emitted it, matching the bytes the program
// holds now; `after` is the same source with the edits in it. Functions whose
// meaning did not change are not compiled at all. A function whose only change
// is the value of a literal is not regenerated either: the literal's own bytes
// are rewritten where they already live, provided the new value fits. Anything
// else is compiled, and even then only the bytes that differ from what the
// program already holds are emitted.
//
// The result's `bytes` is empty; `update` carries the work. `error` is set and
// `ok` is false if any part of it could not be done.
Result compile_update(assembler::Target target, const std::string &before, const std::string &after,
                      uint64_t address, const Environment &environment, Update &update,
                      const Options &options = Options());

// Compiles `source` for `target` as if the function were loaded at `address`.
Result compile(assembler::Target target, const std::string &source, uint64_t address,
               const Environment &environment, const Options &options = Options());

// The same, stopping after the assembly is generated. Useful for seeing what
// the compiler decided without needing the assembler to accept it.
Result compile_to_assembly(assembler::Target target, const std::string &source, uint64_t address,
                           const Environment &environment, const Options &options = Options());

} // namespace compiler
} // namespace astral_internal

#endif
