#include "machine.hh"

#include "image.hh"

#include "architecture.hh"
#include "emulate.hh"
#include "loadimage.hh"
#include "memstate.hh"
#include "translate.hh"

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace astral_internal {
namespace emulator {
namespace {

// Where the stack lives, and where a return from the outermost call lands. Both
// are chosen to be somewhere no real program has mapped, so reaching either is
// unmistakable rather than a coincidence.
const uint64_t kStackTop = 0x0000700000000000ull;
const uint64_t kStackSize = 1u << 20;
const uint64_t kFinished = 0x00000000deadbeefull;
// Where strings handed to the program are put, and where malloc hands memory
// from. Both sit above the stack, well clear of the image.
const uint64_t kScratch = 0x0000710000000000ull;

// Collects the p-code one instruction lowers to.
class OpCollector : public ghidra::PcodeEmit {
public:
    std::vector<ghidra::PcodeOpRaw *> ops;
    std::vector<ghidra::VarnodeData *> nodes;

    ~OpCollector() override { clear(); }

    void clear()
    {
        for (ghidra::PcodeOpRaw *op : ops)
            delete op;
        for (ghidra::VarnodeData *node : nodes)
            delete node;
        ops.clear();
        nodes.clear();
    }

    void dump(const ghidra::Address &addr, ghidra::OpCode opcode, ghidra::VarnodeData *outvar,
              ghidra::VarnodeData *vars, ghidra::int4 isize) override
    {
        ghidra::PcodeOpRaw *op = new ghidra::PcodeOpRaw();
        op->setSeqNum(addr, static_cast<ghidra::uintm>(ops.size()));
        op->setBehavior(behaviors_ == nullptr ? nullptr : (*behaviors_)[opcode]);
        if (outvar != nullptr) {
            ghidra::VarnodeData *out = new ghidra::VarnodeData(*outvar);
            nodes.push_back(out);
            op->setOutput(out);
        }
        for (ghidra::int4 i = 0; i < isize; ++i) {
            ghidra::VarnodeData *in = new ghidra::VarnodeData(vars[i]);
            nodes.push_back(in);
            op->addInput(in);
        }
        ops.push_back(op);
    }

    void setBehaviors(const std::vector<ghidra::OpBehavior *> *behaviors) { behaviors_ = behaviors; }

private:
    const std::vector<ghidra::OpBehavior *> *behaviors_ = nullptr;
};

// The machine itself: p-code over a memory state, one instruction at a time.
class Machine : public ghidra::EmulateMemory {
public:
    Machine(ghidra::Architecture *arch, ghidra::MemoryState *state)
        : ghidra::EmulateMemory(state), arch_(arch)
    {
        arch_->collectBehaviors(behaviors_);
        collector_.setBehaviors(&behaviors_);
    }

    void setExecuteAddress(const ghidra::Address &addr) override
    {
        address_ = addr;
        fetch();
    }

    ghidra::Address getExecuteAddress() const override { return address_; }

    // Move to the next p-code op of this instruction, or on to the next
    // instruction when they are done.
    void fallthruOp() override
    {
        ++position_;
        if (position_ >= collector_.ops.size()) {
            address_ = address_ + length_;
            fetch();
        } else {
            point_at(position_);
        }
    }

    void executeBranch() override
    {
        const ghidra::VarnodeData *destination = currentOp->getInput(0);
        if (destination->space->getType() == ghidra::IPTR_CONSTANT) {
            // A branch inside one instruction's own p-code.
            const ghidra::intb shift = static_cast<ghidra::intb>(destination->offset);
            const ghidra::intb want = static_cast<ghidra::intb>(position_) + shift;
            if (want < 0 || want >= static_cast<ghidra::intb>(collector_.ops.size())) {
                address_ = address_ + length_;
                fetch();
                return;
            }
            position_ = static_cast<size_t>(want);
            point_at(position_);
            return;
        }
        address_ = ghidra::Address(destination->space, destination->offset);
        fetch();
    }

    void executeBranchind() override
    {
        const uint64_t where = memstate->getValue(currentOp->getInput(0));
        address_ = ghidra::Address(address_.getSpace(), where);
        fetch();
    }

    void executeCall() override
    {
        const ghidra::VarnodeData *destination = currentOp->getInput(0);
        address_ = ghidra::Address(destination->space, destination->offset);
        fetch();
    }

    void executeCallind() override
    {
        const uint64_t where = memstate->getValue(currentOp->getInput(0));
        address_ = ghidra::Address(address_.getSpace(), where);
        fetch();
    }

    // A processor-specific operation Astral does not model. Stepping over it is
    // wrong in general but right for the ones that turn up in ordinary code -
    // memory barriers, hints, pointer authentication - none of which change a
    // value the program then reads.
    void executeCallother() override { fallthruOp(); }
    void executeMultiequal() override { fallthruOp(); }
    void executeIndirect() override { fallthruOp(); }
    void executeSegmentOp() override { fallthruOp(); }
    void executeCpoolRef() override { fallthruOp(); }
    void executeNew() override { fallthruOp(); }

    bool at_instruction_start() const { return position_ == 0; }
    size_t instruction_length() const { return length_; }
    bool decoded() const { return decoded_; }

private:
    void point_at(size_t index)
    {
        currentOp = collector_.ops[index];
        currentBehave = currentOp->getBehavior();
    }

    void fetch()
    {
        collector_.clear();
        position_ = 0;
        decoded_ = false;
        try {
            length_ = static_cast<size_t>(arch_->translate->oneInstruction(collector_, address_));
        } catch (ghidra::LowlevelError &) {
            length_ = 0;
        } catch (ghidra::DecoderError &) {
            length_ = 0;
        }
        if (length_ == 0 || collector_.ops.empty())
            return;
        decoded_ = true;
        point_at(0);
    }

    ghidra::Architecture *arch_;
    std::vector<ghidra::OpBehavior *> behaviors_;
    OpCollector collector_;
    ghidra::Address address_;
    size_t position_ = 0;
    size_t length_ = 0;
    bool decoded_ = false;
};

} // namespace
} // namespace emulator
} // namespace astral_internal

namespace astral_internal {
namespace emulator {
namespace {

// What the C library does, done here instead. A program under emulation has no
// operating system to ask, so the calls it makes into the library are answered
// by Astral: that is what lets it run at all.
class Library {
public:
    Library(ghidra::MemoryState *state, ghidra::AddrSpace *ram, RunResult &result,
            const std::string &input)
        : state_(state), ram_(ram), result_(result), input_(input)
    {
    }

    // Apple's ABI passes the arguments past a format string on the stack rather
    // than in registers, so that is where they are read from. Getting this wrong
    // prints whatever happened to be in a register instead of what was passed.
    void set_variadic_base(uint64_t stack) { variadic_ = stack; }

    // Reads a NUL-terminated string out of the program's memory.
    std::string text_at(uint64_t address) const
    {
        std::string out;
        for (int i = 0; i < 4096; ++i) {
            const uint8_t c = static_cast<uint8_t>(state_->getValue(ram_, address + i, 1));
            if (c == 0)
                break;
            out.push_back(static_cast<char>(c));
        }
        return out;
    }

    void write_text(uint64_t address, const std::string &text)
    {
        for (size_t i = 0; i < text.size(); ++i)
            state_->setValue(ram_, address + i, 1, static_cast<uint8_t>(text[i]));
        state_->setValue(ram_, address + text.size(), 1, 0);
    }

    uint64_t allocate(uint64_t bytes)
    {
        const uint64_t where = next_;
        next_ += (bytes + 15) & ~uint64_t(15);
        return where;
    }

    // Answers one call. `arguments` are the values the architecture passed.
    // Returns false when Astral has nothing to say for this name, which is
    // reported rather than guessed at.
    bool call(const std::string &name, const std::vector<uint64_t> &arguments, uint64_t &result)
    {
        result = 0;
        auto argument = [&](size_t i) { return i < arguments.size() ? arguments[i] : 0; };

        if (name == "puts") {
            result_.output += text_at(argument(0));
            result_.output += '\n';
            result = 1;
            return true;
        }
        if (name == "putchar") {
            result_.output.push_back(static_cast<char>(argument(0)));
            result = argument(0);
            return true;
        }
        if (name == "printf" || name == "fprintf") {
            const size_t first = name == "printf" ? 0 : 1;
            // Only the fixed arguments are in registers on this ABI; everything
            // past the format string was pushed, so the conversions read from
            // the stack. Passing the registers on would print whatever happened
            // to be in them.
            const std::vector<uint64_t> fixed(arguments.begin(),
                                              arguments.begin() + static_cast<long>(first + 1));
            result_.output += format(text_at(argument(first)), fixed, first + 1);
            result = 1;
            return true;
        }
        if (name == "sprintf" || name == "snprintf") {
            const size_t shape = name == "sprintf" ? 1 : 2;
            const std::vector<uint64_t> fixed(arguments.begin(),
                                              arguments.begin() + static_cast<long>(shape + 1));
            const std::string text = format(text_at(argument(shape)), fixed, shape + 1);
            write_text(argument(0), text);
            result = text.size();
            return true;
        }
        if (name == "strlen") {
            result = text_at(argument(0)).size();
            return true;
        }
        if (name == "strcmp") {
            const std::string a = text_at(argument(0));
            const std::string b = text_at(argument(1));
            result = static_cast<uint64_t>(static_cast<int64_t>(a.compare(b) < 0   ? -1
                                                                : a.compare(b) > 0 ? 1
                                                                                   : 0));
            return true;
        }
        if (name == "strncmp") {
            const std::string a = text_at(argument(0));
            const std::string b = text_at(argument(1));
            const size_t n = static_cast<size_t>(argument(2));
            const int order = a.compare(0, n, b, 0, n);
            result = static_cast<uint64_t>(static_cast<int64_t>(order < 0 ? -1 : order > 0 ? 1 : 0));
            return true;
        }
        if (name == "malloc" || name == "calloc") {
            const uint64_t size = name == "malloc" ? argument(0) : argument(0) * argument(1);
            result = allocate(size == 0 ? 1 : size);
            if (name == "calloc")
                for (uint64_t i = 0; i < size; ++i)
                    state_->setValue(ram_, result + i, 1, 0);
            return true;
        }
        if (name == "free") {
            return true; // nothing is reclaimed; a run is short
        }
        if (name == "memset") {
            for (uint64_t i = 0; i < argument(2); ++i)
                state_->setValue(ram_, argument(0) + i, 1, argument(1) & 0xff);
            result = argument(0);
            return true;
        }
        if (name == "memcpy" || name == "memmove") {
            for (uint64_t i = 0; i < argument(2); ++i)
                state_->setValue(ram_, argument(0) + i, 1,
                                 state_->getValue(ram_, argument(1) + i, 1));
            result = argument(0);
            return true;
        }
        if (name == "strcpy") {
            const std::string text = text_at(argument(1));
            write_text(argument(0), text);
            result = argument(0);
            return true;
        }
        if (name == "exit" || name == "_exit") {
            result = argument(0);
            exited_ = true;
            return true;
        }
        if (name == "fgets" || name == "gets") {
            // One line of what the run was given to read.
            if (offered_ >= input_.size())
                return true; // nothing left: a null answer
            size_t stop = input_.find('\n', offered_);
            if (stop == std::string::npos)
                stop = input_.size();
            std::string line = input_.substr(offered_, stop - offered_);
            offered_ = stop + 1;
            if (name == "fgets")
                line.push_back('\n');
            write_text(argument(0), line);
            result = argument(0);
            return true;
        }
        return false;
    }

    bool exited() const { return exited_; }

private:
    // Only what turns up in the programs this runs: the whole of printf is a
    // library of its own, and guessing at the rest would print nonsense.
    std::string format(const std::string &shape, const std::vector<uint64_t> &arguments,
                       size_t next) const
    {
        std::string out;
        for (size_t i = 0; i < shape.size(); ++i) {
            if (shape[i] != '%' || i + 1 >= shape.size()) {
                out.push_back(shape[i]);
                continue;
            }
            size_t j = i + 1;
            while (j < shape.size() && std::strchr("-+ #0123456789.lhz", shape[j]) != nullptr)
                ++j;
            if (j >= shape.size())
                break;
            const char kind = shape[j];
            uint64_t value = 0;
            if (next < arguments.size()) {
                value = arguments[next];
            } else if (variadic_ != 0) {
                value = state_->getValue(ram_, variadic_ + (next - arguments.size()) * 8, 8);
            }
            char piece[64];
            switch (kind) {
            case '%': out.push_back('%'); i = j; continue;
            case 's': out += text_at(value); ++next; break;
            case 'c': out.push_back(static_cast<char>(value)); ++next; break;
            case 'd': case 'i':
                std::snprintf(piece, sizeof piece, "%lld",
                              static_cast<long long>(static_cast<int64_t>(
                                  static_cast<int32_t>(value))));
                out += piece;
                ++next;
                break;
            case 'u':
                std::snprintf(piece, sizeof piece, "%llu",
                              static_cast<unsigned long long>(static_cast<uint32_t>(value)));
                out += piece;
                ++next;
                break;
            case 'x': case 'X': case 'p':
                std::snprintf(piece, sizeof piece, kind == 'X' ? "%llX" : "%llx",
                              static_cast<unsigned long long>(value));
                out += piece;
                ++next;
                break;
            default:
                out.push_back('%');
                out.push_back(kind);
                ++next;
                break;
            }
            i = j;
        }
        return out;
    }

    ghidra::MemoryState *state_;
    ghidra::AddrSpace *ram_;
    RunResult &result_;
    std::string input_;
    size_t offered_ = 0;
    uint64_t next_ = kScratch + 0x10000;
    bool exited_ = false;
    uint64_t variadic_ = 0;
};

} // namespace
} // namespace emulator
} // namespace astral_internal

namespace astral_internal {
namespace emulator {

RunResult run(ghidra::Architecture *architecture, const BinaryImage &image,
              const RunOptions &options)
{
    RunResult result;
    if (architecture == nullptr || architecture->translate == nullptr) {
        result.error = "there is no architecture to run this on";
        return result;
    }
    ghidra::AddrSpace *ram = architecture->getDefaultCodeSpace();

    // The program's own bytes, with anything written during the run kept in
    // pages of its own so the file is never touched.
    // The memory state looks registers up through the translator and does not
    // change it; the interface simply predates saying so.
    ghidra::MemoryState state(const_cast<ghidra::Translate *>(architecture->translate));
    std::vector<std::unique_ptr<ghidra::MemoryBank>> banks;
    for (ghidra::int4 i = 0; i < architecture->translate->numSpaces(); ++i) {
        ghidra::AddrSpace *space = architecture->translate->getSpace(i);
        if (space == nullptr)
            continue;
        ghidra::MemoryBank *under = nullptr;
        if (space == ram) {
            banks.push_back(std::make_unique<ghidra::MemoryImage>(space, 8, 4096,
                                                                 architecture->loader));
            under = banks.back().get();
        }
        banks.push_back(std::make_unique<ghidra::MemoryPageOverlay>(space, 8, 4096, under));
        state.setMemoryBank(banks.back().get());
    }

    Library library(&state, ram, result, options.input);

    // The arguments, laid out where a program expects to find them.
    std::vector<uint64_t> argv;
    for (const std::string &one : options.arguments) {
        const uint64_t where = library.allocate(one.size() + 1);
        library.write_text(where, one);
        argv.push_back(where);
    }
    const uint64_t argv_table = library.allocate((argv.size() + 1) * 8);
    for (size_t i = 0; i < argv.size(); ++i)
        state.setValue(ram, argv_table + i * 8, 8, argv[i]);
    state.setValue(ram, argv_table + argv.size() * 8, 8, 0);

    // Which addresses stand for a name from another image: reaching one is a
    // call into the library, which is answered rather than executed.
    std::map<uint64_t, std::string> imports;
    for (const Symbol &symbol : image.symbols)
        if (symbol.is_import && !symbol.name.empty())
            imports.emplace(symbol.address, symbol.name);

    bool arm = false;
    try {
        arm = architecture->translate->getRegister("x0").size != 0;
    } catch (ghidra::LowlevelError &) {
        arm = false;
    }
    auto set_register = [&](const char *name, uint64_t value) {
        try {
            state.setValue(name, value);
        } catch (ghidra::LowlevelError &) {
        }
    };
    auto get_register = [&](const char *name) -> uint64_t {
        try {
            return state.getValue(name);
        } catch (ghidra::LowlevelError &) {
            return 0;
        }
    };

    uint64_t entry = options.entry;
    if (entry == 0 && !image.entry_points.empty())
        entry = image.entry_points.front();
    if (entry == 0) {
        result.error = "there is nowhere to start: give a function to run";
        return result;
    }

    if (arm) {
        set_register("sp", kStackTop);
        set_register("x30", kFinished);           // where a return lands
        set_register("x0", options.arguments.size());
        set_register("x1", argv_table);
    } else {
        set_register("RSP", kStackTop - 8);
        // A return reads the address off the stack, so it is put there.
        state.setValue(ram, kStackTop - 8, 8, kFinished);
        set_register("RDI", options.arguments.size());
        set_register("RSI", argv_table);
    }

    Machine machine(architecture, &state);
    machine.setExecuteAddress(ghidra::Address(ram, entry));

    while (result.steps < options.step_limit) {
        const uint64_t at = machine.getExecuteAddress().getOffset();

        if (at == kFinished) {
            result.returned = true;
            result.result = arm ? get_register("x0") : get_register("RAX");
            result.stopped_because = "returned";
            break;
        }

        const auto import = imports.find(at);
        if (import != imports.end()) {
            std::vector<uint64_t> arguments;
            static const char *arm_order[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
            static const char *x86_order[] = {"RDI", "RSI", "RDX", "RCX", "R8", "R9"};
            const size_t count = arm ? 8 : 6;
            for (size_t i = 0; i < count; ++i)
                arguments.push_back(get_register(arm ? arm_order[i] : x86_order[i]));
            library.set_variadic_base(arm ? get_register("sp") : get_register("RSP") + 8);
            uint64_t answer = 0;
            if (!library.call(import->second, arguments, answer)) {
                result.stopped_because = "a call to " + import->second +
                                         ", which Astral does not know how to answer";
                result.stopped_at = at;
                result.ok = true;
                return result;
            }
            result.calls.push_back(import->second);
            set_register(arm ? "x0" : "RAX", answer);
            if (library.exited()) {
                result.returned = true;
                result.result = answer;
                result.stopped_because = "the program exited";
                break;
            }
            // Back to whoever called, the way a return would go.
            uint64_t back = 0;
            if (arm) {
                back = get_register("x30");
            } else {
                const uint64_t stack = get_register("RSP");
                back = state.getValue(ram, stack, 8);
                set_register("RSP", stack + 8);
            }
            machine.setExecuteAddress(ghidra::Address(ram, back));
            continue;
        }

        if (!machine.decoded()) {
            result.stopped_because = "there is no instruction to run there";
            result.stopped_at = at;
            result.ok = true;
            return result;
        }

        if (options.trace && machine.at_instruction_start()) {
            std::string listing;
            std::string ignored;
            char line[64];
            std::snprintf(line, sizeof line, "0x%llx", static_cast<unsigned long long>(at));
            result.trace.push_back(line);
        }

        try {
            machine.executeCurrentOp();
        } catch (ghidra::LowlevelError &error) {
            result.stopped_because = error.explain;
            result.stopped_at = at;
            result.ok = true;
            return result;
        }
        if (machine.at_instruction_start())
            ++result.steps;
    }

    if (result.stopped_because.empty())
        result.stopped_because = "the step limit was reached";
    result.stopped_at = machine.getExecuteAddress().getOffset();
    result.ok = true;
    return result;
}

} // namespace emulator
} // namespace astral_internal
