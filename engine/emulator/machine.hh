// Running a program without running it.
//
// Astral reads a binary statically; this executes one, on its own terms. The
// instructions are stepped as p-code, which is what the decompiler already
// lifts them to, so the same machine runs an arm64 program on an x86 host or a
// Windows program on a Mac. Nothing is handed to the operating system: the
// memory is Astral's, the registers are Astral's, and a call into the C library
// is answered here rather than by the real one.
//
// That is the point of doing it this way. A binary you would not want to run,
// or cannot run, still tells you what it does when you can watch it do it.
#ifndef ASTRAL_MACHINE_HH
#define ASTRAL_MACHINE_HH

#include <cstdint>
#include <string>
#include <vector>

namespace ghidra {
class Architecture;
}

namespace astral_internal {

struct BinaryImage;

namespace emulator {

struct RunOptions {
    // Where to start. Zero means the program's own entry point.
    uint64_t entry = 0;
    // What to hand it, argv[0] included.
    std::vector<std::string> arguments;
    // What it reads from its input.
    std::string input;
    // A run that will not end is stopped rather than left going.
    uint64_t step_limit = 2000000;
    // Record every instruction executed. Costly, and off unless asked for.
    bool trace = false;
};

struct RunResult {
    bool ok = false;
    std::string error;
    // Why it stopped: "returned", "step limit", "no instruction", and so on.
    std::string stopped_because;
    uint64_t steps = 0;
    uint64_t stopped_at = 0;
    bool returned = false;
    uint64_t result = 0;          // what the function handed back
    std::string output;           // what it wrote
    std::vector<std::string> calls;  // the library calls it made, in order
    std::vector<std::string> trace;  // one line per instruction, when asked for
};

// Runs the program. `image` says where its bytes and its imported names are;
// the architecture says how to read them.
RunResult run(ghidra::Architecture *architecture, const BinaryImage &image,
              const RunOptions &options);

} // namespace emulator
} // namespace astral_internal

#endif
