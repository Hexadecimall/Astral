// Checking the debugger by watching a real program run.
//
// The subject is the plain crackme, built at -O0 so the call the tests step
// over is really made rather than folded into its caller. Every check here
// drives the public interface and then says what actually happened; a stop that
// looks plausible is not evidence, so the addresses are compared with the
// disassembly of the same program.
#include "astral/astral.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void report(bool passed, const std::string &what, const std::string &saw)
{
    ++checks;
    if (passed) {
        std::printf("  ok    %s\n", what.c_str());
        return;
    }
    ++failures;
    std::printf("  FAIL  %s\n        %s\n", what.c_str(), saw.c_str());
}

void expect(bool condition, const std::string &what, const std::string &saw = std::string())
{
    report(condition, what, saw.empty() ? "the condition did not hold" : saw);
}

void expect_equal(unsigned long long got, unsigned long long wanted, const std::string &what)
{
    char saw[128];
    std::snprintf(saw, sizeof saw, "got 0x%llx, wanted 0x%llx", got, wanted);
    report(got == wanted, what, saw);
}

void expect_text(const std::string &got, const std::string &wanted, const std::string &what)
{
    report(got == wanted, what, "got \"" + got + "\", wanted \"" + wanted + "\"");
}

std::string hex(uint64_t value)
{
    char text[32];
    std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

// Where each symbol of the subject sits, looked up once.
struct Subject {
    std::string path;
    uint64_t check = 0;
    uint64_t main = 0;
    uint64_t strcmp_stub = 0;
};

Subject subject;

astral::Program open()
{
    return astral::Program::open(subject.path);
}

// Whether an address lands in a segment the program can run from.
bool executable(const astral::Program &program, uint64_t address)
{
    for (const astral::Segment &segment : program.segments())
        if (segment.executable && address >= segment.address &&
            address < segment.address + segment.size)
            return true;
    return false;
}

// The arguments a run of the crackme is given: its own name and a key.
std::vector<std::string> argv(const std::string &key)
{
    return {subject.path, key};
}

// ------------------------------------------------------------------ tests

void a_breakpoint_stops_where_it_was_put()
{
    std::printf("\na breakpoint stops where it was put\n");
    astral::Program program = open();
    astral::Debugger debugger = program.debug(0, argv("astral"));
    debugger.add_breakpoint(subject.check);
    const astral::Debugger::State state = debugger.go();

    expect_equal(state.address, subject.check, "it stopped at check");
    expect(state.stop == ASTRAL_STOP_BREAKPOINT, "the reason given is a breakpoint", state.reason);
    expect_text(state.function, "check", "it knows which function it is in");
    expect(state.steps > 0, "instructions were counted", std::to_string(state.steps));
    expect(debugger.breakpoints().size() == 1, "the breakpoint is listed");
}

void stepping_follows_the_disassembly()
{
    std::printf("\nstepping follows the disassembly\n");
    astral::Program program = open();
    astral::Debugger debugger = program.debug(0, argv("astral"));
    debugger.add_breakpoint(subject.check);
    debugger.go();

    // The first six instructions of check, as the listing gives them.
    const std::string listing = program.disassemble(subject.check, 6);
    std::vector<uint64_t> wanted;
    for (size_t at = 0; at < listing.size();) {
        const size_t stop = listing.find('\n', at);
        wanted.push_back(std::strtoull(listing.c_str() + at, nullptr, 0));
        if (stop == std::string::npos)
            break;
        at = stop + 1;
    }

    expect(wanted.size() >= 6, "the listing gave instructions to compare against",
           std::to_string(wanted.size()));
    for (size_t i = 1; i < 6 && i < wanted.size(); ++i) {
        const astral::Debugger::State state = debugger.step();
        expect_equal(state.address, wanted[i], "step " + std::to_string(i) + " is at " +
                                                   hex(wanted[i]));
    }
}

void step_over_runs_the_call_out()
{
    std::printf("\nstep_over runs the call out\n");
    astral::Program program = open();
    astral::Debugger debugger = program.debug(0, argv("astral"));
    // main's call to check. Stopping on check and walking the stack says where
    // the call was made from, so the test does not hard-code it.
    debugger.add_breakpoint(subject.check);
    debugger.go();
    // Past check's prologue, so its own frame pointer is in place and the
    // chain reaches the caller.
    for (int i = 0; i < 3; ++i)
        debugger.step();
    const std::vector<astral::Debugger::Frame> frames = debugger.stack();
    expect(frames.size() >= 2, "there is a caller to come back to",
           std::to_string(frames.size()));
    if (frames.size() < 2)
        return;
    const uint64_t after_the_call = frames[1].address;
    expect_text(frames[1].function, "main", "the caller is main");

    // Start again and stop at the instruction that makes the call.
    astral::Debugger second = program.debug(0, argv("astral"));
    second.clear_breakpoints();
    second.run_to(after_the_call - 4);
    expect_equal(second.state().address, after_the_call - 4, "it is on the call instruction");

    const astral::Debugger::State state = second.step_over();
    expect_equal(state.address, after_the_call, "step_over lands after the call");
    // check answers 1 for the right key, and that answer is in the result
    // register: the call really ran rather than being skipped.
    expect_equal(second.register_value("x0") & 0xffffffffull, 1, "the call's answer is there");
}

void step_out_returns_to_the_caller()
{
    std::printf("\nstep_out returns to the caller\n");
    astral::Program program = open();
    astral::Debugger debugger = program.debug(0, argv("astral"));
    debugger.add_breakpoint(subject.check);
    debugger.go();
    debugger.clear_breakpoints();
    // Into the body, so the frame being left is check's own.
    debugger.step();
    debugger.step();

    const astral::Debugger::State state = debugger.step_out();
    expect(state.stop == ASTRAL_STOP_RETURNED, "the reason given is a return", state.reason);
    expect_text(state.function, "main", "it came back to main");
}

void registers_read_back_what_was_set()
{
    std::printf("\nregisters read back what was set\n");
    astral::Program program = open();
    astral::Debugger debugger = program.debug(0, argv("astral"));
    debugger.add_breakpoint(subject.check);
    debugger.go();

    expect(debugger.registers().size() > 8, "the architecture named its registers",
           std::to_string(debugger.registers().size()));
    debugger.set_register("x9", 0x1234567890abcdefull);
    expect_equal(debugger.register_value("x9"), 0x1234567890abcdefull, "x9 holds what was set");
    // The stack pointer is where the machine put it, not somewhere arbitrary.
    expect(debugger.register_value("sp") != 0, "the stack pointer has a value");
}

void memory_reads_back_what_was_written()
{
    std::printf("\nmemory reads back what was written\n");
    astral::Program program = open();
    astral::Debugger debugger = program.debug(0, argv("astral"));
    debugger.add_breakpoint(subject.check);
    debugger.go();

    const uint64_t where = debugger.register_value("sp");
    const std::vector<uint8_t> bytes = {0xde, 0xad, 0xbe, 0xef};
    debugger.write(where, bytes);
    const std::vector<uint8_t> back = debugger.read(where, 4);
    expect(back == bytes, "the four bytes came back",
           back.size() == 4 ? hex(back[0]) + " " + hex(back[1]) + " " + hex(back[2]) + " " +
                                  hex(back[3])
                            : "only " + std::to_string(back.size()) + " bytes");
}

void a_watchpoint_fires_on_the_write_that_hits_it()
{
    std::printf("\na watchpoint fires on the write that hits it\n");
    astral::Program program = open();
    astral::Debugger debugger = program.debug(0, argv("astral"));
    debugger.add_breakpoint(subject.check);
    debugger.go();
    debugger.clear_breakpoints();

    // check's prologue saves the frame pointer pair and then stores its
    // argument below it; watching that slot catches the store.
    // The frame check is about to build, which its prologue writes into.
    const uint64_t stack = debugger.register_value("sp");
    debugger.add_watchpoint(stack - 0x40, 0x40);
    const astral::Debugger::State state = debugger.go();
    expect(state.stop == ASTRAL_STOP_WATCHPOINT, "a watchpoint stopped it", state.reason);
    expect(state.address != 0, "it stopped somewhere real", hex(state.address));
}

void the_stack_names_the_function_it_is_in()
{
    std::printf("\nthe stack names the function it is in\n");
    astral::Program program = open();
    astral::Debugger debugger = program.debug(0, argv("astral"));
    debugger.add_breakpoint(subject.check);
    debugger.go();
    // Past the prologue, so the frame pointer is check's own.
    debugger.clear_breakpoints();
    for (int i = 0; i < 3; ++i)
        debugger.step();

    const std::vector<astral::Debugger::Frame> frames = debugger.stack();
    expect(frames.size() >= 2, "two frames were recovered", std::to_string(frames.size()));
    if (frames.empty())
        return;
    expect_text(frames[0].function, "check", "the innermost frame is check");
    if (frames.size() > 1)
        expect_text(frames[1].function, "main", "the one under it is main");
}

void a_function_can_be_called_on_its_own()
{
    std::printf("\na function can be called on its own\n");
    astral::Program program = open();
    astral::Debugger debugger = program.debug(0, argv("astral"));
    debugger.start();

    const astral::Debugger::CallResult right = debugger.call(subject.check, {"astral"});
    expect_equal(right.result, 1, "check(\"astral\") answers 1");
    const astral::Debugger::CallResult wrong = debugger.call(subject.check, {"wrong"});
    expect_equal(wrong.result, 0, "check(\"wrong\") answers 0");

    // The call left nothing behind: the debugger is still where it was.
    expect_equal(debugger.state().address, program.entry_points().front(),
                 "the debugger is where it was before the call");
}

void a_snapshot_winds_a_run_back()
{
    std::printf("\na snapshot winds a run back\n");
    astral::Program program = open();
    astral::Debugger debugger = program.debug(0, argv("astral"));
    debugger.add_breakpoint(subject.check);
    debugger.go();

    const std::vector<uint8_t> saved = debugger.snapshot();
    expect(!saved.empty(), "the snapshot has bytes", std::to_string(saved.size()));
    const uint64_t address = debugger.state().address;
    const uint64_t argument = debugger.register_value("x0");

    debugger.clear_breakpoints();
    const astral::Debugger::State ran = debugger.go();
    expect(ran.stop == ASTRAL_STOP_FINISHED, "it ran to the end", ran.reason);

    debugger.restore(saved);
    expect_equal(debugger.state().address, address, "it is back at check");
    expect_equal(debugger.register_value("x0"), argument, "the argument register came back");

    // And the same run can be made again from there.
    const astral::Debugger::State again = debugger.go();
    expect(again.stop == ASTRAL_STOP_FINISHED, "the run repeats", again.reason);
    expect_text(again.output.substr(0, 7), "correct", "with the same answer");
}

void cancel_stops_a_run_from_another_thread()
{
    std::printf("\ncancel stops a run from another thread\n");
    astral::Program program = open();
    // A branch to itself where check begins, so the run that follows will not
    // end on its own. This is the only way to know cancel did anything: a
    // program that stops by itself proves nothing. Instructions are decoded
    // from the image rather than from the emulator's memory, so the loop is put
    // there rather than written through the debugger.
    program.patch_bytes(subject.check, std::vector<uint8_t>{0x00, 0x00, 0x00, 0x14},
                        "a branch to itself");
    astral::Debugger debugger = program.debug(0, argv("astral"), std::string(), 2000000000ull);

    std::atomic<bool> running{true};
    std::thread stopper([&debugger, &running] {
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            debugger.cancel();
        }
    });
    const astral::Debugger::State state = debugger.go();
    running.store(false);
    stopper.join();

    expect(state.stop == ASTRAL_STOP_CANCELLED, "the run was cancelled", state.reason);
    expect_equal(state.address, subject.check, "it stopped on the loop it was in");
    expect(state.steps > 1000, "it really was running", std::to_string(state.steps));
}

// The point of the whole thing: the key comes out of the running program
// without the source, and without reading the constant out of the file.
void the_key_falls_out_of_the_comparison()
{
    std::printf("\nthe key falls out of the comparison\n");
    astral::Program program = open();
    astral::Debugger debugger = program.debug(0, argv("not-the-key"));
    debugger.add_breakpoint(subject.strcmp_stub);
    const astral::Debugger::State state = debugger.go();
    expect(state.stop == ASTRAL_STOP_BREAKPOINT, "it stopped on strcmp", state.reason);

    const std::string given = debugger.read_text(debugger.register_value("x0"));
    const std::string against = debugger.read_text(debugger.register_value("x1"));
    std::printf("        strcmp(\"%s\", \"%s\")\n", given.c_str(), against.c_str());
    expect_text(given, "not-the-key", "the first argument is what was handed in");
    expect_text(against, "astral", "the second is the key the program wants");

    // And the recovered key works, which is the whole claim.
    const astral::Debugger::CallResult answer = debugger.call(subject.check, {against});
    expect_equal(answer.result, 1, "the recovered key is accepted");
}

// `run` is the debugger told never to stop, so the two have to agree about what
// a program did. If they ever disagree, one of them is a second emulator.
void running_agrees_with_debugging()
{
    std::printf("\nrunning and debugging say the same thing\n");

    astral::Program program = open();
    astral::Debugger debugger = program.debug(0, argv("astral"));
    const astral::Debugger::State watched = debugger.go();

    // The library's own one-call form, over the same program and arguments.
    // Opened through the C interface because that is the only way in: there is
    // no `run` command any more, so nothing else exercises this path.
    astral_program *raw = astral_program_open(subject.path.c_str(), nullptr);
    expect(raw != nullptr, "the program opens through the C interface",
           astral_last_error() == nullptr ? "" : astral_last_error());
    if (raw == nullptr)
        return;
    const char *given[] = {subject.path.c_str(), "astral", nullptr};
    char *text = astral_program_run(raw, 0, given, nullptr, 0);
    expect(text != nullptr, "the program ran",
           astral_last_error() == nullptr ? "" : astral_last_error());
    if (text == nullptr) {
        astral_program_close(raw);
        return;
    }
    const std::string reported = text;
    astral_string_free(text);
    astral_program_close(raw);

    expect(reported.find(watched.output) != std::string::npos,
           "what it printed is what the debugger saw it print", reported);
    expect(reported.find("returned with 0") != std::string::npos,
           "it ended the way the debugger says it ended", reported);
    for (const std::string &call : watched.calls)
        expect(reported.find(call) != std::string::npos,
               "the call to " + call + " is reported by both", reported);
}

// A trace has to be the instructions that ran, not the addresses they ran at:
// half hex and half C is not something anyone can read.
void a_trace_says_what_ran()
{
    std::printf("\na trace says what ran\n");

    astral::Program program = open();
    astral::Debugger quiet = program.debug(0, argv("astral"));
    quiet.go();
    expect(quiet.trace().empty(), "nothing is recorded unless it is asked for",
           std::to_string(quiet.trace().size()) + " lines");

    astral::Debugger debugger = program.debug(0, argv("astral"));
    debugger.set_trace(true);
    debugger.go();
    const std::vector<std::string> lines = debugger.trace();
    expect(!lines.empty(), "asking for a trace records one");
    if (lines.empty())
        return;

    // Every line is an address and an instruction, not an address alone.
    bool all_have_text = true;
    std::string offender;
    for (const std::string &line : lines) {
        const size_t colon = line.find(": ");
        if (colon == std::string::npos || line.find_first_not_of(" ", colon + 2) ==
                                              std::string::npos) {
            all_have_text = false;
            offender = line;
            break;
        }
    }
    expect(all_have_text, "every line carries the instruction, not just where it was", offender);
    std::printf("        %zu instructions, first: %s\n", lines.size(), lines.front().c_str());

    // And it is the program's own first instruction that starts it.
    expect(lines.front().find(hex(subject.main).substr(2)) != std::string::npos,
           "it starts where the program starts", lines.front());
}

} // namespace

int main(int argc, char **argv_in)
{
    if (argc < 2) {
        std::printf("no program was named, so there is nothing to debug\n");
        return 2;
    }
    subject.path = argv_in[1];

    astral::Library library;
    {
        astral::Program program = astral::Program::open(subject.path);
        for (const astral::Symbol &symbol : program.symbols()) {
            if (symbol.name == "check" && symbol.is_function)
                subject.check = symbol.address;
            if (symbol.name == "main" && symbol.is_function)
                subject.main = symbol.address;
            // The stub in the text of the program, not the slot in its data:
            // the one the program branches to is the one that can be broken on.
            if (symbol.name == "strcmp" && executable(program, symbol.address))
                subject.strcmp_stub = symbol.address;
        }
    }
    if (subject.check == 0 || subject.main == 0) {
        std::printf("the subject has no symbols to debug against\n");
        return 2;
    }

    a_breakpoint_stops_where_it_was_put();
    stepping_follows_the_disassembly();
    step_over_runs_the_call_out();
    step_out_returns_to_the_caller();
    registers_read_back_what_was_set();
    memory_reads_back_what_was_written();
    a_watchpoint_fires_on_the_write_that_hits_it();
    the_stack_names_the_function_it_is_in();
    a_function_can_be_called_on_its_own();
    a_snapshot_winds_a_run_back();
    cancel_stops_a_run_from_another_thread();
    the_key_falls_out_of_the_comparison();
    running_agrees_with_debugging();
    a_trace_says_what_ran();

    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
