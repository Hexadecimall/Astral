#include "pcode.hh"

#include <map>
#include <sstream>

namespace astral_internal {
namespace nova {
namespace pcode {

namespace {

using ghidra::OpCode;

// Which of a pair of operations a signedness picks.
//
// p-code has no signed values, only signed operations: the same bits are added
// the same way either way, and it is the comparison and the division and the
// shift that have to be told. Choosing here is what makes signedness a property
// of the operation, which is where the machine keeps it too.
OpCode signed_or_not(bool is_signed, OpCode when_signed, OpCode when_not)
{
    return is_signed ? when_signed : when_not;
}

class Writer {
public:
    Writer(const ir::Target &target, std::vector<std::string> &problems)
        : target_(target), problems_(problems)
    {
    }

    bool write(const ir::Function &function, Sequence &out);

private:
    void complain(const std::string &message) { problems_.push_back(message); }

    // Where a value lives. A pinned one is its register, and everything else is
    // given room of its own, which is what the unique space is for.
    Varnode place(const ir::Value &value, int width);
    Varnode constant(uint64_t value, int size) const;

    bool operation(const ir::Instruction &instruction, Block &into);

    const ir::Target &target_;
    std::vector<std::string> &problems_;
    std::map<uint32_t, Varnode> placed_;
    uint64_t next_unique_ = 0;
};

Varnode Writer::constant(uint64_t value, int size) const
{
    Varnode node;
    node.where = Where::Constant;
    node.offset = value;
    node.size = size;
    return node;
}

Varnode Writer::place(const ir::Value &value, int width)
{
    auto already = placed_.find(value.identifier);
    if (already != placed_.end())
        return already->second;

    Varnode node;
    node.size = width > 0 ? width : (target_.word_bytes > 0 ? target_.word_bytes : 8);

    switch (value.storage.kind) {
    case Storage::Kind::Register:
    case Storage::Kind::EntryRegister: {
        // A pinned value is its register, at the offset the specification gives
        // for it. Two names can overlap the same bytes, and the offset is what
        // says so; the name is only what the source wrote.
        const ir::Target::RegisterPlace *where = target_.register_place(value.storage.register_name);
        if (where == nullptr) {
            complain("this processor has no register called " + value.storage.register_name);
            node.where = Where::Unique;
            node.offset = next_unique_;
            next_unique_ += static_cast<uint64_t>(node.size);
            break;
        }
        node.where = Where::Register;
        node.offset = where->offset;
        node.size = where->width;
        break;
    }
    case Storage::Kind::Address:
        node.where = Where::Memory;
        node.offset = value.storage.address;
        break;
    case Storage::Kind::Frame:
        node.where = Where::Frame;
        node.offset = static_cast<uint64_t>(value.storage.offset);
        break;
    case Storage::Kind::None:
    default:
        node.where = Where::Unique;
        node.offset = next_unique_;
        next_unique_ += static_cast<uint64_t>(node.size);
        break;
    }

    placed_.emplace(value.identifier, node);
    return node;
}

bool Writer::operation(const ir::Instruction &instruction, Block &into)
{
    const int width = ir::width_of(instruction, target_);
    const bool is_signed = instruction.is_signed;

    Operation made;
    made.writes = instruction.result.is_valid();
    if (made.writes)
        made.output = place(instruction.result, width);

    // The arguments, each in the place it was given.
    std::vector<Varnode> arguments;
    for (const ir::Value &argument : instruction.arguments)
        arguments.push_back(place(argument, width));

    switch (instruction.operation) {
    case ir::Operation::Constant:
        made.opcode = ghidra::CPUI_COPY;
        made.inputs.push_back(constant(instruction.immediate, width));
        break;
    case ir::Operation::Copy:
        made.opcode = ghidra::CPUI_COPY;
        made.inputs = arguments;
        if (made.inputs.empty())
            made.inputs.push_back(constant(0, width));
        break;

    case ir::Operation::Add: made.opcode = ghidra::CPUI_INT_ADD; made.inputs = arguments; break;
    case ir::Operation::Subtract:
        made.opcode = ghidra::CPUI_INT_SUB;
        made.inputs = arguments;
        break;
    case ir::Operation::Multiply:
        made.opcode = ghidra::CPUI_INT_MULT;
        made.inputs = arguments;
        break;
    case ir::Operation::Divide:
        made.opcode = signed_or_not(is_signed, ghidra::CPUI_INT_SDIV, ghidra::CPUI_INT_DIV);
        made.inputs = arguments;
        break;
    case ir::Operation::Remainder:
        made.opcode = signed_or_not(is_signed, ghidra::CPUI_INT_SREM, ghidra::CPUI_INT_REM);
        made.inputs = arguments;
        break;
    case ir::Operation::BitAnd: made.opcode = ghidra::CPUI_INT_AND; made.inputs = arguments; break;
    case ir::Operation::BitOr: made.opcode = ghidra::CPUI_INT_OR; made.inputs = arguments; break;
    case ir::Operation::BitXor: made.opcode = ghidra::CPUI_INT_XOR; made.inputs = arguments; break;
    case ir::Operation::ShiftLeft:
        made.opcode = ghidra::CPUI_INT_LEFT;
        made.inputs = arguments;
        break;
    case ir::Operation::ShiftRight:
        // An arithmetic shift keeps the sign and a logical one does not, which
        // is a different instruction on every processor and so a different
        // operation here.
        made.opcode = signed_or_not(is_signed, ghidra::CPUI_INT_SRIGHT, ghidra::CPUI_INT_RIGHT);
        made.inputs = arguments;
        break;

    // p-code's names for these two are the other way round from what they look
    // like: negating an integer is a two's complement, and what it calls a
    // negate is flipping every bit.
    case ir::Operation::Negate:
        made.opcode = ghidra::CPUI_INT_2COMP;
        made.inputs = arguments;
        break;
    case ir::Operation::BitNot:
        made.opcode = ghidra::CPUI_INT_NEGATE;
        made.inputs = arguments;
        break;

    case ir::Operation::Equal:
        made.opcode = ghidra::CPUI_INT_EQUAL;
        made.inputs = arguments;
        break;
    case ir::Operation::NotEqual:
        made.opcode = ghidra::CPUI_INT_NOTEQUAL;
        made.inputs = arguments;
        break;
    case ir::Operation::Less:
        made.opcode = signed_or_not(is_signed, ghidra::CPUI_INT_SLESS, ghidra::CPUI_INT_LESS);
        made.inputs = arguments;
        break;
    case ir::Operation::LessOrEqual:
        made.opcode =
            signed_or_not(is_signed, ghidra::CPUI_INT_SLESSEQUAL, ghidra::CPUI_INT_LESSEQUAL);
        made.inputs = arguments;
        break;

    case ir::Operation::Extend:
        made.opcode = signed_or_not(is_signed, ghidra::CPUI_INT_SEXT, ghidra::CPUI_INT_ZEXT);
        made.inputs = arguments;
        break;
    case ir::Operation::Truncate:
        // Taking the low bytes of a value is a subpiece starting at nothing.
        made.opcode = ghidra::CPUI_SUBPIECE;
        made.inputs = arguments;
        made.inputs.push_back(constant(0, 4));
        break;

    case ir::Operation::Load:
        // A load names the space it reads from before the address it reads at.
        made.opcode = ghidra::CPUI_LOAD;
        made.inputs.push_back(constant(static_cast<uint64_t>(instruction.space), 4));
        for (const Varnode &argument : arguments)
            made.inputs.push_back(argument);
        break;
    case ir::Operation::Store:
        made.opcode = ghidra::CPUI_STORE;
        made.writes = false;
        made.inputs.push_back(constant(static_cast<uint64_t>(instruction.space), 4));
        for (const Varnode &argument : arguments)
            made.inputs.push_back(argument);
        break;

    case ir::Operation::FrameAddress:
    case ir::Operation::GlobalAddress:
    case ir::Operation::FunctionAddress:
        // An address is a number until a frame and an image exist, and both are
        // decided after this. Copying the number is what it means until then.
        made.opcode = ghidra::CPUI_COPY;
        made.inputs.push_back(
            constant(instruction.immediate,
                     target_.pointer_bytes > 0 ? target_.pointer_bytes : width));
        break;

    case ir::Operation::Call:
        made.opcode = ghidra::CPUI_CALL;
        made.inputs = arguments;
        break;
    case ir::Operation::Return:
        made.opcode = ghidra::CPUI_RETURN;
        made.writes = false;
        made.inputs = arguments;
        break;
    case ir::Operation::Jump:
        made.opcode = ghidra::CPUI_BRANCH;
        made.writes = false;
        made.successors = instruction.successors;
        break;
    case ir::Operation::Branch:
        // A conditional branch names where it goes and then what decides it.
        made.opcode = ghidra::CPUI_CBRANCH;
        made.writes = false;
        made.inputs = arguments;
        made.successors = instruction.successors;
        break;
    case ir::Operation::Switch:
        made.opcode = ghidra::CPUI_BRANCHIND;
        made.writes = false;
        made.inputs = arguments;
        made.successors = instruction.successors;
        break;

    case ir::Operation::Phi:
        made.opcode = ghidra::CPUI_MULTIEQUAL;
        made.inputs = arguments;
        break;

    case ir::Operation::Raw:
        // Bytes that were already instructions have no p-code spelling here:
        // what they do is whatever the processor does with them, which is a
        // question for the specification and not for this.
        complain("a body that is already instructions cannot be written as p-code");
        return false;
    }

    into.operations.push_back(std::move(made));
    return true;
}

bool Writer::write(const ir::Function &function, Sequence &out)
{
    out.name = function.name;
    out.entry = function.entry;

    // A parameter is already somewhere before anything runs, so its place is
    // decided before the body is written and not when it is first read.
    for (const ir::Value &parameter : function.parameters)
        place(parameter, 0);

    const size_t before = problems_.size();
    for (const ir::Block &block : function.blocks) {
        Block written;
        written.identifier = block.identifier;
        for (const ir::Instruction &instruction : block.instructions) {
            if (!operation(instruction, written))
                return false;
        }
        out.blocks.push_back(std::move(written));
    }
    return problems_.size() == before;
}

std::string where_name(Where where)
{
    switch (where) {
    case Where::Constant: return "const";
    case Where::Register: return "register";
    case Where::Unique: return "unique";
    case Where::Memory: return "ram";
    case Where::Frame: return "frame";
    }
    return "?";
}

std::string varnode_text(const Varnode &node)
{
    std::ostringstream out;
    out << "(" << where_name(node.where) << ", 0x" << std::hex << node.offset << std::dec << ", "
        << node.size << ")";
    return out.str();
}

} // namespace

const char *opcode_name(ghidra::OpCode opcode) { return ghidra::get_opname(opcode); }

bool to_pcode(const ir::Function &function, const ir::Target &target, Sequence &out,
              std::vector<std::string> &problems)
{
    Writer writer(target, problems);
    return writer.write(function, out);
}

std::string to_text(const Sequence &sequence)
{
    std::ostringstream out;
    out << "pcode " << sequence.name << "\n";
    for (const Block &block : sequence.blocks) {
        out << "block " << block.identifier;
        if (block.identifier == sequence.entry)
            out << " (entry)";
        out << "\n";
        for (const Operation &operation : block.operations) {
            out << "  ";
            if (operation.writes)
                out << varnode_text(operation.output) << " = ";
            out << opcode_name(operation.opcode);
            for (const Varnode &input : operation.inputs)
                out << " " << varnode_text(input);
            for (uint32_t successor : operation.successors)
                out << " -> " << successor;
            out << "\n";
        }
    }
    return out.str();
}


// ------------------------------------------------------------------ running

namespace {

// A value cut down to the width it is held in. p-code is explicit about width
// everywhere, and an operation that works in four bytes must not answer with
// eight; the truncation is the operation, not an afterthought.
uint64_t narrowed(uint64_t value, int size)
{
    if (size <= 0 || size >= 8)
        return value;
    const uint64_t keep = (static_cast<uint64_t>(1) << (size * 8)) - 1;
    return value & keep;
}

// The same value read as signed, which is what the signed operations mean by
// it. Widening the top bit is what makes -1 in four bytes still -1 in eight.
int64_t as_signed(uint64_t value, int size)
{
    if (size <= 0 || size >= 8)
        return static_cast<int64_t>(value);
    const uint64_t sign = static_cast<uint64_t>(1) << (size * 8 - 1);
    if ((value & sign) == 0)
        return static_cast<int64_t>(value);
    const uint64_t ones = ~((static_cast<uint64_t>(1) << (size * 8)) - 1);
    return static_cast<int64_t>(value | ones);
}

// Everywhere a value can be while this runs. Memory is kept by address rather
// than as a block, because a function under test touches a handful of places
// and reserving a program's worth of memory to hold them would say nothing.
class Running {
public:
    explicit Running(const Machine &machine) : machine_(machine)
    {
        for (const auto &entry : machine.registers)
            registers_[entry.first] = entry.second;
    }

    uint64_t read(const Varnode &node) const
    {
        if (node.is_constant())
            return narrowed(node.offset, node.size);
        const std::map<uint64_t, uint64_t> &from = store_for(node.where);
        auto found = from.find(node.offset);
        return narrowed(found == from.end() ? 0 : found->second, node.size);
    }

    void write(const Varnode &node, uint64_t value)
    {
        if (node.is_constant())
            return;
        store_for(node.where)[node.offset] = narrowed(value, node.size);
    }

private:
    const std::map<uint64_t, uint64_t> &store_for(Where where) const
    {
        switch (where) {
        case Where::Register: return registers_;
        case Where::Unique: return uniques_;
        case Where::Frame: return frame_;
        default: return memory_;
        }
    }
    std::map<uint64_t, uint64_t> &store_for(Where where)
    {
        return const_cast<std::map<uint64_t, uint64_t> &>(
            static_cast<const Running *>(this)->store_for(where));
    }

    const Machine &machine_;
    std::map<uint64_t, uint64_t> registers_;
    std::map<uint64_t, uint64_t> uniques_;
    std::map<uint64_t, uint64_t> memory_;
    std::map<uint64_t, uint64_t> frame_;
};

const Block *block_named(const Sequence &sequence, uint32_t identifier)
{
    for (const Block &block : sequence.blocks) {
        if (block.identifier == identifier)
            return &block;
    }
    return nullptr;
}

} // namespace

Answer run(const Sequence &sequence, const Machine &machine)
{
    Answer answer;
    Running running(machine);

    const Block *block = block_named(sequence, sequence.entry);
    if (block == nullptr) {
        answer.error = "there is no entry block to start from";
        return answer;
    }

    while (block != nullptr) {
        const Block *next = nullptr;
        for (const Operation &operation : block->operations) {
            if (++answer.steps > machine.budget) {
                answer.error = "it ran longer than it was allowed to, so it does not finish";
                return answer;
            }

            auto input = [&](size_t which) -> uint64_t {
                return which < operation.inputs.size() ? running.read(operation.inputs[which]) : 0;
            };
            auto width = [&](size_t which) -> int {
                return which < operation.inputs.size() ? operation.inputs[which].size : 8;
            };

            const int out_size = operation.writes ? operation.output.size : 8;
            uint64_t made = 0;
            bool wrote = true;

            switch (operation.opcode) {
            case ghidra::CPUI_COPY: made = input(0); break;
            case ghidra::CPUI_INT_ADD: made = input(0) + input(1); break;
            case ghidra::CPUI_INT_SUB: made = input(0) - input(1); break;
            case ghidra::CPUI_INT_MULT: made = input(0) * input(1); break;
            case ghidra::CPUI_INT_DIV:
                if (input(1) == 0) {
                    answer.error = "it divided by nothing";
                    return answer;
                }
                made = input(0) / input(1);
                break;
            case ghidra::CPUI_INT_SDIV: {
                const int64_t divisor = as_signed(input(1), width(1));
                if (divisor == 0) {
                    answer.error = "it divided by nothing";
                    return answer;
                }
                made = static_cast<uint64_t>(as_signed(input(0), width(0)) / divisor);
                break;
            }
            case ghidra::CPUI_INT_REM:
                if (input(1) == 0) {
                    answer.error = "it took a remainder by nothing";
                    return answer;
                }
                made = input(0) % input(1);
                break;
            case ghidra::CPUI_INT_SREM: {
                const int64_t divisor = as_signed(input(1), width(1));
                if (divisor == 0) {
                    answer.error = "it took a remainder by nothing";
                    return answer;
                }
                made = static_cast<uint64_t>(as_signed(input(0), width(0)) % divisor);
                break;
            }
            case ghidra::CPUI_INT_AND: made = input(0) & input(1); break;
            case ghidra::CPUI_INT_OR: made = input(0) | input(1); break;
            case ghidra::CPUI_INT_XOR: made = input(0) ^ input(1); break;
            case ghidra::CPUI_INT_LEFT: made = input(0) << (input(1) & 63); break;
            case ghidra::CPUI_INT_RIGHT: made = input(0) >> (input(1) & 63); break;
            case ghidra::CPUI_INT_SRIGHT:
                made = static_cast<uint64_t>(as_signed(input(0), width(0)) >>
                                             (input(1) & 63));
                break;
            case ghidra::CPUI_INT_2COMP: made = ~input(0) + 1; break;
            case ghidra::CPUI_INT_NEGATE: made = ~input(0); break;
            case ghidra::CPUI_INT_EQUAL: made = input(0) == input(1) ? 1 : 0; break;
            case ghidra::CPUI_INT_NOTEQUAL: made = input(0) != input(1) ? 1 : 0; break;
            case ghidra::CPUI_INT_LESS: made = input(0) < input(1) ? 1 : 0; break;
            case ghidra::CPUI_INT_LESSEQUAL: made = input(0) <= input(1) ? 1 : 0; break;
            case ghidra::CPUI_INT_SLESS:
                made = as_signed(input(0), width(0)) < as_signed(input(1), width(1)) ? 1 : 0;
                break;
            case ghidra::CPUI_INT_SLESSEQUAL:
                made = as_signed(input(0), width(0)) <= as_signed(input(1), width(1)) ? 1 : 0;
                break;
            case ghidra::CPUI_INT_ZEXT: made = input(0); break;
            case ghidra::CPUI_INT_SEXT:
                made = static_cast<uint64_t>(as_signed(input(0), width(0)));
                break;
            case ghidra::CPUI_SUBPIECE: made = input(0) >> (input(1) * 8); break;

            case ghidra::CPUI_LOAD:
                // The space is the first input and the address the second.
                made = running.read(Varnode{Where::Memory, input(1), out_size});
                break;
            case ghidra::CPUI_STORE:
                running.write(Varnode{Where::Memory, input(1), width(2)}, input(2));
                wrote = false;
                break;

            case ghidra::CPUI_BRANCH:
                if (operation.successors.empty()) {
                    answer.error = "a branch with nowhere to go";
                    return answer;
                }
                next = block_named(sequence, operation.successors.front());
                wrote = false;
                break;
            case ghidra::CPUI_CBRANCH:
                if (operation.successors.size() < 2) {
                    answer.error = "a conditional branch with only one way out";
                    return answer;
                }
                next = block_named(sequence,
                                   input(0) != 0 ? operation.successors[0]
                                                 : operation.successors[1]);
                wrote = false;
                break;
            case ghidra::CPUI_RETURN:
                answer.ok = true;
                answer.returned = true;
                answer.value = operation.inputs.empty() ? 0 : input(0);
                return answer;

            default:
                answer.error = std::string("nothing here knows how to run a ") +
                               opcode_name(operation.opcode);
                return answer;
            }

            if (wrote && operation.writes)
                running.write(operation.output, narrowed(made, out_size));
        }

        if (next == nullptr) {
            answer.error = "a block ran off its end without leaving";
            return answer;
        }
        block = next;
    }

    answer.error = "it went somewhere that is not a block";
    return answer;
}

} // namespace pcode
} // namespace nova
} // namespace astral_internal
