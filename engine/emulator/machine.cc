#include "machine.hh"

#include "debug.hh"

namespace astral_internal {
namespace emulator {

RunResult run(ghidra::Architecture *architecture, const BinaryImage &image,
              const RunOptions &options)
{
    RunResult result;

    // The debugger is the machine. Running a program to its end is the debugger
    // told never to stop, so that is what this is: there is no second emulator
    // here to disagree with the one in debug.cc about what a program does.
    std::string error;
    std::unique_ptr<Debugger> debugger = Debugger::create(architecture, image, options, error);
    if (debugger == nullptr) {
        result.error = error;
        return result;
    }

    debugger->go();
    result = debugger->outcome();
    result.ok = true;
    return result;
}

} // namespace emulator
} // namespace astral_internal
