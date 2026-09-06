#include "catalogue.hh"

#include "semantics.hh"
#include "translate.hh"
#include "sleighbase.hh"
#include "slghpattern.hh"
#include "slghpatexpress.hh"
#include "slghsymbol.hh"
#include "specification.hh"

#include <sstream>

namespace astral_internal {
namespace nova {
namespace catalogue {

namespace {

// The subtable everything starts from. A specification builds instructions out
// of tables that refer to one another, and this is the one an instruction is
// read from first.
const char *const kRoot = "instruction";

// Operations a template holds that say nothing about what the instruction
// computes. A build is the specification stitching in another table, and a
// delay slot is a scheduling remark; neither is part of the meaning, and
// counting them would make almost every form look like it does several things.
bool is_bookkeeping(ghidra::OpCode opcode)
{
    return opcode == ghidra::CPUI_MULTIEQUAL || opcode == ghidra::CPUI_INDIRECT;
}

// What a value in a template refers to.
//
// A template says where a value is with three pieces - a space, an offset and a
// size - and each is either a number written in the specification or a
// reference to one of the form's own slots. A slot is what gets filled in when
// an instruction is actually written, so a value whose offset is a slot is
// "whatever register was put there".
Form::Piece read_piece(const ghidra::VarnodeTpl *value)
{
    Form::Piece piece;
    if (value == nullptr)
        return piece;

    const ghidra::ConstTpl &offset = value->getOffset();
    switch (offset.getType()) {
    case ghidra::ConstTpl::handle:
        piece.is_slot = true;
        piece.slot = offset.getHandleIndex();
        break;
    case ghidra::ConstTpl::real:
        piece.is_fixed = true;
        piece.fixed = offset.getReal();
        break;
    default:
        // Something worked out while decoding - where the instruction is, or
        // where it goes next. Neither is a slot to fill nor a fixed number, and
        // saying so is better than pretending it is one.
        break;
    }
    return piece;
}

// The bits a form always has, and how they are found.
//
// A form is chosen from a stream of bits by a tree: each node looks at a few
// bits and goes to the child they name, and a form sits at the bottom. So the
// bits a form insists on are every decision made on the way down to it, plus
// whatever the pattern at the bottom still says.
//
// They are not on the constructor. A constructor's own pattern is built while a
// specification is being compiled and is not carried in the result, so on a
// loaded specification it is always absent - and the pattern at the bottom of
// the tree holds only what was left to distinguish by then. Taking either alone
// says a form insists on almost nothing, which would make every form look like
// every other.
// A decision made on the way down: these bits, this many of them, had to be
// this. Kept as it was made rather than as a mask, because where those bits sit
// depends on how long the instruction is, and that is not known until the form
// at the bottom is reached.
struct Decided {
    int start = 0;
    int size = 0;
    uint64_t value = 0;
};

struct Constraint {
    std::vector<Decided> decisions;  // from the root down
    uint64_t leaf_mask = 0;          // and whatever the pattern at the bottom still said
    uint64_t leaf_bits = 0;
    // Whether getting here depended on something the processor already knew.
    bool from_context = false;
};

void walk(const ghidra::DecisionNode *node, Constraint sofar,
          std::map<const ghidra::Constructor *, Constraint> &found)
{
    if (node == nullptr)
        return;

    for (int i = 0; i < node->numPatterns(); ++i) {
        const ghidra::Constructor *made = node->getPatternConstructor(i);
        if (made == nullptr)
            continue;
        Constraint whole = sofar;
        if (const ghidra::DisjointPattern *pattern = node->getPattern(i)) {
            // Both counts are in bits, and only the first few are asked for on
            // purpose.
            //
            // A pattern at the bottom of the tree holds what was left to
            // distinguish once everything above it had been decided, and read
            // in full it disagrees with instructions that exist - it pins down
            // bits a real one does not have. What the way down decided is the
            // part that holds up, checked against MIPS and AARCH64 encodings
            // that are written down elsewhere, so that is what is used and this
            // contributes only where it is certain.
            whole.leaf_mask |= pattern->getMask(0, 4, false);
            whole.leaf_bits |= pattern->getValue(0, 4, false);
        }
        found.emplace(made, whole);
    }

    // A node deciding on context is deciding on what the processor already
    // knew rather than on what is written, so it adds nothing to the bits an
    // instruction has to have.
    if (node->isContextDecision()) {
        // Which way this goes is not written in the instruction, so it adds no
        // bits - but it does mean everything below it is only reached under
        // some setting, and that has to travel down or a form that exists only
        // in one mode looks like one that exists always.
        Constraint below = sofar;
        below.from_context = true;
        for (int i = 0; i < node->numChildren(); ++i)
            walk(node->getChild(i), below, found);
        return;
    }

    for (int i = 0; i < node->numChildren(); ++i) {
        Constraint below = sofar;
        Decided decision;
        decision.start = node->getStartBit();
        decision.size = node->getBitSize();
        decision.value = static_cast<uint64_t>(i);  // going to the i-th child is those bits being i
        if (decision.size > 0)
            below.decisions.push_back(decision);
        walk(node->getChild(i), below, found);
    }
}

// The decisions turned into the bits of an instruction that long.
//
// A bit is counted from the top of the instruction rather than the bottom. That
// is not a guess: counted from the bottom, no form at all agrees with an
// instruction anybody has seen, and counted from the top exactly one does,
// which is what it should be - one instruction means one form.
void settle(const Constraint &constraint, int length, uint64_t &mask, uint64_t &bits)
{
    mask = constraint.leaf_mask;
    bits = constraint.leaf_bits;
    if (length <= 0 || length > 8)
        return;

    const int width = length * 8;
    for (const Decided &decision : constraint.decisions) {
        const int shift = width - decision.start - decision.size;
        if (shift < 0 || decision.size <= 0 || decision.size >= 64)
            continue;
        const uint64_t here = ((static_cast<uint64_t>(1) << decision.size) - 1)
                              << static_cast<unsigned>(shift);
        mask |= here;
        bits |= (decision.value << static_cast<unsigned>(shift)) & here;
    }
    bits &= mask;
}

// Where an operand is written, and what can go there.
//
// An operand is a field of bits somewhere in the instruction. Which bits, is on
// the field; what the value there means, is on the symbol the operand is
// defined by - a list of registers picks one by number, and anything else is
// the number itself.
// Which registers a symbol can stand for, and the bits that pick each.
//
// A register operand is rarely a list of registers. It is usually a table, and
// often a table of tables: one constructor says "when these bits are so, this
// is whatever that other table says", and only at the bottom does something
// name an actual place. So this follows the forwarding down, carrying the bits
// each level insisted on, and records a register when it finally reaches one.
//
// `depth` stops a specification that refers to itself from doing so forever.
void resolve_registers(const ghidra::TripleSymbol *symbol, int length,
                       const std::map<uint64_t, std::string> &by_offset, uint64_t sofar_mask,
                       uint64_t sofar_bits, int depth, Form::Slot &into);

// A constructor's export, when it is an actual place rather than a forwarding.
// A place is a space and an offset, and the offset is what a register is.
bool exported_register(const ghidra::Constructor *made,
                       const std::map<uint64_t, std::string> &by_offset, std::string &named,
                       int &forwards_to)
{
    named.clear();
    forwards_to = -1;
    if (made == nullptr)
        return false;
    const ghidra::ConstructTpl *templ = made->getTempl();
    if (templ == nullptr)
        return false;
    const ghidra::HandleTpl *result = templ->getResult();
    if (result == nullptr)
        return false;

    const ghidra::ConstTpl &space = result->getSpace();
    const ghidra::ConstTpl &offset = result->getPtrOffset();

    // Whatever some other operand of this constructor says.
    if (space.getType() == ghidra::ConstTpl::handle) {
        forwards_to = space.getHandleIndex();
        return false;
    }
    if (space.getType() != ghidra::ConstTpl::spaceid ||
        offset.getType() != ghidra::ConstTpl::real)
        return false;

    auto found = by_offset.find(offset.getReal());
    if (found == by_offset.end())
        return false;
    named = found->second;
    return true;
}

void resolve_registers(const ghidra::TripleSymbol *symbol, int length,
                       const std::map<uint64_t, std::string> &by_offset, uint64_t sofar_mask,
                       uint64_t sofar_bits, int depth, Form::Slot &into)
{
    if (symbol == nullptr || depth > 4)
        return;

    // A list picks a register by the field's value directly.
    if (const ghidra::VarnodeListSymbol *list =
            dynamic_cast<const ghidra::VarnodeListSymbol *>(symbol)) {
        const ghidra::ValueSymbol *value = dynamic_cast<const ghidra::ValueSymbol *>(symbol);
        const ghidra::TokenField *field =
            value == nullptr ? nullptr
                             : dynamic_cast<const ghidra::TokenField *>(value->getPatternValue());
        if (field == nullptr || length <= 0)
            return;
        const int width = length * 8;
        if (field->getBitEnd() >= width)
            return;
        const int size = field->getBitEnd() - field->getBitStart() + 1;
        const int shift = field->getBitStart();
        if (size <= 0 || size >= 64)
            return;
        const uint64_t mask = ((static_cast<uint64_t>(1) << size) - 1)
                              << static_cast<unsigned>(shift);
        for (int i = 0; i < list->numVarnodes(); ++i) {
            const ghidra::VarnodeSymbol *named = list->getVarnode(i);
            if (named == nullptr)
                continue;
            into.registers.emplace(named->getName(),
                                   (static_cast<uint64_t>(i) << static_cast<unsigned>(shift)) &
                                       mask);
        }
        into.along_the_way_mask |= sofar_mask;
        into.along_the_way_bits |= sofar_bits;
        return;
    }

    const ghidra::SubtableSymbol *table = dynamic_cast<const ghidra::SubtableSymbol *>(symbol);
    if (table == nullptr)
        return;

    std::map<const ghidra::Constructor *, Constraint> within;
    walk(table->getDecisionTree(), Constraint(), within);
    for (const auto &one : within) {
        uint64_t mask = 0;
        uint64_t bits = 0;
        settle(one.second, length, mask, bits);

        std::string named;
        int forwards_to = -1;
        if (exported_register(one.first, by_offset, named, forwards_to)) {
            // The bits that pick this register, and separately the bits the way
            // here insisted on. Mixing them makes a register's encoding look
            // like it includes the opcode, and then writing one erases the
            // instruction.
            into.registers.emplace(named, bits);
            into.along_the_way_mask |= sofar_mask;
            into.along_the_way_bits |= sofar_bits;
            continue;
        }
        // It stands for whatever one of its own operands stands for, so the
        // question is asked again one level down, carrying what this level
        // insisted on.
        if (forwards_to >= 0 && forwards_to < one.first->getNumOperands()) {
            const ghidra::OperandSymbol *inner = one.first->getOperand(forwards_to);
            if (inner != nullptr)
                resolve_registers(inner->getDefiningSymbol(), length, by_offset,
                                  sofar_mask | mask, sofar_bits | bits, depth + 1, into);
        }
    }
}

Form::Slot read_slot(const ghidra::OperandSymbol *operand, int length,
                     const std::map<uint64_t, std::string> &by_offset)
{
    Form::Slot slot;
    if (operand == nullptr)
        return slot;

    // The field, whether the operand is defined by an expression of its own or
    // by a symbol that has one.
    const ghidra::PatternExpression *expression = operand->getDefiningExpression();
    const ghidra::TripleSymbol *symbol = operand->getDefiningSymbol();
    const ghidra::VarnodeListSymbol *list = nullptr;
    if (expression == nullptr && symbol != nullptr) {
        list = dynamic_cast<const ghidra::VarnodeListSymbol *>(symbol);
        const ghidra::ValueSymbol *value = dynamic_cast<const ghidra::ValueSymbol *>(symbol);
        if (value != nullptr)
            expression = value->getPatternValue();
    }

    if (const ghidra::TokenField *field = dynamic_cast<const ghidra::TokenField *>(expression)) {
        // The field says which bits of its token, counted from the least
        // significant. The bits above are counted from the top of the
        // instruction, so this is turned round to match rather than left in two
        // different countings that would silently disagree.
        const int width = length * 8;
        if (length > 0 && field->getBitEnd() < width) {
            slot.first_bit = width - 1 - field->getBitEnd();
            slot.last_bit = width - 1 - field->getBitStart();
        }
    }

    resolve_registers(symbol, length, by_offset, 0, 0, 0, slot);

    // Which bits actually choose between registers, as opposed to bits the
    // table happened to insist on along the way.
    //
    // Reaching a register through a table means passing decisions that are part
    // of the instruction's own identity - its opcode, among others - and those
    // came back mixed in with the register's bits. Clearing them before writing
    // a register would erase the opcode and write some other instruction
    // entirely, which is exactly what it did.
    //
    // What varies between the choices is what chooses, so that is the mask: the
    // bits where the answers differ from one another.
    if (!slot.registers.empty()) {
        uint64_t either = 0;
        for (const auto &one : slot.registers)
            either |= one.second;
        slot.register_mask = either;
    }
    return slot;
}

} // namespace

bool Catalogue::read(const ir::Target &target, std::string &error)
{
    forms_.clear();
    by_operation_.clear();

    ghidra::SleighArchitecture *architecture = specification_for(
        target.compiler.empty() ? target.language_id : target.language_id + ":" + target.compiler,
        error);
    if (architecture == nullptr)
        return false;

    const ghidra::SleighBase *sleigh =
        dynamic_cast<const ghidra::SleighBase *>(architecture->translate);
    if (sleigh == nullptr) {
        error = "this processor is not described by a specification that can be read this way";
        return false;
    }

    ghidra::SleighSymbol *symbol = sleigh->findSymbol(kRoot);
    ghidra::SubtableSymbol *root = dynamic_cast<ghidra::SubtableSymbol *>(symbol);
    if (root == nullptr) {
        error = std::string("this specification has no ") + kRoot + " table to read";
        return false;
    }

    // Registers by where they are, so a table that stands for one can be asked
    // which it is: what a table exports is a place, and a name is wanted.
    std::map<uint64_t, std::string> by_offset;
    {
        std::map<ghidra::VarnodeData, std::string> every;
        architecture->translate->getAllRegisters(every);
        for (const auto &one : every)
            by_offset.emplace(one.first.offset, one.second);
    }

    // The patterns, taken off the tree that decides which constructor a stream
    // of bits means, since a loaded specification carries them nowhere else.
    std::map<const ghidra::Constructor *, Constraint> patterns;
    walk(root->getDecisionTree(), Constraint(), patterns);

    for (int i = 0; i < root->getNumConstructors(); ++i) {
        ghidra::Constructor *made = root->getConstructor(i);
        if (made == nullptr)
            continue;

        Form form;
        form.operands = made->getNumOperands();
        form.shortest = made->getMinimumLength();
        form.line = made->getLineno();

        ghidra::ConstructTpl *templ = made->getTempl();
        const ghidra::OpTpl *only = nullptr;
        if (templ != nullptr) {
            for (const ghidra::OpTpl *operation : templ->getOpvec()) {
                if (operation == nullptr || is_bookkeeping(operation->getOpcode()))
                    continue;
                form.does.push_back(operation->getOpcode());
                only = operation;
            }
        }

        // The shape of it, when there is one operation to have a shape. This is
        // what a selection matches against: which values are slots waiting to
        // be filled and which the form always uses.
        if (form.does.size() == 1 && only != nullptr) {
            if (only->getOut() != nullptr) {
                form.writes = true;
                form.writes_to = read_piece(only->getOut());
            }
            for (int input = 0; input < only->numInput(); ++input)
                form.reads.push_back(read_piece(only->getIn(input)));
        }

        auto pattern = patterns.find(made);
        if (pattern != patterns.end()) {
            settle(pattern->second, form.shortest, form.fixed_mask, form.fixed_bits);
            form.needs_context = pattern->second.from_context;
        }

        for (int slot = 0; slot < made->getNumOperands(); ++slot)
            form.slots.push_back(read_slot(made->getOperand(slot), form.shortest, by_offset));

        forms_.push_back(std::move(form));
    }

    // Indexed by what they do, and only the ones that do exactly one thing: a
    // form that does several can still be chosen, but not without reasoning
    // about the rest of what it did, which is a later question.
    for (const Form &form : forms_) {
        if (form.does.size() == 1)
            by_operation_[form.does.front()].push_back(&form);
    }
    return true;
}

std::vector<const Form *> Catalogue::plainly_doing(ghidra::OpCode opcode, int inputs,
                                                  bool including_context) const
{
    std::vector<const Form *> plain;
    for (const Form *form : doing(opcode)) {
        if (form->needs_context && !including_context)
            continue;
        if (!form->writes || !form->writes_to.is_slot)
            continue;
        if (static_cast<int>(form->reads.size()) != inputs)
            continue;
        bool all_slots = true;
        for (const Form::Piece &piece : form->reads)
            all_slots = all_slots && piece.is_slot;
        if (all_slots)
            plain.push_back(form);
    }
    return plain;
}

const std::vector<const Form *> &Catalogue::doing(ghidra::OpCode opcode) const
{
    static const std::vector<const Form *> none;
    auto found = by_operation_.find(opcode);
    return found == by_operation_.end() ? none : found->second;
}

bool Catalogue::write(const Form &form, const std::vector<std::string> &registers,
                      std::vector<uint8_t> &bytes, std::string &error)
{
    std::vector<std::vector<std::string>> only;
    for (const std::string &one : registers)
        only.push_back({one});
    std::vector<std::string> used;
    return write_any(form, only, bytes, used, error);
}

bool Catalogue::write_any(const Form &form,
                          const std::vector<std::vector<std::string>> &registers,
                          std::vector<uint8_t> &bytes, std::vector<std::string> &used,
                          std::string &error)
{
    used.clear();
    bytes.clear();
    if (form.shortest <= 0 || form.shortest > 8) {
        error = "this form is not a length an instruction can be written in";
        return false;
    }
    if (form.fixed_mask == 0) {
        error = "this form does not say which bits it insists on";
        return false;
    }

    uint64_t word = form.fixed_bits;
    size_t next = 0;
    for (const Form::Slot &slot : form.slots) {
        if (!slot.is_register())
            continue;
        if (next >= registers.size())
            break;

        // Whichever of this register's names the slot knows.
        auto found = slot.registers.end();
        for (const std::string &name : registers[next]) {
            found = slot.registers.find(name);
            if (found != slot.registers.end()) {
                used.push_back(name);
                break;
            }
        }
        if (found == slot.registers.end()) {
            error = "this instruction cannot put " +
                    (registers[next].empty() ? std::string("that") : registers[next].front()) +
                    " where it was asked to";
            return false;
        }
        // The form's own bits win.
        //
        // A register is reached through a table, and the way there passes
        // decisions that belong to the instruction rather than to the register -
        // its opcode among them. Those came back mixed into the register's bits,
        // so writing one without care erases the opcode and writes some other
        // instruction entirely. Which it did: a RISC-V add came out as a
        // compressed floating-point store.
        //
        // So a slot may only touch bits the form did not insist on. What it
        // insisted on is what makes the instruction that instruction.
        const uint64_t may_touch = ~form.fixed_mask;
        word = (word & ~(slot.register_mask & may_touch)) | (found->second & may_touch);
        // What the way to the register insisted on is part of the instruction,
        // so it is set rather than cleared.
        word |= slot.along_the_way_bits & may_touch;
        word = (word & ~form.fixed_mask) | form.fixed_bits;
        ++next;
    }
    if (next < registers.size()) {
        error = "this instruction has fewer places for registers than it was given";
        return false;
    }

    // The bits are the instruction's bytes in the order they are written, with
    // the first byte most significant, so writing them out is reading them back
    // the same way. Which end of memory those bytes go to is the processor's
    // byte order, and it has already been accounted for.
    bytes.resize(static_cast<size_t>(form.shortest));
    for (int at = 0; at < form.shortest; ++at) {
        const unsigned shift = static_cast<unsigned>((form.shortest - 1 - at) * 8);
        bytes[static_cast<size_t>(at)] = static_cast<uint8_t>((word >> shift) & 0xff);
    }
    return true;
}

std::string Catalogue::summary() const
{
    size_t single = 0;
    size_t silent = 0;
    for (const Form &form : forms_) {
        if (form.does.size() == 1)
            ++single;
        else if (form.does.empty())
            ++silent;
    }

    std::ostringstream out;
    out << forms_.size() << " forms, " << single << " doing one thing, " << silent
        << " doing nothing on their own";
    return out.str();
}

} // namespace catalogue
} // namespace nova
} // namespace astral_internal
