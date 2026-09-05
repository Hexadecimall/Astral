// The parts of the machine that both running and debugging need.
//
// The debugger in debug.cc drives a program an instruction at a time, and `run`
// in machine.cc is that same debugger told never to stop. The machine, its
// memory and its stand-in C library are therefore written once, here, rather
// than twice in two places free to drift apart about what a program does.
#ifndef ASTRAL_MACHINE_INTERNAL_HH
#define ASTRAL_MACHINE_INTERNAL_HH

#include "machine.hh"

#include "architecture.hh"
#include "emulate.hh"
#include "loadimage.hh"
#include "memstate.hh"
#include "translate.hh"

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace astral_internal {
namespace emulator {

// Where the stack lives, and where a return from the outermost call lands. Both
// are chosen to be somewhere no real program has mapped, so reaching either is
// unmistakable rather than a coincidence.
const uint64_t kStackTop = 0x0000700000000000ull;
const uint64_t kStackSize = 1u << 20;
const uint64_t kFinished = 0x00000000deadbeefull;
// Where strings handed to the program are put, and where malloc hands memory
// from. Both sit above the stack, well clear of the image.
const uint64_t kScratch = 0x0000710000000000ull;

// Told about every byte the program writes. Running has no use for this;
// watchpoints and snapshots do, and the only way to know a page changed is to
// be told at the moment it does.
class WriteWatcher {
public:
    virtual ~WriteWatcher() = default;
    virtual void wrote(uint64_t address, uint64_t size) = 0;
};

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

// One instruction rendered as text, in the same form the disassembler produces:
// "<address>: <mnemonic> <operands>". A trace is written this way so it can be
// handed to the readable listing afterwards rather than needing a second
// renderer that would say the same thing differently.
class LineEmit : public ghidra::AssemblyEmit {
public:
    std::string text;

    void dump(const ghidra::Address &addr, const std::string &mnemonic,
              const std::string &body) override
    {
        std::ostringstream out;
        addr.printRaw(out);
        out << ": " << mnemonic << ' ' << body;
        text = out.str();
    }
};

// The line for one address, empty when nothing decodes there.
inline std::string assembly_line(ghidra::Architecture *arch, uint64_t address)
{
    LineEmit emit;
    try {
        arch->translate->printAssembly(emit,
                                       ghidra::Address(arch->getDefaultCodeSpace(), address));
    } catch (ghidra::LowlevelError &) {
        return std::string();
    }
    return emit.text;
}

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

    // A RETURN reaches here as well as a computed jump, and the two have to be
    // told apart: only one of them ends a frame, and the depth is what stepping
    // over a call and stepping out of one are counted with.
    void executeBranchind() override
    {
        if (currentBehave != nullptr && currentBehave->getOpcode() == ghidra::CPUI_RETURN)
            --depth_;
        const uint64_t where = memstate->getValue(currentOp->getInput(0));
        address_ = ghidra::Address(address_.getSpace(), where);
        fetch();
    }

    void executeCall() override
    {
        ++depth_;
        const ghidra::VarnodeData *destination = currentOp->getInput(0);
        address_ = ghidra::Address(destination->space, destination->offset);
        fetch();
    }

    void executeCallind() override
    {
        ++depth_;
        const uint64_t where = memstate->getValue(currentOp->getInput(0));
        address_ = ghidra::Address(address_.getSpace(), where);
        fetch();
    }

    // Every write the program makes passes through here, which is the one place
    // a watchpoint can be answered without comparing memory to itself after
    // every instruction.
    void executeStore() override
    {
        ghidra::AddrSpace *space = currentOp->getInput(0)->getSpaceFromConst();
        const uint64_t where = ghidra::AddrSpace::addressToByte(
            memstate->getValue(currentOp->getInput(1)), space->getWordSize());
        const uint64_t size = static_cast<uint64_t>(currentOp->getInput(2)->size);
        ghidra::EmulateMemory::executeStore();
        if (watcher_ != nullptr && space == arch_->getDefaultCodeSpace())
            watcher_->wrote(where, size);
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

    void set_watcher(WriteWatcher *watcher) { watcher_ = watcher; }

    // How many calls deep the program is, counted from wherever it started.
    // Negative once it has returned past its own first frame.
    int64_t depth() const { return depth_; }
    void set_depth(int64_t depth) { depth_ = depth; }
    void note_return() { --depth_; }

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
    WriteWatcher *watcher_ = nullptr;
    int64_t depth_ = 0;
};

// The program's memory: the file's own bytes underneath, and pages of its own
// for anything written, so what is on disk is never touched.
class Memory {
public:
    explicit Memory(ghidra::Architecture *architecture)
        // The memory state looks registers up through the translator and does
        // not change it; the interface simply predates saying so.
        : state_(const_cast<ghidra::Translate *>(architecture->translate))
    {
        ghidra::AddrSpace *ram = architecture->getDefaultCodeSpace();
        for (ghidra::int4 i = 0; i < architecture->translate->numSpaces(); ++i) {
            ghidra::AddrSpace *space = architecture->translate->getSpace(i);
            if (space == nullptr)
                continue;
            ghidra::MemoryBank *under = nullptr;
            if (space == ram) {
                banks_.push_back(std::make_unique<ghidra::MemoryImage>(space, 8, 4096,
                                                                      architecture->loader));
                under = banks_.back().get();
            }
            banks_.push_back(std::make_unique<ghidra::MemoryPageOverlay>(space, 8, 4096, under));
            state_.setMemoryBank(banks_.back().get());
        }
    }

    ghidra::MemoryState &state() { return state_; }

private:
    ghidra::MemoryState state_;
    std::vector<std::unique_ptr<ghidra::MemoryBank>> banks_;
};

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

    void set_watcher(WriteWatcher *watcher) { watcher_ = watcher; }

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
        note(address, text.size() + 1);
    }

    uint64_t allocate(uint64_t bytes)
    {
        const uint64_t where = next_;
        next_ += (bytes + 15) & ~uint64_t(15);
        return where;
    }

    // Where the next allocation would land, and a way to put it back: a call
    // made by hand borrows scratch memory and should not leak it into the run
    // it was made from.
    uint64_t allocation_mark() const { return next_; }
    void set_allocation_mark(uint64_t mark) { next_ = mark; }

    void reset_input() { offered_ = 0; }

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
            result_.output += format(text_at(argument(first)), arguments, first + 1);
            result = 1;
            return true;
        }
        if (name == "sprintf" || name == "snprintf") {
            const size_t shape = name == "sprintf" ? 1 : 2;
            const std::string text = format(text_at(argument(shape)), arguments, shape + 1);
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
            if (name == "calloc") {
                for (uint64_t i = 0; i < size; ++i)
                    state_->setValue(ram_, result + i, 1, 0);
                note(result, size);
            }
            return true;
        }
        if (name == "free") {
            return true; // nothing is reclaimed; a run is short
        }
        if (name == "memset") {
            for (uint64_t i = 0; i < argument(2); ++i)
                state_->setValue(ram_, argument(0) + i, 1, argument(1) & 0xff);
            note(argument(0), argument(2));
            result = argument(0);
            return true;
        }
        if (name == "memcpy" || name == "memmove") {
            for (uint64_t i = 0; i < argument(2); ++i)
                state_->setValue(ram_, argument(0) + i, 1,
                                 state_->getValue(ram_, argument(1) + i, 1));
            note(argument(0), argument(2));
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
    void clear_exited() { exited_ = false; }

private:
    void note(uint64_t address, uint64_t size)
    {
        if (watcher_ != nullptr && size != 0)
            watcher_->wrote(address, size);
    }

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
    WriteWatcher *watcher_ = nullptr;
};

// True when this architecture names its registers the way arm64 does. The two
// register sets are the only ones the machine has to tell apart: which one it
// is decides where arguments, the stack pointer and a return address live.
inline bool is_arm(ghidra::Architecture *architecture)
{
    try {
        return architecture->translate->getRegister("x0").size != 0;
    } catch (ghidra::LowlevelError &) {
        return false;
    }
}

} // namespace emulator
} // namespace astral_internal

#endif
