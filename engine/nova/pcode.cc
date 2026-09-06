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

} // namespace pcode
} // namespace nova
} // namespace astral_internal
