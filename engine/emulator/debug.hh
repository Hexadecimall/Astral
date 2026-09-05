// Watching a program run, a step at a time.
//
// `run` in machine.hh executes a program and reports what happened. This holds
// the same machine still: it stops where it is told, and while it is stopped
// everything it holds can be read and changed. That is the difference between
// being told what a function did and watching it do it.
//
// Nothing here touches the operating system. The process being debugged is not
// a process: it is Astral's own memory and registers, stepped as p-code. So a
// program for another architecture debugs the same as a native one, a program
// that would refuse to run under a real debugger cannot tell, and nothing that
// happens can escape into the machine it is being watched on.
#ifndef ASTRAL_DEBUG_HH
#define ASTRAL_DEBUG_HH

#include "emulator/machine.hh"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ghidra {
class Architecture;
}

namespace astral_internal {

struct BinaryImage;

namespace emulator {

class Debugger {
public:
    // Builds a run that has not started. Returns null and fills `error` when
    // the program cannot be set up.
    static std::unique_ptr<Debugger> create(ghidra::Architecture *architecture,
                                            const BinaryImage &image, const RunOptions &options,
                                            std::string &error);
    ~Debugger();

    Debugger(const Debugger &) = delete;
    Debugger &operator=(const Debugger &) = delete;

    // Why it is not running just now.
    enum class Stop {
        NotStarted,
        Stepped,      // the step asked for is done
        Breakpoint,   // it reached one that was set
        Watchpoint,   // memory being watched changed
        Returned,     // the frame being watched returned
        Finished,     // the program ran to its end
        StepLimit,    // it was still going when the budget ran out
        Fault,        // it did something it could not do
        Cancelled,    // something asked it to stop
    };

    struct State {
        Stop stop = Stop::NotStarted;
        // What happened, said the way it would be shown to a person.
        std::string reason;
        // Where it is now.
        uint64_t address = 0;
        // The name of whatever contains that address, when it has one.
        std::string function;
        uint64_t steps = 0;
        // False once it has finished or faulted: nothing more will happen.
        bool live = true;
        // What it wrote and which library calls it made, since the last stop.
        std::string output;
        std::vector<std::string> calls;
    };

    // ------------------------------------------------------------ control

    // Puts it at the first instruction with nothing executed yet.
    State start();
    // One instruction, entering any call it makes.
    State step();
    // One instruction, running any call it makes to completion.
    State step_over();
    // Until the current frame returns.
    State step_out();
    // Until it reaches `address`, a breakpoint, or the end.
    State run_to(uint64_t address);
    // Until a breakpoint, or the end.
    State go();
    // Asks a run in progress to stop. Safe to call from another thread; the
    // run stops at the next instruction and reports Cancelled.
    void cancel();

    // Whether to keep a line for every instruction executed from here on. Off
    // unless asked for: a line per instruction is millions of them on anything
    // the size of a real program.
    void set_trace(bool on);

    State state() const;

    // Everything that has happened since it started: what it wrote, the library
    // calls it made, the trace when one was asked for, and why it stopped. A
    // `State` says what changed at the last stop; this says the whole run.
    const RunResult &outcome() const;

    // --------------------------------------------------------- breakpoints

    void add_breakpoint(uint64_t address);
    void remove_breakpoint(uint64_t address);
    void clear_breakpoints();
    std::vector<uint64_t> breakpoints() const;

    // Stops when any byte in the range is written.
    void add_watchpoint(uint64_t address, uint64_t size);
    void remove_watchpoint(uint64_t address);
    void clear_watchpoints();

    // ------------------------------------------------------------- state

    struct Register {
        std::string name;
        uint64_t value = 0;
        // How many bytes of `value` mean anything.
        int width = 0;
    };
    // The architecture's registers, in the order it names them.
    std::vector<Register> registers() const;
    bool set_register(const std::string &name, uint64_t value, std::string &error);

    // Reads what the program can see. A short result means the range leaves
    // the memory it has.
    std::vector<uint8_t> read(uint64_t address, uint64_t size) const;
    bool write(uint64_t address, const std::vector<uint8_t> &bytes, std::string &error);

    struct Frame {
        uint64_t address = 0;        // where this frame is executing
        uint64_t frame_pointer = 0;
        std::string function;        // the name of what it is in, when known
    };
    // Innermost first. Best effort: a frame pointer that has been reused for
    // something else ends the walk rather than inventing frames.
    std::vector<Frame> stack() const;

    // ------------------------------------------------------------ calling

    struct CallResult {
        bool ok = false;
        uint64_t result = 0;
        std::string output;
        std::string error;
        uint64_t steps = 0;
    };
    // Runs one function with these arguments and hands back what it answered,
    // leaving the debugger where it was. This is what makes a single recovered
    // function testable without running the program around it.
    //
    // A `std::string` argument is written into memory the call can reach and
    // passed as a pointer to it; an integer is passed as itself.
    struct Argument {
        bool is_text = false;
        uint64_t value = 0;
        std::string text;
    };
    CallResult call(uint64_t address, const std::vector<Argument> &arguments,
                    uint64_t step_limit = 0);

    // ---------------------------------------------------------- snapshots

    // Everything the machine holds, so a run can be wound back and tried
    // again with something changed. The bytes are opaque.
    std::vector<uint8_t> snapshot() const;
    bool restore(const std::vector<uint8_t> &snapshot, std::string &error);

private:
    Debugger();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace emulator
} // namespace astral_internal

#endif
