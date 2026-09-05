#include "debug.hh"

#include "machine_internal.hh"

#include "image.hh"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace astral_internal {
namespace emulator {
namespace {

// Pages are the unit a snapshot keeps, because that is the unit the overlay
// writes: anything finer would store the same page many times over.
const uint64_t kPage = 4096;
// Enough of a chain to be useful without walking a corrupted one forever.
const int kMaxFrames = 64;
// A call made by hand gets its own budget when none was named, so a function
// that does not return cannot hang the caller that asked about it.
const uint64_t kCallSteps = 1000000;

const char kSnapshotMagic[8] = {'A', 's', 't', 'r', 'a', 'l', 'D', '1'};

void put(std::vector<uint8_t> &out, const void *bytes, size_t size)
{
    const uint8_t *from = static_cast<const uint8_t *>(bytes);
    out.insert(out.end(), from, from + size);
}

void put_number(std::vector<uint8_t> &out, uint64_t value)
{
    put(out, &value, sizeof value);
}

void put_text(std::vector<uint8_t> &out, const std::string &text)
{
    put_number(out, text.size());
    put(out, text.data(), text.size());
}

// Reads a snapshot back. Every read is bounds-checked against what is left,
// because the bytes are handed in from outside and a truncated one must be
// refused rather than trusted.
class Reader {
public:
    Reader(const uint8_t *at, size_t size) : at_(at), left_(size) {}

    bool take(void *out, size_t size)
    {
        if (size > left_)
            return false;
        std::memcpy(out, at_, size);
        at_ += size;
        left_ -= size;
        return true;
    }

    bool number(uint64_t &out) { return take(&out, sizeof out); }

    bool text(std::string &out)
    {
        uint64_t size = 0;
        if (!number(size) || size > left_)
            return false;
        out.assign(reinterpret_cast<const char *>(at_), static_cast<size_t>(size));
        at_ += size;
        left_ -= static_cast<size_t>(size);
        return true;
    }

private:
    const uint8_t *at_;
    size_t left_;
};

} // namespace

// ---------------------------------------------------------------------------

struct Debugger::Impl : public WriteWatcher {
    ghidra::Architecture *arch = nullptr;
    const BinaryImage *image = nullptr;
    ghidra::AddrSpace *ram = nullptr;
    RunOptions options;
    bool arm = false;
    uint64_t entry = 0;

    // Rebuilt by reset(), so starting again is the same as never having run.
    std::unique_ptr<Memory> memory;
    RunResult sink;
    std::unique_ptr<Library> library;
    std::unique_ptr<Machine> machine;

    std::map<uint64_t, std::string> imports;
    // Function symbols by address, sorted, so the name containing an address is
    // one search rather than a scan.
    std::vector<std::pair<uint64_t, Symbol>> functions;

    std::set<uint64_t> breakpoints;
    struct Watch {
        uint64_t address = 0;
        uint64_t size = 0;
    };
    std::vector<Watch> watchpoints;
    bool watch_hit = false;
    uint64_t watch_at = 0;

    // Which pages the program has written. A snapshot keeps these and nothing
    // else: the rest of the address space is still the file on disk.
    std::set<uint64_t> dirty;

    std::atomic<bool> stop_asked{false};

    State current;
    uint64_t steps = 0;
    // How much of the output and how many of the calls have been reported, so
    // each stop says what happened since the last one rather than everything.
    size_t output_seen = 0;
    size_t calls_seen = 0;

    // ------------------------------------------------------------- writes

    void wrote(uint64_t address, uint64_t size) override
    {
        for (uint64_t page = address / kPage; page <= (address + size - 1) / kPage; ++page)
            dirty.insert(page);
        for (const Watch &watch : watchpoints) {
            if (address < watch.address + watch.size && watch.address < address + size) {
                watch_hit = true;
                watch_at = watch.address;
            }
        }
    }

    // -------------------------------------------------------------- setup

    // Builds the machine from nothing: fresh memory, a fresh library, the
    // registers a program expects to start with.
    bool reset(std::string &error)
    {
        memory = std::make_unique<Memory>(arch);
        sink = RunResult();
        library = std::make_unique<Library>(&memory->state(), ram, sink, options.input);
        library->set_watcher(this);
        machine = std::make_unique<Machine>(arch, &memory->state());
        machine->set_watcher(this);
        dirty.clear();
        steps = 0;
        output_seen = 0;
        calls_seen = 0;
        watch_hit = false;
        stop_asked.store(false, std::memory_order_relaxed);

        // The arguments, laid out where a program expects to find them.
        std::vector<uint64_t> argv;
        for (const std::string &one : options.arguments) {
            const uint64_t where = library->allocate(one.size() + 1);
            library->write_text(where, one);
            argv.push_back(where);
        }
        const uint64_t table = library->allocate((argv.size() + 1) * 8);
        for (size_t i = 0; i < argv.size(); ++i)
            write_word(table + i * 8, 8, argv[i]);
        write_word(table + argv.size() * 8, 8, 0);

        if (arm) {
            set_register_value("sp", kStackTop);
            set_register_value("x30", kFinished); // where a return lands
            set_register_value("x0", options.arguments.size());
            set_register_value("x1", table);
        } else {
            set_register_value("RSP", kStackTop - 8);
            // A return reads the address off the stack, so it is put there.
            write_word(kStackTop - 8, 8, kFinished);
            set_register_value("RDI", options.arguments.size());
            set_register_value("RSI", table);
        }

        machine->set_depth(0);
        machine->setExecuteAddress(ghidra::Address(ram, entry));
        current = State();
        current.stop = Stop::NotStarted;
        current.reason = "nothing has run yet";
        current.live = true;
        refresh();
        error.clear();
        return true;
    }

    // ------------------------------------------------------- small helpers

    void write_word(uint64_t address, int size, uint64_t value)
    {
        memory->state().setValue(ram, address, size, value);
        wrote(address, static_cast<uint64_t>(size));
    }

    void set_register_value(const char *name, uint64_t value)
    {
        try {
            memory->state().setValue(name, value);
        } catch (ghidra::LowlevelError &) {
        }
    }

    uint64_t register_value(const char *name) const
    {
        try {
            return memory->state().getValue(name);
        } catch (ghidra::LowlevelError &) {
            return 0;
        }
    }

    uint64_t where() const { return machine->getExecuteAddress().getOffset(); }

    // One line of the trace: what decodes at an address, or the words given
    // when the thing that happens there is not an instruction.
    void note_trace(uint64_t at, const std::string &instead)
    {
        std::string line;
        if (instead.empty())
            line = assembly_line(arch, at);
        if (line.empty()) {
            std::ostringstream out;
            ghidra::Address(ram, at).printRaw(out);
            out << ": " << (instead.empty() ? std::string("?") : instead);
            line = out.str();
        }
        sink.trace.push_back(line);
    }

    // The name of whatever contains an address. An import is named by the stub
    // that stands for it, which is what the program actually branched to.
    std::string name_for(uint64_t address) const
    {
        const auto import = imports.find(address);
        if (import != imports.end())
            return import->second;
        auto after = std::upper_bound(functions.begin(), functions.end(), address,
                                      [](uint64_t value, const std::pair<uint64_t, Symbol> &entry) {
                                          return value < entry.first;
                                      });
        if (after == functions.begin())
            return std::string();
        --after;
        const Symbol &symbol = after->second;
        if (symbol.size != 0 && address >= symbol.address + symbol.size)
            return std::string();
        return symbol.name;
    }

    // Registers worth showing: the widest one at each place the architecture
    // names, so x0 is listed rather than x0, w0 and every other view of it.
    std::vector<Debugger::Register> register_list() const
    {
        std::vector<Debugger::Register> out;
        std::map<ghidra::VarnodeData, std::string> all;
        try {
            arch->translate->getAllRegisters(all);
        } catch (ghidra::LowlevelError &) {
            return out;
        }
        // Keyed by where it lives, so the views of one register collapse to it.
        std::map<std::pair<ghidra::AddrSpace *, uint64_t>, std::pair<int, std::string>> widest;
        for (const auto &entry : all) {
            if (entry.first.size == 0 || entry.first.size > 8)
                continue;
            const auto place = std::make_pair(entry.first.space, entry.first.offset);
            auto found = widest.find(place);
            if (found == widest.end() || found->second.first < static_cast<int>(entry.first.size))
                widest[place] = {static_cast<int>(entry.first.size), entry.second};
        }
        for (const auto &entry : widest) {
            Debugger::Register one;
            one.name = entry.second.second;
            one.width = entry.second.first;
            one.value = 0;
            // The machine keeps the program counter of its own rather than in
            // the register file, so that one is answered from where it is.
            if (one.name == "pc" || one.name == "RIP") {
                one.value = where();
                out.push_back(one);
                continue;
            }
            try {
                one.value = memory->state().getValue(entry.first.first, entry.first.second,
                                                     entry.second.first);
            } catch (ghidra::LowlevelError &) {
            }
            out.push_back(one);
        }
        return out;
    }

    void refresh()
    {
        current.address = where();
        current.function = name_for(current.address);
        current.steps = steps;
        if (sink.output.size() > output_seen) {
            current.output = sink.output.substr(output_seen);
            output_seen = sink.output.size();
        } else {
            current.output.clear();
        }
        current.calls.assign(sink.calls.begin() + static_cast<long>(calls_seen), sink.calls.end());
        calls_seen = sink.calls.size();
    }

    void settle(Stop stop, const std::string &reason, bool live = true)
    {
        current.stop = stop;
        current.reason = reason;
        current.live = live;
        refresh();
        // The same stop, said the way a report wants it rather than the way a
        // person watching wants it. Both are decided here so the two can never
        // disagree about why a program stopped.
        sink.steps = steps;
        sink.stopped_at = current.address;
        sink.stopped_because =
            stop == Stop::Finished ? (library->exited() ? "the program exited" : "returned")
                                   : reason;
    }

    // ------------------------------------------------------------ stepping

    enum class Advance { Ok, Ended };

    // One machine instruction, whatever number of p-code operations that is.
    // Stepping in p-code would stop in the middle of an instruction, which is a
    // place the program never is.
    Advance advance()
    {
        const uint64_t at = where();

        if (at == kFinished) {
            sink.returned = true;
            sink.result = arm ? register_value("x0") : register_value("RAX");
            settle(Stop::Finished, "it returned " + std::to_string(static_cast<long long>(
                                       static_cast<int32_t>(sink.result))),
                   false);
            return Advance::Ended;
        }

        const auto import = imports.find(at);
        if (import != imports.end()) {
            if (options.trace)
                note_trace(at, "call " + import->second);
            std::vector<uint64_t> arguments;
            static const char *arm_order[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
            static const char *x86_order[] = {"RDI", "RSI", "RDX", "RCX", "R8", "R9"};
            const size_t count = arm ? 8 : 6;
            for (size_t i = 0; i < count; ++i)
                arguments.push_back(register_value(arm ? arm_order[i] : x86_order[i]));
            library->set_variadic_base(arm ? register_value("sp") : register_value("RSP") + 8);
            uint64_t answer = 0;
            if (!library->call(import->second, arguments, answer)) {
                settle(Stop::Fault,
                       "a call to " + import->second +
                           ", which Astral does not know how to answer",
                       false);
                return Advance::Ended;
            }
            sink.calls.push_back(import->second);
            set_register_value(arm ? "x0" : "RAX", answer);
            if (library->exited()) {
                sink.returned = true;
                sink.result = answer;
                settle(Stop::Finished, "the program exited with " +
                                           std::to_string(static_cast<long long>(
                                               static_cast<int32_t>(answer))),
                       false);
                return Advance::Ended;
            }
            // Back to whoever called, the way a return would go.
            uint64_t back = 0;
            if (arm) {
                back = register_value("x30");
            } else {
                const uint64_t stack = register_value("RSP");
                back = memory->state().getValue(ram, stack, 8);
                set_register_value("RSP", stack + 8);
            }
            machine->note_return();
            machine->setExecuteAddress(ghidra::Address(ram, back));
            ++steps;
            return Advance::Ok;
        }

        if (!machine->decoded()) {
            settle(Stop::Fault, "there is no instruction to run there", false);
            return Advance::Ended;
        }

        if (options.trace)
            note_trace(at, std::string());

        watch_hit = false;
        try {
            do {
                machine->executeCurrentOp();
            } while (!machine->at_instruction_start());
        } catch (ghidra::LowlevelError &error) {
            settle(Stop::Fault, error.explain, false);
            return Advance::Ended;
        }
        ++steps;
        return Advance::Ok;
    }

    // Everything that runs more than one instruction shares this. `keep_going`
    // is asked after each one and says whether there is any reason to stop
    // beyond the ones every run has.
    template <typename Test>
    void drive(const Test &keep_going, Stop reached, const std::string &reached_reason)
    {
        stop_asked.store(false, std::memory_order_relaxed);
        uint64_t budget = options.step_limit == 0 ? kCallSteps : options.step_limit;
        while (true) {
            if (stop_asked.load(std::memory_order_relaxed)) {
                settle(Stop::Cancelled, "something asked it to stop");
                return;
            }
            if (advance() == Advance::Ended)
                return;
            if (watch_hit) {
                char line[64];
                std::snprintf(line, sizeof line, "0x%llx",
                              static_cast<unsigned long long>(watch_at));
                settle(Stop::Watchpoint, std::string("memory at ") + line + " was written");
                return;
            }
            if (breakpoints.count(where()) != 0) {
                settle(Stop::Breakpoint, "it reached a breakpoint");
                return;
            }
            if (!keep_going()) {
                settle(reached, reached_reason);
                return;
            }
            if (budget != 0 && --budget == 0) {
                settle(Stop::StepLimit, "the step limit was reached");
                return;
            }
        }
    }
};

// ---------------------------------------------------------------------------

Debugger::Debugger() : impl_(new Impl()) {}
Debugger::~Debugger() = default;

std::unique_ptr<Debugger> Debugger::create(ghidra::Architecture *architecture,
                                           const BinaryImage &image, const RunOptions &options,
                                           std::string &error)
{
    if (architecture == nullptr || architecture->translate == nullptr) {
        error = "there is no architecture to run this on";
        return nullptr;
    }
    std::unique_ptr<Debugger> debugger(new Debugger());
    Impl &impl = *debugger->impl_;
    impl.arch = architecture;
    impl.image = &image;
    impl.ram = architecture->getDefaultCodeSpace();
    impl.options = options;
    impl.arm = is_arm(architecture);

    impl.entry = options.entry;
    if (impl.entry == 0 && !image.entry_points.empty())
        impl.entry = image.entry_points.front();
    if (impl.entry == 0) {
        error = "there is nowhere to start: give a function to run";
        return nullptr;
    }

    for (const Symbol &symbol : image.symbols) {
        if (symbol.name.empty())
            continue;
        if (symbol.is_import) {
            impl.imports.emplace(symbol.address, symbol.name);
        } else if (symbol.is_function) {
            impl.functions.emplace_back(symbol.address, symbol);
        }
    }
    std::sort(impl.functions.begin(), impl.functions.end(),
              [](const std::pair<uint64_t, Symbol> &a, const std::pair<uint64_t, Symbol> &b) {
                  return a.first < b.first;
              });

    if (!impl.reset(error))
        return nullptr;
    return debugger;
}

// ------------------------------------------------------------------ control

Debugger::State Debugger::start()
{
    std::string error;
    impl_->reset(error);
    return impl_->current;
}

Debugger::State Debugger::step()
{
    Impl &impl = *impl_;
    if (!impl.current.live)
        return impl.current;
    impl.stop_asked.store(false, std::memory_order_relaxed);
    if (impl.advance() == Impl::Advance::Ended)
        return impl.current;
    if (impl.watch_hit) {
        char line[64];
        std::snprintf(line, sizeof line, "0x%llx",
                      static_cast<unsigned long long>(impl.watch_at));
        impl.settle(Stop::Watchpoint, std::string("memory at ") + line + " was written");
        return impl.current;
    }
    impl.settle(Stop::Stepped, "one instruction");
    return impl.current;
}

Debugger::State Debugger::step_over()
{
    Impl &impl = *impl_;
    if (!impl.current.live)
        return impl.current;
    const int64_t depth = impl.machine->depth();
    impl.stop_asked.store(false, std::memory_order_relaxed);
    if (impl.advance() == Impl::Advance::Ended)
        return impl.current;
    // Only a call needs running out; anything else has already finished.
    if (impl.machine->depth() <= depth) {
        if (impl.watch_hit) {
            impl.settle(Stop::Watchpoint, "memory being watched was written");
            return impl.current;
        }
        impl.settle(Stop::Stepped, "one instruction");
        return impl.current;
    }
    Machine *machine = impl.machine.get();
    impl.drive([machine, depth] { return machine->depth() > depth; }, Stop::Stepped,
               "one instruction, over the call it made");
    return impl.current;
}

Debugger::State Debugger::step_out()
{
    Impl &impl = *impl_;
    if (!impl.current.live)
        return impl.current;
    const int64_t depth = impl.machine->depth();
    Machine *machine = impl.machine.get();
    impl.drive([machine, depth] { return machine->depth() >= depth; }, Stop::Returned,
               "the frame it was in returned");
    return impl.current;
}

Debugger::State Debugger::run_to(uint64_t address)
{
    Impl &impl = *impl_;
    if (!impl.current.live)
        return impl.current;
    Impl *at = &impl;
    impl.drive([at, address] { return at->where() != address; }, Stop::Breakpoint,
               "it reached the address asked for");
    return impl.current;
}

Debugger::State Debugger::go()
{
    Impl &impl = *impl_;
    if (!impl.current.live)
        return impl.current;
    impl.drive([] { return true; }, Stop::Stepped, std::string());
    return impl.current;
}

void Debugger::cancel() { impl_->stop_asked.store(true, std::memory_order_relaxed); }

void Debugger::set_trace(bool on) { impl_->options.trace = on; }

Debugger::State Debugger::state() const { return impl_->current; }

const RunResult &Debugger::outcome() const { return impl_->sink; }

// -------------------------------------------------------------- breakpoints

void Debugger::add_breakpoint(uint64_t address) { impl_->breakpoints.insert(address); }
void Debugger::remove_breakpoint(uint64_t address) { impl_->breakpoints.erase(address); }
void Debugger::clear_breakpoints() { impl_->breakpoints.clear(); }

std::vector<uint64_t> Debugger::breakpoints() const
{
    return std::vector<uint64_t>(impl_->breakpoints.begin(), impl_->breakpoints.end());
}

void Debugger::add_watchpoint(uint64_t address, uint64_t size)
{
    remove_watchpoint(address);
    Impl::Watch watch;
    watch.address = address;
    watch.size = size == 0 ? 1 : size;
    impl_->watchpoints.push_back(watch);
}

void Debugger::remove_watchpoint(uint64_t address)
{
    std::vector<Impl::Watch> &all = impl_->watchpoints;
    all.erase(std::remove_if(all.begin(), all.end(),
                             [address](const Impl::Watch &watch) {
                                 return watch.address == address;
                             }),
              all.end());
}

void Debugger::clear_watchpoints() { impl_->watchpoints.clear(); }

// ------------------------------------------------------------------- state

std::vector<Debugger::Register> Debugger::registers() const { return impl_->register_list(); }

bool Debugger::set_register(const std::string &name, uint64_t value, std::string &error)
{
    try {
        impl_->memory->state().setValue(name, value);
    } catch (ghidra::LowlevelError &failure) {
        error = failure.explain;
        return false;
    }
    error.clear();
    return true;
}

std::vector<uint8_t> Debugger::read(uint64_t address, uint64_t size) const
{
    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(size));
    for (uint64_t i = 0; i < size; ++i) {
        try {
            out.push_back(
                static_cast<uint8_t>(impl_->memory->state().getValue(impl_->ram, address + i, 1)));
        } catch (ghidra::LowlevelError &) {
            break; // the range leaves the memory the program has
        }
    }
    return out;
}

bool Debugger::write(uint64_t address, const std::vector<uint8_t> &bytes, std::string &error)
{
    try {
        for (size_t i = 0; i < bytes.size(); ++i)
            impl_->memory->state().setValue(impl_->ram, address + i, 1, bytes[i]);
    } catch (ghidra::LowlevelError &failure) {
        error = failure.explain;
        return false;
    }
    if (!bytes.empty())
        impl_->wrote(address, bytes.size());
    // A write can land on the instruction the machine has already decoded, so
    // it is fetched again rather than left as it was read a moment ago.
    impl_->machine->setExecuteAddress(impl_->machine->getExecuteAddress());
    error.clear();
    return true;
}

std::vector<Debugger::Frame> Debugger::stack() const
{
    Impl &impl = *impl_;
    std::vector<Frame> out;
    Frame here;
    here.address = impl.where();
    here.frame_pointer = impl.register_value(impl.arm ? "x29" : "RBP");
    here.function = impl.name_for(here.address);
    out.push_back(here);

    // The chain of saved frame pointers, walked until it stops making sense.
    // Nothing is invented: a link that is not a plausible frame ends the walk.
    uint64_t frame = here.frame_pointer;
    const uint64_t floor = kStackTop - kStackSize;
    for (int i = 0; i < kMaxFrames; ++i) {
        if (frame < floor || frame >= kStackTop)
            break;
        uint64_t saved = 0;
        uint64_t back = 0;
        try {
            saved = impl.memory->state().getValue(impl.ram, frame, 8);
            back = impl.memory->state().getValue(impl.ram, frame + 8, 8);
        } catch (ghidra::LowlevelError &) {
            break;
        }
        if (back == kFinished || back == 0)
            break;
        if (impl.image == nullptr || !impl.image->contains(back))
            break;
        // A frame pointer that has been reused for something else shows up as a
        // chain that stops climbing.
        if (saved <= frame)
            break;
        Frame caller;
        caller.address = back;
        caller.frame_pointer = saved;
        caller.function = impl.name_for(back);
        out.push_back(caller);
        frame = saved;
    }
    return out;
}

// ----------------------------------------------------------------- calling

Debugger::CallResult Debugger::call(uint64_t address, const std::vector<Argument> &arguments,
                                    uint64_t step_limit)
{
    Impl &impl = *impl_;
    CallResult answer;
    if (impl.machine == nullptr) {
        answer.error = "there is nothing to call into";
        return answer;
    }

    // Everything the call touches is put back afterwards, so asking a question
    // of a function never changes the run it was asked from.
    const std::vector<uint8_t> before = snapshot();
    const State was = impl.current;
    const uint64_t mark = impl.library->allocation_mark();
    const size_t output_before = impl.sink.output.size();

    // Text arguments are written where the call can reach them and passed as
    // pointers, which is what a C function expects of a string.
    std::vector<uint64_t> values;
    for (const Argument &argument : arguments) {
        if (!argument.is_text) {
            values.push_back(argument.value);
            continue;
        }
        const uint64_t where = impl.library->allocate(argument.text.size() + 1);
        impl.library->write_text(where, argument.text);
        values.push_back(where);
    }

    static const char *arm_order[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
    static const char *x86_order[] = {"RDI", "RSI", "RDX", "RCX", "R8", "R9"};
    const size_t slots = impl.arm ? 8 : 6;
    for (size_t i = 0; i < values.size() && i < slots; ++i)
        impl.set_register_value(impl.arm ? arm_order[i] : x86_order[i], values[i]);

    // A frame of its own, and a return address nothing else can be at, so the
    // end of the call is unmistakable.
    if (impl.arm) {
        impl.set_register_value("sp", kStackTop);
        impl.set_register_value("x30", kFinished);
    } else {
        impl.set_register_value("RSP", kStackTop - 8);
        impl.write_word(kStackTop - 8, 8, kFinished);
    }
    impl.machine->set_depth(0);
    impl.machine->setExecuteAddress(ghidra::Address(impl.ram, address));

    const uint64_t budget = step_limit != 0 ? step_limit
                            : impl.options.step_limit != 0 ? impl.options.step_limit
                                                           : kCallSteps;
    const uint64_t started = impl.steps;
    bool finished = false;
    for (uint64_t used = 0; used < budget; ++used) {
        if (impl.where() == kFinished) {
            finished = true;
            break;
        }
        if (impl.advance() == Impl::Advance::Ended) {
            // Reaching the end is how a call finishes; anything else is a
            // failure worth reporting rather than a result.
            finished = impl.current.stop == Stop::Finished;
            if (!finished)
                answer.error = impl.current.reason;
            break;
        }
    }
    if (finished) {
        answer.ok = true;
        answer.result = impl.arm ? impl.register_value("x0") : impl.register_value("RAX");
    } else if (answer.error.empty()) {
        answer.error = "the call did not return inside the budget it was given";
    }
    answer.steps = impl.steps - started;
    if (impl.sink.output.size() > output_before)
        answer.output = impl.sink.output.substr(output_before);

    std::string ignored;
    restore(before, ignored);
    impl.library->set_allocation_mark(mark);
    impl.current = was;
    return answer;
}

// --------------------------------------------------------------- snapshots

std::vector<uint8_t> Debugger::snapshot() const
{
    Impl &impl = *impl_;
    std::vector<uint8_t> out;
    put(out, kSnapshotMagic, sizeof kSnapshotMagic);
    put_number(out, impl.where());
    put_number(out, static_cast<uint64_t>(impl.machine->depth()));
    put_number(out, impl.steps);
    put_number(out, impl.library->allocation_mark());
    put_number(out, static_cast<uint64_t>(impl.current.stop));
    put_number(out, impl.current.live ? 1 : 0);
    put_text(out, impl.sink.output);
    put_number(out, impl.sink.calls.size());
    for (const std::string &one : impl.sink.calls)
        put_text(out, one);

    const std::vector<Register> registers = impl.register_list();
    put_number(out, registers.size());
    for (const Register &one : registers) {
        put_text(out, one.name);
        put_number(out, one.value);
    }

    // Only the pages the program has written. The rest of the address space is
    // still the file, which has not changed and does not need carrying.
    put_number(out, impl.dirty.size());
    for (uint64_t page : impl.dirty) {
        put_number(out, page);
        std::vector<uint8_t> bytes(kPage, 0);
        for (uint64_t i = 0; i < kPage; ++i) {
            try {
                bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(
                    impl.memory->state().getValue(impl.ram, page * kPage + i, 1));
            } catch (ghidra::LowlevelError &) {
                break;
            }
        }
        put(out, bytes.data(), bytes.size());
    }
    return out;
}

bool Debugger::restore(const std::vector<uint8_t> &snapshot, std::string &error)
{
    Impl &impl = *impl_;
    if (snapshot.size() < sizeof kSnapshotMagic ||
        std::memcmp(snapshot.data(), kSnapshotMagic, sizeof kSnapshotMagic) != 0) {
        error = "those bytes are not a snapshot of this machine";
        return false;
    }
    Reader reader(snapshot.data() + sizeof kSnapshotMagic,
                  snapshot.size() - sizeof kSnapshotMagic);

    uint64_t address = 0;
    uint64_t depth = 0;
    uint64_t steps = 0;
    uint64_t mark = 0;
    uint64_t stop = 0;
    uint64_t live = 0;
    std::string output;
    uint64_t call_count = 0;
    if (!reader.number(address) || !reader.number(depth) || !reader.number(steps) ||
        !reader.number(mark) || !reader.number(stop) || !reader.number(live) ||
        !reader.text(output) || !reader.number(call_count)) {
        error = "the snapshot ends before it should";
        return false;
    }
    std::vector<std::string> calls;
    for (uint64_t i = 0; i < call_count; ++i) {
        std::string one;
        if (!reader.text(one)) {
            error = "the snapshot ends before it should";
            return false;
        }
        calls.push_back(one);
    }

    uint64_t register_count = 0;
    if (!reader.number(register_count)) {
        error = "the snapshot ends before it should";
        return false;
    }
    std::vector<std::pair<std::string, uint64_t>> registers;
    for (uint64_t i = 0; i < register_count; ++i) {
        std::string name;
        uint64_t value = 0;
        if (!reader.text(name) || !reader.number(value)) {
            error = "the snapshot ends before it should";
            return false;
        }
        registers.emplace_back(name, value);
    }

    uint64_t page_count = 0;
    if (!reader.number(page_count)) {
        error = "the snapshot ends before it should";
        return false;
    }
    std::map<uint64_t, std::vector<uint8_t>> pages;
    for (uint64_t i = 0; i < page_count; ++i) {
        uint64_t page = 0;
        std::vector<uint8_t> bytes(kPage);
        if (!reader.number(page) || !reader.take(bytes.data(), bytes.size())) {
            error = "the snapshot ends before it should";
            return false;
        }
        pages.emplace(page, std::move(bytes));
    }

    // Pages written since the snapshot but untouched before it have to go back
    // to what the file says, or to nothing when the file says nothing there.
    for (uint64_t page : impl.dirty) {
        if (pages.count(page) != 0)
            continue;
        std::vector<uint8_t> bytes(kPage, 0);
        if (impl.image != nullptr)
            impl.image->read(page * kPage, bytes.data(), bytes.size());
        pages.emplace(page, std::move(bytes));
    }

    for (const auto &entry : pages) {
        for (uint64_t i = 0; i < kPage; ++i) {
            try {
                impl.memory->state().setValue(impl.ram, entry.first * kPage + i, 1,
                                              entry.second[static_cast<size_t>(i)]);
            } catch (ghidra::LowlevelError &) {
                break;
            }
        }
        impl.dirty.insert(entry.first);
    }

    for (const auto &one : registers)
        impl.set_register_value(one.first.c_str(), one.second);

    impl.sink.output = output;
    impl.sink.calls = calls;
    impl.output_seen = output.size();
    impl.calls_seen = calls.size();
    impl.steps = steps;
    impl.library->set_allocation_mark(mark);
    impl.machine->set_depth(static_cast<int64_t>(depth));
    impl.machine->setExecuteAddress(ghidra::Address(impl.ram, address));
    impl.current.stop = static_cast<Stop>(stop);
    impl.current.live = live != 0;
    impl.refresh();
    error.clear();
    return true;
}

} // namespace emulator
} // namespace astral_internal
