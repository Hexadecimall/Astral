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

    // Moves the stack by `bytes`, the way this processor's frame grows.
    bool move_stack(uint64_t bytes, bool giving_back, Block &into);

    const ir::Target &target_;
    std::vector<std::string> &problems_;
    std::map<uint32_t, Varnode> placed_;
    uint64_t next_unique_ = 0;
    // Where this processor keeps its frame, read once for the whole function.
    ir::Target::Frame frame_;
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

bool Writer::move_stack(uint64_t bytes, bool giving_back, Block &into)
{
    if (!frame_.known) {
        complain("this function needs a frame and this processor has no stack");
        return false;
    }
    const ir::Target::RegisterPlace *pointer = target_.register_place(frame_.pointer);
    if (pointer == nullptr) {
        complain("this processor's stack register " + frame_.pointer + " has no place");
        return false;
    }

    Varnode stack;
    stack.where = Where::Register;
    stack.offset = pointer->offset;
    stack.size = pointer->width;

    // Taking room on a downward frame is subtracting, and giving it back is
    // adding; on a processor whose frame grows the other way it is the other
    // way round, which is why the specification is asked which it is.
    const bool subtracting = frame_.grows_downward != giving_back;

    Operation made;
    made.opcode = subtracting ? ghidra::CPUI_INT_SUB : ghidra::CPUI_INT_ADD;
    made.writes = true;
    made.output = stack;
    made.inputs.push_back(stack);
    made.inputs.push_back(constant(bytes, pointer->width));
    into.operations.push_back(std::move(made));
    return true;
}

bool Writer::operation(const ir::Instruction &instruction, Block &into)
{
    const int width = ir::width_of(instruction, target_);
    const bool is_signed = instruction.is_signed;

    // A truth is one byte, whatever it was worked out from.
    //
    // The width of an operation is the width of the things it works on, and for
    // a comparison that is not the width of its answer: comparing two eight-byte
    // values asks an eight-byte question and gets a yes or a no. p-code says so
    // - every processor's own comparisons decode with a one-byte output - and
    // giving the answer the operands' width asks for somewhere eight bytes wide
    // to keep a yes in, which a thirty-two bit processor has not got.
    //
    // It has to be settled here rather than after the answer is made, because a
    // value is placed once and remembered: narrowing what was emitted would
    // leave everything that reads it still asking for the wider one, which is a
    // different value entirely.
    const bool answers_yes_or_no = instruction.operation == ir::Operation::Equal ||
                                   instruction.operation == ir::Operation::NotEqual ||
                                   instruction.operation == ir::Operation::Less ||
                                   instruction.operation == ir::Operation::LessOrEqual;

    Operation made;
    made.writes = instruction.result.is_valid();
    if (made.writes)
        made.output = place(instruction.result, answers_yes_or_no ? 1 : width);

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
        made.inputs.push_back(constant(static_cast<uint64_t>(ir::Space::Data), 4));
        for (const Varnode &argument : arguments)
            made.inputs.push_back(argument);
        break;
    case ir::Operation::Store:
        made.opcode = ghidra::CPUI_STORE;
        made.writes = false;
        made.inputs.push_back(constant(static_cast<uint64_t>(ir::Space::Data), 4));
        for (const Varnode &argument : arguments)
            made.inputs.push_back(argument);
        break;

    case ir::Operation::FrameAddress: {
        // A frame offset is not an address. It is so many bytes from wherever
        // this function's frame begins, and where that is, is in a register the
        // processor names - so the address is that register plus the offset.
        // This is the step where a place in a frame becomes somewhere real.
        if (!frame_.known) {
            complain("this processor has no frame to take an address in");
            return false;
        }
        const ir::Target::RegisterPlace *pointer = target_.register_place(frame_.pointer);
        if (pointer == nullptr) {
            complain("this processor's stack register " + frame_.pointer + " has no place");
            return false;
        }
        Varnode base;
        base.where = Where::Register;
        base.offset = pointer->offset;
        base.size = pointer->width;

        made.opcode = ghidra::CPUI_INT_ADD;
        made.inputs.push_back(base);
        made.inputs.push_back(constant(instruction.immediate, pointer->width));
        if (made.writes)
            made.output.size = pointer->width;
        break;
    }
    case ir::Operation::GlobalAddress:
    case ir::Operation::FunctionAddress:
        // An address in the image is a number until the image is laid out,
        // which is decided after this.
        made.opcode = ghidra::CPUI_COPY;
        made.inputs.push_back(
            constant(instruction.immediate,
                     target_.pointer_bytes > 0 ? target_.pointer_bytes : width));
        break;

    case ir::Operation::Call:
        // A call names where it goes and nothing else.
        //
        // The arguments were put where the callee will look for them before
        // this ran - that is what a calling convention is, and the copies that
        // do it are already in the block above. Handing them to the call as
        // well asked every processor for a call instruction that takes as many
        // operands as the function takes arguments, and no processor has one:
        // a call names an address, and that is all.
        //
        // Where the callee is, is not known until the image is laid out, so
        // what is written now is nothing and the name is carried instead.
        made.opcode = ghidra::CPUI_CALL;
        made.inputs.push_back(
            constant(0, target_.pointer_bytes > 0 ? target_.pointer_bytes : width));
        made.callee = instruction.callee;
        break;
    case ir::Operation::Return:
        // Nothing is read to leave.
        //
        // The representation says what a function answers with, because that is
        // what the function means. A processor's return instruction says none of
        // it: the answer was put in its agreed place by the copy just before
        // this, and what is left to do is go back, which reads the register the
        // return address is in - a register the instruction names itself rather
        // than takes as an operand. Handing it the answer as an input asked
        // every processor for a return that takes one, and none has one.
        made.opcode = ghidra::CPUI_RETURN;
        made.writes = false;
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

    // What an operation works on has to be all one width.
    //
    // p-code says so: adding a four-byte value to an eight-byte one is not an
    // operation, it is two operations with the widening left out. It arises
    // because width belongs to the instruction here and not to the value, and a
    // value is placed once - so one made by a four-byte instruction and read by
    // an eight-byte one came back four bytes wide, and what was written was an
    // eight-byte add over an eight-byte and a four-byte operand.
    //
    // The widening that was left out is put in. Which one it is, is the reading
    // instruction's business, because that is what knows whether the value it is
    // reading is a signed one.
    switch (made.opcode) {
    case ghidra::CPUI_INT_ADD:
    case ghidra::CPUI_INT_SUB:
    case ghidra::CPUI_INT_MULT:
    case ghidra::CPUI_INT_DIV:
    case ghidra::CPUI_INT_SDIV:
    case ghidra::CPUI_INT_REM:
    case ghidra::CPUI_INT_SREM:
    case ghidra::CPUI_INT_AND:
    case ghidra::CPUI_INT_OR:
    case ghidra::CPUI_INT_XOR:
    case ghidra::CPUI_INT_EQUAL:
    case ghidra::CPUI_INT_NOTEQUAL:
    case ghidra::CPUI_INT_LESS:
    case ghidra::CPUI_INT_SLESS:
    case ghidra::CPUI_INT_LESSEQUAL:
    case ghidra::CPUI_INT_SLESSEQUAL: {
        // A comparison answers in one byte, so its width is the width of what it
        // asks about rather than of its answer.
        const bool answers_yes_or_no = made.output.size == 1 && made.writes &&
                                       made.opcode >= ghidra::CPUI_INT_EQUAL &&
                                       made.opcode <= ghidra::CPUI_INT_SLESSEQUAL;
        int want = made.writes && !answers_yes_or_no ? made.output.size : 0;
        for (const Varnode &input : made.inputs) {
            if (!input.is_constant() && input.size > want)
                want = input.size;
        }
        if (want <= 0)
            break;
        for (Varnode &input : made.inputs) {
            if (input.size == want)
                continue;
            if (input.is_constant()) {
                // A number has no place of its own, so it is simply asked for at
                // the width it is being read at.
                input.size = want;
                continue;
            }
            Varnode wider;
            wider.where = Where::Unique;
            wider.offset = next_unique_;
            wider.size = want;
            next_unique_ += static_cast<uint64_t>(want);

            Operation widening;
            widening.opcode =
                input.size < want ? signed_or_not(is_signed, ghidra::CPUI_INT_SEXT,
                                                  ghidra::CPUI_INT_ZEXT)
                                  : ghidra::CPUI_SUBPIECE;
            widening.writes = true;
            widening.output = wider;
            widening.inputs.push_back(input);
            if (widening.opcode == ghidra::CPUI_SUBPIECE)
                widening.inputs.push_back(constant(0, 4));
            into.operations.push_back(std::move(widening));
            input = wider;
        }

        // And where the answer goes is that width too. An operation whose
        // operands are wider than the place its answer was going works at the
        // wider width and the answer is narrowed afterwards, which is what the
        // narrowing is for and is again a thing p-code says outright rather
        // than leaves to be inferred.
        if (made.writes && !answers_yes_or_no && made.output.size != want) {
            Varnode room;
            room.where = Where::Unique;
            room.offset = next_unique_;
            room.size = want;
            next_unique_ += static_cast<uint64_t>(want);

            Operation narrowing;
            narrowing.opcode = made.output.size < want
                                   ? ghidra::CPUI_SUBPIECE
                                   : signed_or_not(is_signed, ghidra::CPUI_INT_SEXT,
                                                   ghidra::CPUI_INT_ZEXT);
            narrowing.writes = true;
            narrowing.output = made.output;
            narrowing.inputs.push_back(room);
            if (narrowing.opcode == ghidra::CPUI_SUBPIECE)
                narrowing.inputs.push_back(constant(0, 4));

            made.output = room;
            into.operations.push_back(std::move(made));
            into.operations.push_back(std::move(narrowing));
            return true;
        }
        break;
    }
    default:
        break;
    }

    into.operations.push_back(std::move(made));
    return true;
}

bool Writer::write(const ir::Function &function, Sequence &out)
{
    out.name = function.name;
    out.entry = function.entry;

    // Where the frame is measured from, read once rather than at every slot.
    // A function that never touches its frame does not need one, so a processor
    // with no stack is only a problem for a function that wanted a frame.
    std::string unused;
    target_.frame(frame_, unused);

    // Where this function will have left its answer by the time it goes back.
    //
    // A processor's return instruction carries no value. It reads the register
    // the return address is in and goes there, and a caller finds an answer
    // only because the convention agreed beforehand where one would be sitting.
    // So the place is settled once, here, and every way out puts the answer in
    // it before leaving.
    for (const ir::Block &block : function.blocks) {
        bool found = false;
        for (const ir::Instruction &instruction : block.instructions) {
            if (instruction.operation != ir::Operation::Return || instruction.arguments.empty())
                continue;
            std::vector<Storage> nowhere;
            Storage answer;
            std::string trouble;
            const int wide = instruction.width > 0 ? instruction.width : target_.word_bytes;
            if (!target_.calling_convention({}, wide, nowhere, answer, trouble)) {
                complain("this processor's specification does not say where a function leaves "
                         "its answer: " + trouble);
                return false;
            }
            if (answer.kind == Storage::Kind::Register) {
                const ir::Target::RegisterPlace *where =
                    target_.register_place(answer.register_name);
                if (where != nullptr) {
                    out.answer.where = Where::Register;
                    out.answer.offset = where->offset;
                    out.answer.size = where->width;
                }
            }
            found = true;
            break;
        }
        if (found)
            break;
    }

    // A parameter is already somewhere before anything runs, so its place is
    // decided before the body is written and not when it is first read.
    for (const ir::Value &parameter : function.parameters)
        place(parameter, 0);

    const size_t before = problems_.size();
    for (const ir::Block &block : function.blocks) {
        Block written;
        written.identifier = block.identifier;

        // Taking the room the locals need, before anything reaches for it.
        if (block.identifier == function.entry && function.frame_bytes != 0) {
            if (!move_stack(function.frame_bytes, false, written))
                return false;
        }

        for (const ir::Instruction &instruction : block.instructions) {
            if (instruction.operation == ir::Operation::Return) {
                // The answer goes where the caller will look, before anything
                // else about leaving happens - while the frame is still there,
                // since the answer may be in it.
                if (!instruction.arguments.empty() && out.answer.size != 0) {
                    Operation put;
                    put.opcode = ghidra::CPUI_COPY;
                    put.writes = true;
                    put.output = out.answer;
                    put.inputs.push_back(place(instruction.arguments.front(), out.answer.size));
                    written.operations.push_back(std::move(put));
                }

                // And giving the room back before leaving, at every way out
                // rather than at one of them: a function with two returns gives
                // it back twice or not at all, and not at all is a stack that
                // never comes back.
                if (function.frame_bytes != 0 && !move_stack(function.frame_bytes, true, written))
                    return false;
            }
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

namespace {

// Carrying a value in two registers, when one will not hold it.
class Splitter {
public:
    Splitter(Sequence &sequence, const ir::Target &target, int half,
             std::vector<std::string> &problems)
        : sequence_(sequence), target_(target), half_(half), problems_(problems)
    {
        // Somewhere to put the halves that nothing else is using. Values with no
        // home are told apart by where they sit, so a new one sits past
        // everything already there.
        for (const Block &block : sequence_.blocks) {
            for (const Operation &operation : block.operations) {
                auto past = [&](const Varnode &node) {
                    if (node.where != Where::Unique)
                        return;
                    const uint64_t ends = node.offset + static_cast<uint64_t>(node.size);
                    if (ends > next_)
                        next_ = ends;
                };
                if (operation.writes)
                    past(operation.output);
                for (const Varnode &input : operation.inputs)
                    past(input);
            }
        }
    }

    bool run();

private:
    struct Pair {
        Varnode low;
        Varnode high;
    };

    bool too_wide(const Varnode &node) const
    {
        return node.size > half_ &&
               (node.where == Where::Unique || node.where == Where::Register);
    }

    Varnode fresh(int size)
    {
        Varnode node;
        node.where = Where::Unique;
        node.offset = next_;
        node.size = size;
        next_ += static_cast<uint64_t>(half_);
        return node;
    }

    Varnode number(uint64_t value, int size) const
    {
        Varnode node;
        node.where = Where::Constant;
        node.offset = value;
        node.size = size;
        return node;
    }

    // The two halves a wide value is carried in, remembered so that every
    // mention of it means the same two.
    Pair halves_of(const Varnode &node)
    {
        const std::pair<uint64_t, int> key{node.offset, node.size};
        auto already = known_.find(key);
        if (already != known_.end())
            return already->second;
        Pair pair;
        pair.low = fresh(half_);
        pair.high = fresh(half_);
        known_.emplace(key, pair);
        return pair;
    }

    void emit(ghidra::OpCode opcode, const Varnode &into, const std::vector<Varnode> &from)
    {
        Operation made;
        made.opcode = opcode;
        made.writes = true;
        made.output = into;
        made.inputs = from;
        out_.push_back(std::move(made));
    }

    bool split(const Operation &operation);
    bool split_memory(const Operation &operation);

    Sequence &sequence_;
    const ir::Target &target_;
    const int half_;
    std::vector<std::string> &problems_;
    uint64_t next_ = 0;
    std::map<std::pair<uint64_t, int>, Pair> known_;
    std::vector<Operation> out_;
};

// A load or a store goes twice, at addresses a half apart. Which half sits at
// the lower address is the processor's business: one that writes its biggest
// byte first puts the high half there.
bool Splitter::split_memory(const Operation &operation)
{
    const bool storing = operation.opcode == ghidra::CPUI_STORE;
    if (operation.inputs.size() < (storing ? 3u : 2u))
        return false;

    const Varnode space = operation.inputs[0];
    const Varnode address = operation.inputs[1];
    if (too_wide(address))
        return false;  // an address that itself needs two is not done here

    const Varnode far = fresh(address.size);
    emit(ghidra::CPUI_INT_ADD, far,
         {address, number(static_cast<uint64_t>(half_), address.size)});

    const Varnode first = target_.data_big_endian ? address : far;
    const Varnode second = target_.data_big_endian ? far : address;

    if (storing) {
        if (!too_wide(operation.inputs[2]))
            return false;
        const Pair value = halves_of(operation.inputs[2]);
        for (int which = 0; which < 2; ++which) {
            Operation made;
            made.opcode = ghidra::CPUI_STORE;
            made.writes = false;
            made.inputs = {space, which == 0 ? first : second,
                           which == 0 ? value.high : value.low};
            out_.push_back(std::move(made));
        }
        return true;
    }

    const Pair into = halves_of(operation.output);
    emit(ghidra::CPUI_LOAD, into.high, {space, first});
    emit(ghidra::CPUI_LOAD, into.low, {space, second});
    return true;
}

bool Splitter::split(const Operation &operation)
{
    switch (operation.opcode) {
    case ghidra::CPUI_COPY: {
        if (operation.inputs.size() != 1 || !operation.writes)
            return false;
        const Pair into = halves_of(operation.output);
        const Varnode from = operation.inputs[0];
        if (from.is_constant()) {
            // A number too wide to carry is two numbers: the low half, and what
            // was above it.
            const uint64_t mask = half_ >= 8 ? ~static_cast<uint64_t>(0)
                                             : (static_cast<uint64_t>(1) << (half_ * 8)) - 1;
            emit(ghidra::CPUI_COPY, into.low, {number(from.offset & mask, half_)});
            emit(ghidra::CPUI_COPY, into.high,
                 {number(half_ >= 8 ? 0 : (from.offset >> (half_ * 8)) & mask, half_)});
            return true;
        }
        if (!too_wide(from))
            return false;
        const Pair source = halves_of(from);
        emit(ghidra::CPUI_COPY, into.low, {source.low});
        emit(ghidra::CPUI_COPY, into.high, {source.high});
        return true;
    }

    case ghidra::CPUI_INT_ADD:
    case ghidra::CPUI_INT_SUB: {
        if (operation.inputs.size() != 2 || !operation.writes)
            return false;
        if (!too_wide(operation.inputs[0]) || !too_wide(operation.inputs[1]))
            return false;
        const bool adding = operation.opcode == ghidra::CPUI_INT_ADD;
        const Pair left = halves_of(operation.inputs[0]);
        const Pair right = halves_of(operation.inputs[1]);
        const Pair into = halves_of(operation.output);

        // The low half, and then what it carried into the high one. What comes
        // out of an addition is what the processor calls a carry; what comes out
        // of a subtraction is the low half of the first being below the low half
        // of the second, which is that same question asked the way p-code asks
        // it.
        emit(operation.opcode, into.low, {left.low, right.low});
        const Varnode said = fresh(1);
        emit(adding ? ghidra::CPUI_INT_CARRY : ghidra::CPUI_INT_LESS, said,
             {left.low, right.low});
        const Varnode carried = fresh(half_);
        emit(ghidra::CPUI_INT_ZEXT, carried, {said});
        const Varnode tops = fresh(half_);
        emit(operation.opcode, tops, {left.high, right.high});
        emit(operation.opcode, into.high, {tops, carried});
        return true;
    }

    case ghidra::CPUI_INT_AND:
    case ghidra::CPUI_INT_OR:
    case ghidra::CPUI_INT_XOR: {
        if (operation.inputs.size() != 2 || !operation.writes)
            return false;
        if (!too_wide(operation.inputs[0]) || !too_wide(operation.inputs[1]))
            return false;
        const Pair left = halves_of(operation.inputs[0]);
        const Pair right = halves_of(operation.inputs[1]);
        const Pair into = halves_of(operation.output);
        emit(operation.opcode, into.low, {left.low, right.low});
        emit(operation.opcode, into.high, {left.high, right.high});
        return true;
    }

    case ghidra::CPUI_INT_EQUAL:
    case ghidra::CPUI_INT_NOTEQUAL: {
        if (operation.inputs.size() != 2 || !operation.writes)
            return false;
        if (!too_wide(operation.inputs[0]) || !too_wide(operation.inputs[1]))
            return false;
        const Pair left = halves_of(operation.inputs[0]);
        const Pair right = halves_of(operation.inputs[1]);

        // Two long values are equal when both halves are, and differ when
        // either half does.
        const Varnode low_says = fresh(1);
        const Varnode high_says = fresh(1);
        emit(operation.opcode, low_says, {left.low, right.low});
        emit(operation.opcode, high_says, {left.high, right.high});
        emit(operation.opcode == ghidra::CPUI_INT_EQUAL ? ghidra::CPUI_BOOL_AND
                                                        : ghidra::CPUI_BOOL_OR,
             operation.output, {low_says, high_says});
        return true;
    }

    case ghidra::CPUI_INT_ZEXT:
    case ghidra::CPUI_INT_SEXT: {
        if (operation.inputs.size() != 1 || !operation.writes ||
            too_wide(operation.inputs[0]) || operation.inputs[0].size > half_)
            return false;
        const Pair into = halves_of(operation.output);
        emit(ghidra::CPUI_COPY, into.low, {operation.inputs[0]});
        if (operation.opcode == ghidra::CPUI_INT_ZEXT) {
            emit(ghidra::CPUI_COPY, into.high, {number(0, half_)});
            return true;
        }
        // Widening a signed value fills the high half with its sign, which is
        // the value shifted down until only that is left.
        emit(ghidra::CPUI_INT_SRIGHT, into.high,
             {operation.inputs[0], number(static_cast<uint64_t>(half_ * 8 - 1), 4)});
        return true;
    }

    case ghidra::CPUI_SUBPIECE: {
        // Taking the low bytes of a long value is taking the half they are in.
        if (operation.inputs.size() != 2 || !operation.writes ||
            !operation.inputs[1].is_constant() || operation.inputs[1].offset != 0 ||
            operation.output.size > half_ || !too_wide(operation.inputs[0]))
            return false;
        const Pair from = halves_of(operation.inputs[0]);
        emit(ghidra::CPUI_COPY, operation.output, {from.low});
        return true;
    }

    case ghidra::CPUI_LOAD:
    case ghidra::CPUI_STORE:
        return split_memory(operation);

    default:
        return false;
    }
}

bool Splitter::run()
{
    const size_t before = problems_.size();
    for (Block &block : sequence_.blocks) {
        out_.clear();
        for (const Operation &operation : block.operations) {
            bool wide = operation.writes && too_wide(operation.output);
            for (const Varnode &input : operation.inputs)
                wide = wide || too_wide(input);
            if (!wide) {
                out_.push_back(operation);
                continue;
            }
            if (split(operation))
                continue;
            const int how_wide =
                operation.writes && operation.output.size > half_ ? operation.output.size : 0;
            problems_.push_back(
                std::string("a value wider than any register this processor has cannot be ") +
                opcode_name(operation.opcode) + "ed a half at a time here" +
                (how_wide > 0 ? " (" + std::to_string(how_wide) + " bytes)" : ""));
            out_.push_back(operation);
        }
        block.operations = out_;
    }
    return problems_.size() == before;
}

} // namespace

bool split_wide(Sequence &sequence, const ir::Target &target, int widest,
                std::vector<std::string> &problems)
{
    if (widest <= 0)
        return true;  // nothing known about its registers, so nothing to say

    bool any_wider = false;
    for (const Block &block : sequence.blocks) {
        for (const Operation &operation : block.operations) {
            if (operation.writes && operation.output.size > widest &&
                operation.output.where != Where::Constant)
                any_wider = true;
            for (const Varnode &input : operation.inputs) {
                if (input.size > widest && input.where != Where::Constant)
                    any_wider = true;
            }
        }
    }
    if (!any_wider)
        return true;

    Splitter splitter(sequence, target, widest, problems);
    return splitter.run();
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
            if (!operation.callee.empty())
                out << " " << operation.callee;
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

    const std::map<uint64_t, uint64_t> &registers() const { return registers_; }
    void take_registers(const std::map<uint64_t, uint64_t> &from) { registers_ = from; }

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
            case ghidra::CPUI_CALL: {
                // A call runs the other function over the same registers, which
                // is what a call is: the arguments were put where the callee
                // will look for them before this ran.
                auto found = machine.others.find(operation.callee);
                if (found == machine.others.end() || found->second == nullptr) {
                    answer.error = "there is nothing here called " + operation.callee;
                    return answer;
                }
                Machine inner;
                inner.others = machine.others;
                inner.budget = machine.budget > answer.steps ? machine.budget - answer.steps : 0;
                inner.registers = running.registers();
                const Answer came_back = run(*found->second, inner);
                answer.steps += came_back.steps;
                if (!came_back.ok) {
                    answer.error = "in " + operation.callee + ": " + came_back.error;
                    return answer;
                }
                // What it answered with goes where the caller is looking, and
                // what it left in the registers stays there, the way a real
                // call leaves them.
                running.take_registers(came_back.registers);
                made = came_back.value;
                break;
            }
            case ghidra::CPUI_RETURN:
                // Leaving reads nothing. What the function answered with is
                // wherever it agreed to leave it, which the sequence says.
                answer.ok = true;
                answer.returned = true;
                answer.value = sequence.answer.size != 0 ? running.read(sequence.answer) : 0;
                answer.registers = running.registers();
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
