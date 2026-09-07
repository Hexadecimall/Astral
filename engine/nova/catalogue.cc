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
    case ghidra::ConstTpl::real: {
        piece.is_fixed = true;
        piece.fixed = offset.getReal();
        const ghidra::ConstTpl &space = value->getSpace();
        if (space.getType() == ghidra::ConstTpl::spaceid) {
            const ghidra::AddrSpace *which = space.getSpace();
            piece.fixed_is_register =
                which != nullptr && which->getType() == ghidra::IPTR_PROCESSOR;
        }
        break;
    }
    default:
        // Something worked out while decoding - where the instruction is, or
        // where it goes next. Neither is a slot to fill nor a fixed number, and
        // saying so is better than pretending it is one.
        break;
    }
    return piece;
}

// A field taken as the processor takes it.
Form::Slot::Field field_of(const ghidra::TokenField *field)
{
    Form::Slot::Field out;
    if (field == nullptr)
        return out;
    out.first_byte = field->getByteStart();
    out.last_byte = field->getByteEnd();
    out.shift = field->getShift();
    out.width = field->getBitEnd() - field->getBitStart() + 1;
    out.big_endian = field->isBigEndian();
    out.is_signed = field->isSigned();
    return out;
}

// Where a value put in that field lands, in the instruction's bytes as written.
//
// Reading a field takes its bytes in the order they are written, assembles them
// the way its token assembles, and shifts right. Writing one is that backwards:
// the value is shifted left, and its bytes are put back where they came from.
//
// The two orders are the same on a processor that writes its instructions
// biggest byte first and are a byte-swap apart on one that does not, which is
// why this is done a byte at a time rather than as a shift. Treating a field as
// a range of bits put every AARCH64 register one swap away from where it goes.
bool place_in_field(const Form::Slot::Field &field, int length, uint64_t value, uint64_t &mask,
                    uint64_t &bits, int value_bytes = 8)
{
    mask = 0;
    bits = 0;
    if (!field.is_placed() || field.width >= 64 || field.last_byte < field.first_byte ||
        field.last_byte >= length || length <= 0 || length > 8)
        return false;

    const uint64_t room = (static_cast<uint64_t>(1) << field.width) - 1;
    if ((value & room) != value) {
        // A field the processor reads as signed holds negative numbers, and a
        // negative number has every bit above it set - as far up as the number
        // is wide, and no further. So what has to fit is the number at its own
        // width: sign-extending what would be stored, and comparing within
        // those bytes, because minus four is 0xfffffffc in four of them and
        // 0xfffffffffffffffc in eight.
        if (!field.is_signed)
            return false;  // too big for the field: not this instruction's number
        const unsigned spare = static_cast<unsigned>(64 - field.width);
        const uint64_t back =
            static_cast<uint64_t>(static_cast<int64_t>(value << spare) >> spare);
        const uint64_t within = value_bytes > 0 && value_bytes < 8
                                    ? (static_cast<uint64_t>(1) << (value_bytes * 8)) - 1
                                    : ~static_cast<uint64_t>(0);
        if ((back & within) != (value & within))
            return false;
    }
    value &= room;

    const uint64_t assembled_mask = room << static_cast<unsigned>(field.shift);
    const uint64_t assembled_bits = value << static_cast<unsigned>(field.shift);

    const int count = field.last_byte - field.first_byte + 1;
    for (int i = 0; i < count; ++i) {
        // Byte i of the assembled value, least significant first, goes back to
        // whichever written byte it was read from.
        const int written = field.big_endian ? field.last_byte - i : field.first_byte + i;
        const unsigned at = static_cast<unsigned>((length - 1 - written) * 8);
        mask |= ((assembled_mask >> static_cast<unsigned>(8 * i)) & 0xff) << at;
        bits |= ((assembled_bits >> static_cast<unsigned>(8 * i)) & 0xff) << at;
    }
    return true;
}

// One place a template names, when it names one outright.
struct PlaceKey {
    const ghidra::AddrSpace *space = nullptr;
    uint64_t offset = 0;

    bool operator<(const PlaceKey &other) const
    {
        return space != other.space ? space < other.space : offset < other.offset;
    }
    bool operator==(const PlaceKey &other) const
    {
        return space == other.space && offset == other.offset;
    }
};

bool key_of(const ghidra::VarnodeTpl *value, PlaceKey &out)
{
    if (value == nullptr)
        return false;
    const ghidra::ConstTpl &space = value->getSpace();
    const ghidra::ConstTpl &offset = value->getOffset();
    if (space.getType() != ghidra::ConstTpl::spaceid ||
        offset.getType() != ghidra::ConstTpl::real)
        return false;
    out.space = space.getSpace();
    out.offset = offset.getReal();
    return true;
}

// Where a control transfer's target came from.
//
// A processor's return instruction almost never says "go back". It loads the
// program counter from somewhere and goes through that, and the interesting
// part is what it loaded. AARCH64's `ret` copies x30 into the program counter
// and returns through it; MIPS's `jr ra` masks the low bit off ra first,
// because that bit chooses an instruction set, and only then goes. In both the
// value the return literally reads is the program counter, which is the same
// register every other kind of return goes through too - so reading it says
// nothing about which kind this is.
//
// What does say is where the value came from. This walks that backwards: from
// the place the transfer goes through, to whatever else in the same template
// put something there, to whatever those read, until it reaches places nothing
// in the template wrote. Those arrived with the instruction, and they are what
// the form really goes back through.
void sources_of(const std::vector<ghidra::OpTpl *> &operations, size_t before,
                const ghidra::VarnodeTpl *target, std::vector<Form::Piece> &out)
{
    std::vector<const ghidra::VarnodeTpl *> waiting;
    waiting.push_back(target);
    std::set<PlaceKey> seen;

    // A template is short and this only walks backwards, but a specification is
    // somebody else's file and a bound costs nothing.
    for (int steps = 0; !waiting.empty() && steps < 64; ++steps) {
        const ghidra::VarnodeTpl *value = waiting.back();
        waiting.pop_back();
        if (value == nullptr)
            continue;

        // A slot is where the trail ends: what goes in it is chosen when an
        // instruction is written rather than by the form.
        if (value->getOffset().getType() == ghidra::ConstTpl::handle) {
            out.push_back(read_piece(value));
            continue;
        }

        PlaceKey key;
        if (!key_of(value, key) || !seen.insert(key).second)
            continue;

        const ghidra::OpTpl *wrote = nullptr;
        for (size_t i = 0; i < before && i < operations.size(); ++i) {
            const ghidra::OpTpl *operation = operations[i];
            if (operation == nullptr || is_bookkeeping(operation->getOpcode()))
                continue;
            PlaceKey where;
            if (operation->getOut() != nullptr && key_of(operation->getOut(), where) &&
                where == key)
                wrote = operation;
        }

        if (wrote == nullptr) {
            // Nothing in the template put it there, so it was already there
            // when the instruction ran. Only a register is worth reporting: the
            // constants a template masks and shifts with are its own arithmetic
            // and say nothing about where control is going.
            const Form::Piece piece = read_piece(value);
            if (piece.is_fixed && piece.fixed_is_register)
                out.push_back(piece);
            continue;
        }
        for (int i = 0; i < wrote->numInput(); ++i)
            waiting.push_back(wrote->getIn(i));
    }
}

// The last thing before `before` to write where `value` is, or nothing.
const ghidra::OpTpl *writer_of(const std::vector<ghidra::OpTpl *> &operations, size_t before,
                               const ghidra::VarnodeTpl *value, size_t &at)
{
    PlaceKey wanted;
    if (!key_of(value, wanted))
        return nullptr;
    const ghidra::OpTpl *wrote = nullptr;
    for (size_t i = 0; i < before && i < operations.size(); ++i) {
        const ghidra::OpTpl *operation = operations[i];
        if (operation == nullptr || is_bookkeeping(operation->getOpcode()))
            continue;
        PlaceKey where;
        if (operation->getOut() != nullptr && key_of(operation->getOut(), where) &&
            where == wanted) {
            wrote = operation;
            at = i;
        }
    }
    return wrote;
}

// A value with the moves that carried it followed through.
//
// A template moves values into scratch before working on them and out of
// scratch afterwards, and those moves are not part of what the instruction
// does - they are how a specification is written. Following them gives the
// place the value really came from, which is a slot to fill or something the
// form names outright. A value that some other kind of operation computed is
// neither, and comes back as nothing.
Form::Piece through_moves(const std::vector<ghidra::OpTpl *> &operations, size_t before,
                          const ghidra::VarnodeTpl *value)
{
    for (int steps = 0; steps < 8; ++steps) {
        if (value == nullptr)
            return Form::Piece();
        if (value->getOffset().getType() == ghidra::ConstTpl::handle)
            return read_piece(value);
        size_t at = 0;
        const ghidra::OpTpl *wrote = writer_of(operations, before, value, at);
        if (wrote == nullptr)
            return read_piece(value);  // it was there before the instruction ran
        // A widening is a move. `sltu rd, rs, rt` compares and then zero-extends
        // the answer into rd, so following only plain copies stopped at the
        // extension and the form was read as an extension that happens to
        // compare - which is why not one processor appeared to have a single
        // instruction that compares.
        if ((wrote->getOpcode() != ghidra::CPUI_COPY &&
             wrote->getOpcode() != ghidra::CPUI_INT_ZEXT &&
             wrote->getOpcode() != ghidra::CPUI_INT_SEXT) ||
            wrote->numInput() < 1)
            return Form::Piece();      // computed on the way: not a place
        value = wrote->getIn(0);
        before = at;
    }
    return Form::Piece();
}

// An address the instruction makes out of its own operands.
//
// A load does not take an address. It takes the pieces one is made of - a
// register and a displacement, added together inside the instruction - and no
// amount of following moves reaches a single place, because there is not one:
// there is a sum. So every place the address depends on is collected instead,
// and the form is usable when exactly one of them is a register the caller can
// name. The rest are numbers, and the instruction that means "read from the
// address in this register" is that one with those numbers set to nought.
//
// Returns false when the address depends on more than one register, or on
// nothing that can be named, which is an instruction whose address this cannot
// supply rather than one to guess at.
bool address_of(const std::vector<ghidra::OpTpl *> &operations, size_t before,
                const ghidra::VarnodeTpl *target, const std::vector<Form::Slot> &slots,
                Form::Piece &out, std::vector<int> &zeroed)
{
    std::vector<Form::Piece> pieces;
    sources_of(operations, before, target, pieces);

    bool found = false;
    for (const Form::Piece &piece : pieces) {
        // A number the instruction uses on its own account is not one of the
        // pieces being supplied. A shift by a register masks the amount to the
        // width of the thing being shifted before shifting by it, and refusing
        // any form that mentions a constant on the way refused every
        // register-amount shift on AARCH64 - which left only the shifts by a
        // written-in amount, whose field this cannot solve.
        if (piece.is_fixed && !piece.fixed_is_register)
            continue;
        if (!piece.is_slot || piece.slot < 0 ||
            static_cast<size_t>(piece.slot) >= slots.size())
            return false;
        if (slots[piece.slot].is_register()) {
            if (found)
                return false;  // two registers is a sum this cannot supply
            out = piece;
            found = true;
            continue;
        }
        if (!slots[piece.slot].is_placed())
            return false;
        zeroed.push_back(piece.slot);
    }
    return found;
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
            // What the pattern at the bottom still says, read the whole way
            // across the instruction.
            //
            // Both counts are in bits, and what comes back is pushed down to
            // the bottom of however many were asked for. So asking for a few
            // returns those few sitting where the last few would be, and adding
            // that to an instruction puts the top of the pattern at the bottom
            // of the word. Asking for exactly as many bits as the instruction
            // is long puts them where they belong.
            //
            // This is where a register a form names outright is written down.
            // A specification has one constructor for `jr` over any register
            // and another for `jr ra` alone, and what separates them is five
            // bits in the pattern here. Dropping them left every return coming
            // out as a return through register zero.
            //
            // Only a word can be asked for at a time, so a longer instruction
            // is read a word at a time and put back together in order.
            const int across = made->getMinimumLength() * 8;
            for (int at = 0; at < across && at < 64; at += 32) {
                const int take = across - at < 32 ? across - at : 32;
                whole.leaf_mask = (whole.leaf_mask << take) |
                                  static_cast<uint64_t>(pattern->getMask(at, take, false));
                whole.leaf_bits = (whole.leaf_bits << take) |
                                  static_cast<uint64_t>(pattern->getValue(at, take, false));
            }
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

// The fields a number can be written into, behind whatever stands for them.
//
// An operand that takes a number is usually a table rather than a field.
// AARCH64 spells a constant with movz, whose operand is a table of sixteen-bit
// fields shifted by different amounts; RISC-V's immI is a table of one field
// sign-extended. Each of those is a real field in real bits reached by real
// bits, and this finds them - one entry per way, because they are alternatives
// and merging them would describe an encoding that does not exist.
//
// What the table does to the field on the way out is not worked out here. That
// is a question with an easier answer than reading the template: write the
// field and ask the processor what number came out.
void resolve_numbers(const ghidra::TripleSymbol *symbol, int length, uint64_t sofar_mask,
                     uint64_t sofar_bits, int depth, Form::Slot &into)
{
    if (symbol == nullptr || depth > 4)
        return;

    if (const ghidra::ValueSymbol *value = dynamic_cast<const ghidra::ValueSymbol *>(symbol)) {
        if (const ghidra::TokenField *field =
                dynamic_cast<const ghidra::TokenField *>(value->getPatternValue())) {
            Form::Slot::Way way;
            way.field = field_of(field);
            way.along_mask = sofar_mask;
            way.along_bits = sofar_bits;
            if (way.field.is_placed())
                into.numbers.push_back(way);
        }
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
        for (int slot = 0; slot < one.first->getNumOperands(); ++slot) {
            const ghidra::OperandSymbol *inner = one.first->getOperand(slot);
            if (inner == nullptr)
                continue;
            if (const ghidra::TokenField *field =
                    dynamic_cast<const ghidra::TokenField *>(inner->getDefiningExpression())) {
                Form::Slot::Way way;
                way.field = field_of(field);
                way.along_mask = sofar_mask | mask;
                way.along_bits = sofar_bits | bits;
                if (way.field.is_placed())
                    into.numbers.push_back(way);
                continue;
            }
            resolve_numbers(inner->getDefiningSymbol(), length, sofar_mask | mask,
                            sofar_bits | bits, depth + 1, into);
        }
    }
}

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
        const Form::Slot::Field where = field_of(field);
        for (int i = 0; i < list->numVarnodes(); ++i) {
            const ghidra::VarnodeSymbol *named = list->getVarnode(i);
            if (named == nullptr)
                continue;
            uint64_t mask = 0;
            uint64_t bits = 0;
            if (!place_in_field(where, length, static_cast<uint64_t>(i), mask, bits))
                continue;
            Form::Slot::Choice choice;
            choice.bits = bits;
            choice.along_mask = sofar_mask | mask;
            choice.along_bits = sofar_bits | bits;
            into.registers.emplace(named->getName(), choice);
        }
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
            // instruction. Each register keeps its own way, because two
            // branches of the same table put the register in different bits
            // and demand different things to get there.
            Form::Slot::Choice choice;
            choice.bits = bits;
            choice.along_mask = sofar_mask | mask;
            choice.along_bits = sofar_bits | bits;
            into.registers.emplace(named, choice);
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
        Form::Slot::Way way;
        way.field = field_of(field);
        if (way.field.is_placed())
            slot.numbers.push_back(way);
    } else if (expression == nullptr) {
        resolve_numbers(symbol, length, 0, 0, 0, slot);
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
        uint64_t any_set = 0;
        uint64_t all_set = ~static_cast<uint64_t>(0);
        uint64_t any_mask = 0;
        uint64_t all_mask = ~static_cast<uint64_t>(0);
        for (const auto &one : slot.registers) {
            const uint64_t value =
                one.second.bits | (one.second.along_bits & one.second.along_mask);
            any_set |= value;
            all_set &= value;
            any_mask |= one.second.bits | one.second.along_mask;
            all_mask &= one.second.bits | one.second.along_mask;
        }
        // A bit chooses when the choices disagree about it, either in what they
        // set it to or in whether they speak of it at all.
        slot.register_mask = (any_set & ~all_set) | (any_mask & ~all_mask);
        slot.always_mask = all_mask & ~slot.register_mask;
        slot.always_bits = all_set & slot.always_mask;
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

        for (int slot = 0; slot < made->getNumOperands(); ++slot)
            form.slots.push_back(read_slot(made->getOperand(slot), form.shortest, by_offset));

        ghidra::ConstructTpl *templ = made->getTempl();
        const ghidra::OpTpl *only = nullptr;
        size_t only_at = 0;
        if (templ != nullptr) {
            // The last operation, but not a `callother`.
            //
            // A callother is what a specification writes when p-code has no
            // way to say the thing: a barrier, a cache hint, a mode change. It
            // is a remark about the instruction rather than what the
            // instruction is for, and it is often written last - so a store
            // followed by its release barrier looked like a form that performs
            // a barrier and happens to store on the way.
            size_t at = 0;
            for (const ghidra::OpTpl *operation : templ->getOpvec()) {
                if (operation != nullptr && !is_bookkeeping(operation->getOpcode())) {
                    form.does.push_back(operation->getOpcode());
                    if (operation->getOpcode() != ghidra::CPUI_CALLOTHER || only == nullptr) {
                        only = operation;
                        only_at = at;
                    }
                }
                ++at;
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
        } else if (form.does.size() > 1 && templ != nullptr && only != nullptr &&
                   form.does.back() != ghidra::CPUI_RETURN) {
            // What the form does, when its template is a real instruction.
            //
            // Almost no instruction on a real processor is one p-code
            // operation. An AARCH64 add is seven: one that moves the second
            // operand into scratch, the addition, four that set the condition
            // flags, and one that moves the answer out. Holding out for
            // templates of length one found the handful of clean instructions
            // on each processor and missed the rest - on AARCH64 it left
            // exactly one form that adds, and that one adds a number.
            //
            // The operation that matters is the one whose result reaches what
            // the form writes. So the last thing to write one of the form's own
            // slots is found, what it wrote is followed back through the moves
            // that carried it, and whatever computed it is what the form does.
            // Its inputs are followed back the same way.
            //
            // Everything else is the instruction's own business - its flags,
            // its scratch - and is recorded rather than dropped, because
            // choosing this form disturbs those registers and something has to
            // know that.
            const std::vector<ghidra::OpTpl *> &operations = templ->getOpvec();
            const ghidra::OpTpl *ends_at = nullptr;
            size_t ends_at_index = 0;
            for (size_t at = 0; at < operations.size(); ++at) {
                const ghidra::OpTpl *operation = operations[at];
                if (operation == nullptr || is_bookkeeping(operation->getOpcode()) ||
                    operation->getOut() == nullptr)
                    continue;
                if (operation->getOut()->getOffset().getType() == ghidra::ConstTpl::handle) {
                    ends_at = operation;
                    ends_at_index = at;
                }
            }

            if (ends_at == nullptr && only->getOut() == nullptr && only->numInput() > 0) {
                // A form that writes nowhere is what its last operation is.
                //
                // A store, a call and a branch all produce no value, so there
                // is no answer to trace back from - and looking for one skipped
                // every one of them. What they read is what they read, with the
                // moves that carried it followed through.
                const std::vector<ghidra::OpTpl *> &all = templ->getOpvec();
                const bool asks_a_question = only->getOpcode() == ghidra::CPUI_CBRANCH;

                std::vector<Form::Piece> reads;
                std::vector<int> zeroed;
                bool nameable = true;
                const bool goes_somewhere = only->getOpcode() == ghidra::CPUI_BRANCH ||
                                            only->getOpcode() == ghidra::CPUI_CBRANCH ||
                                            only->getOpcode() == ghidra::CPUI_CALL;
                const bool names_a_space = only->getOpcode() == ghidra::CPUI_STORE ||
                                           only->getOpcode() == ghidra::CPUI_LOAD;

                for (int input = 0; input < only->numInput(); ++input) {
                    Form::Piece piece = through_moves(all, only_at, only->getIn(input));
                    if (piece.is_slot || piece.is_fixed) {
                        reads.push_back(piece);
                        continue;
                    }

                    // Which memory it touches is not one of its values. p-code
                    // names the space first, and that name is neither a place
                    // nor a number, so demanding it be one marked every store
                    // whose template does more than store as unnameable - which
                    // on RISC-V is every ordinary store, since the address is a
                    // register plus a displacement added inside the
                    // instruction. What was left was the hypervisor stores,
                    // whose address is a bare register and whose template is
                    // therefore one operation. Everything downstream already
                    // passes this input over; this is the same rule.
                    if (names_a_space && input == 0) {
                        reads.push_back(piece);
                        continue;
                    }

                    // Where a transfer goes is not named, it is worked out
                    // while decoding - from where the instruction is and what
                    // its offset field holds. Nothing puts a value there, so
                    // requiring it to be a place anybody could name refused
                    // every branch and every call on every processor.
                    if (goes_somewhere && input == 0) {
                        reads.push_back(piece);
                        continue;
                    }

                    // A branch works its own answer out. `beq rs, rt` compares
                    // and then goes if it liked the answer, so what it reads is
                    // the two registers and what it asks is the comparison -
                    // and both come off the template rather than from anything
                    // put in a slot.
                    if (asks_a_question && input == 1) {
                        std::vector<Form::Piece> asked;
                        sources_of(all, only_at, only->getIn(input), asked);

                        // A branch may ask about places the instruction does
                        // not name at all. AARCH64 leaves facts about two
                        // registers in its flags with one instruction and goes
                        // somewhere when a combination of those holds with
                        // another, so the second names no registers and depends
                        // entirely on what the first left. Refusing every form
                        // whose question is not over its own slots refused all
                        // of those - which is every conditional branch AARCH64
                        // has - so what is kept is the slots, and a form that
                        // has none is a form that asks about the flags.
                        bool all_known = !asked.empty();
                        for (const Form::Piece &one : asked)
                            all_known = all_known && (one.is_slot ||
                                                      (one.is_fixed && one.fixed_is_register));
                        if (all_known) {
                            for (size_t back = asked.size(); back > 0; --back) {
                                if (asked[back - 1].is_slot)
                                    reads.push_back(asked[back - 1]);
                            }
                            // Which question, taken from whatever worked the
                            // answer out on the way - and which way round it is
                            // asked, since a branch that goes when a register
                            // is nothing and one that goes when it is not are
                            // the same comparison and opposite instructions.
                            // Taking only the comparison made them
                            // indistinguishable, so neither could be chosen and
                            // every branch of that shape was refused.
                            bool the_other_way = false;
                            for (size_t k = 0; k < all.size() && k < only_at; ++k) {
                                if (all[k] == nullptr)
                                    continue;
                                const ghidra::OpCode what = all[k]->getOpcode();
                                if (what == ghidra::CPUI_BOOL_NEGATE)
                                    the_other_way = !the_other_way;
                                if (what == ghidra::CPUI_INT_EQUAL ||
                                    what == ghidra::CPUI_INT_NOTEQUAL ||
                                    what == ghidra::CPUI_INT_LESS ||
                                    what == ghidra::CPUI_INT_SLESS ||
                                    what == ghidra::CPUI_INT_LESSEQUAL ||
                                    what == ghidra::CPUI_INT_SLESSEQUAL)
                                    form.compares = what;
                            }
                            if (the_other_way) {
                                switch (form.compares) {
                                case ghidra::CPUI_INT_EQUAL:
                                    form.compares = ghidra::CPUI_INT_NOTEQUAL;
                                    break;
                                case ghidra::CPUI_INT_NOTEQUAL:
                                    form.compares = ghidra::CPUI_INT_EQUAL;
                                    break;
                                default:
                                    // The opposite of a comparison of size is
                                    // another comparison of size with its sides
                                    // the other way round, which is not a thing
                                    // this can say - so it says nothing rather
                                    // than something wrong.
                                    form.compares = ghidra::CPUI_COPY;
                                    break;
                                }
                            }
                            continue;
                        }
                    }

                    if (!address_of(all, only_at, only->getIn(input), form.slots, piece, zeroed))
                        nameable = false;
                    reads.push_back(piece);
                }
                if (nameable) {
                    form.zeroed = std::move(zeroed);
                    std::vector<uint64_t> disturbed;
                    std::vector<int> disturbed_slots;
                    for (const ghidra::OpTpl *operation : all) {
                        if (operation == nullptr || operation == only ||
                            is_bookkeeping(operation->getOpcode()))
                            continue;
                        const Form::Piece wrote = read_piece(operation->getOut());
                        if (wrote.is_fixed && wrote.fixed_is_register)
                            disturbed.push_back(wrote.fixed);
                        else if (wrote.is_slot)
                            disturbed_slots.push_back(wrote.slot);
                    }
                    form.does.assign(1, only->getOpcode());
                    form.writes = false;
                    form.reads = std::move(reads);
                    form.also_writes = std::move(disturbed);
                    form.also_writes_slots = std::move(disturbed_slots);
                }
            } else if (ends_at != nullptr && ends_at->numInput() > 0) {
                // What produced the answer, with the moves followed through.
                const ghidra::OpTpl *doing = ends_at;
                size_t doing_at = ends_at_index;
                for (int steps = 0;
                     steps < 8 &&
                     (doing->getOpcode() == ghidra::CPUI_COPY ||
                      doing->getOpcode() == ghidra::CPUI_INT_ZEXT ||
                      doing->getOpcode() == ghidra::CPUI_INT_SEXT) &&
                     doing->numInput() > 0;
                     ++steps) {
                    size_t at = 0;
                    const ghidra::OpTpl *earlier =
                        writer_of(operations, doing_at, doing->getIn(0), at);
                    if (earlier == nullptr)
                        break;
                    doing = earlier;
                    doing_at = at;
                }

                bool nameable = doing != ends_at || (doing->getOpcode() != ghidra::CPUI_COPY &&
                                                     doing->getOpcode() != ghidra::CPUI_INT_ZEXT &&
                                                     doing->getOpcode() != ghidra::CPUI_INT_SEXT);
                std::vector<Form::Piece> reads;
                std::vector<int> zeroed;
                const bool names_a_space = doing->getOpcode() == ghidra::CPUI_LOAD ||
                                           doing->getOpcode() == ghidra::CPUI_STORE;
                for (int input = 0; input < doing->numInput(); ++input) {
                    Form::Piece piece = through_moves(operations, doing_at, doing->getIn(input));
                    // Which memory it touches is not one of its values - see
                    // the store above, and `plainly_doing`, which passes over
                    // the same input for the same reason.
                    if (names_a_space && input == 0) {
                        reads.push_back(piece);
                        continue;
                    }
                    // Or worked out from the form's own pieces, the way an
                    // address is. A shift by a register masks the amount to the
                    // width of what is being shifted before shifting by it, so
                    // following the moves back stops at the mask - and every
                    // register-amount shift was refused, leaving only the ones
                    // whose amount is written in, whose field cannot be solved.
                    if (!piece.is_slot && !piece.is_fixed &&
                        !address_of(operations, doing_at, doing->getIn(input), form.slots, piece,
                                    zeroed))
                        nameable = false;
                    reads.push_back(piece);
                }

                if (nameable) {
                    form.zeroed = zeroed;
                    // The registers the rest of the template disturbs, so that
                    // choosing this form is a decision somebody can make.
                    std::vector<uint64_t> disturbed;
                    std::vector<int> disturbed_slots;
                    for (size_t at = 0; at < operations.size(); ++at) {
                        const ghidra::OpTpl *operation = operations[at];
                        if (operation == nullptr || operation == doing ||
                            operation == ends_at || is_bookkeeping(operation->getOpcode()))
                            continue;
                        const Form::Piece wrote = read_piece(operation->getOut());
                        if (wrote.is_fixed && wrote.fixed_is_register)
                            disturbed.push_back(wrote.fixed);
                        else if (wrote.is_slot)
                            disturbed_slots.push_back(wrote.slot);
                    }

                    form.does.assign(1, doing->getOpcode());
                    form.writes = true;
                    form.writes_to = read_piece(ends_at->getOut());
                    form.reads = std::move(reads);
                    form.also_writes = std::move(disturbed);
                    form.also_writes_slots = std::move(disturbed_slots);
                }
            }
        } else if (form.does.size() > 1 && only != nullptr && templ != nullptr &&
                   form.does.back() == ghidra::CPUI_RETURN && only->numInput() > 0) {
            // A form whose last operation goes back is that return, however
            // long the way there was.
            //
            // Going back is several operations on most processors, and none of
            // them is optional: a MIPS function cannot return without the mask
            // that clears the instruction-set bit, and an AARCH64 one cannot
            // without the copy into the program counter. Refusing every form
            // that does more than one thing refused every real return on both,
            // and left only the ones that return from an exception - which are
            // one operation each, are the same length, and fault.
            //
            // So the extra operations here are how this processor performs a
            // return rather than something else it does as well, and the form
            // is taken as reading whatever the return goes back through.
            std::vector<Form::Piece> sources;
            sources_of(templ->getOpvec(), only_at, only->getIn(0), sources);
            if (!sources.empty()) {
                form.does.assign(1, ghidra::CPUI_RETURN);
                form.writes = false;
                form.reads = std::move(sources);
            }
        }

        auto pattern = patterns.find(made);
        if (pattern != patterns.end()) {
            settle(pattern->second, form.shortest, form.fixed_mask, form.fixed_bits);
            form.needs_context = pattern->second.from_context;
        }

        // A bit that a value goes in is not a bit the form insists on.
        //
        // The tree that picks a form also splits on the bits where values go,
        // because that is how it tells one encoding from a neighbouring one,
        // and those splits came back looking like demands. Left that way the
        // fields are frozen: a number written into one came out as whatever
        // number the form was reached through, so copying twenty produced
        // eighteen.
        for (const Form::Slot &slot : form.slots) {
            form.fixed_mask &= ~slot.register_mask;
            if (slot.is_register() || !slot.is_placed())
                continue;
            for (const Form::Slot::Way &way : slot.numbers) {
                uint64_t room = 0;
                uint64_t unused_bits = 0;
                const uint64_t every =
                    way.field.width >= 64 ? ~static_cast<uint64_t>(0)
                                          : (static_cast<uint64_t>(1) << way.field.width) - 1;
                if (place_in_field(way.field, form.shortest, every, room, unused_bits))
                    form.fixed_mask &= ~room;
            }
        }
        form.fixed_bits &= form.fixed_mask;

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

std::set<uint64_t> Catalogue::registers_for_values(const ir::Target &target) const
{
    // What an integer addition can write into, and nothing else.
    //
    // Adding is the question because nothing an integer adds into is a
    // floating-point register - that is what makes it one. Asking more widely
    // does not narrow it: a move crosses between the files on purpose, since
    // MIPS moves between its general and floating-point registers with `mfc1`,
    // and MIPS spells some floating-point work as an or. Either put f20 to f30
    // among the places an integer value might live, so half of what this
    // processor offered was somewhere no integer instruction could reach and
    // every value landing there was refused with nowhere else to go.
    auto written_by = [&](ghidra::OpCode what, std::set<uint64_t> &into) {
        for (const Form *form : doing(what)) {
            if (!form->writes || !form->writes_to.is_slot || form->writes_to.slot < 0 ||
                static_cast<size_t>(form->writes_to.slot) >= form->slots.size())
                continue;
            for (const auto &one : form->slots[form->writes_to.slot].registers) {
                const ir::Target::RegisterPlace *place = target.register_place(one.first);
                if (place != nullptr)
                    into.insert(place->offset);
            }
        }
    };

    std::set<uint64_t> where;
    written_by(ghidra::CPUI_INT_ADD, where);
    if (!where.empty())
        return where;

    // A processor whose addition names no register to write - because it adds
    // into one particular place, or because it has no addition - is asked the
    // other way. Saying nothing here means no filtering at all, which is worse
    // than a wider answer.
    for (ghidra::OpCode what :
         {ghidra::CPUI_INT_SUB, ghidra::CPUI_INT_AND, ghidra::CPUI_INT_OR, ghidra::CPUI_COPY}) {
        written_by(what, where);
        if (!where.empty())
            return where;
    }
    return where;
}

bool Catalogue::can_carry(ghidra::OpCode what, uint64_t number, int bytes) const
{
    for (const Form *form : doing(what)) {
        for (const Form::Slot &slot : form->slots) {
            if (slot.is_register())
                continue;
            for (const Form::Slot::Way &way : slot.numbers) {
                uint64_t room = 0;
                uint64_t sitting = 0;
                if (place_in_field(way.field, form->shortest, number, room, sitting, bytes))
                    return true;
            }
        }
    }
    return false;
}

int Catalogue::widest_for_values(const ir::Target &target) const
{
    // The same pool a value would actually be given a home from, asked at each
    // width from the widest down.
    //
    // Asking the instruction set alone says eight on MIPS, because its
    // accumulators are eight bytes and an integer adds into them - but the
    // convention has no opinion about those, so nothing is ever housed there and
    // a value of eight bytes has nowhere to go. A width this reports is a width
    // something can be put in, so both have to agree on it.
    const std::set<uint64_t> ordinary = registers_for_values(target);
    if (ordinary.empty())
        return 0;
    for (int width = 16; width >= 1; width /= 2) {
        std::vector<std::string> named;
        std::string unused;
        if (!target.value_registers(width, named, unused))
            continue;
        for (const std::string &one : named) {
            const ir::Target::RegisterPlace *place = target.register_place(one);
            if (place != nullptr && place->width >= width &&
                ordinary.count(place->offset) != 0)
                return width;
        }
    }
    return 0;
}

bool Catalogue::return_through(const ir::Target &target, uint64_t &offset) const
{
    // What the compiler specification says, when it says anything. MIPS and
    // RISC-V both name `ra` here, and both have return forms naming several
    // registers, so this is asked first and settles those.
    std::string name;
    ir::Target::RegisterPlace place;
    std::string missing;
    if (target.return_address(name, place, missing)) {
        offset = place.offset;
        return true;
    }

    // Otherwise the instructions themselves, and only if they agree. A form
    // that takes the register as an operand says nothing about which one a
    // function uses, so only the ones naming a register outright are counted.
    bool found = false;
    uint64_t agreed = 0;
    for (const Form *form : doing(ghidra::CPUI_RETURN)) {
        for (const Form::Piece &piece : form->reads) {
            if (piece.is_slot || !piece.is_fixed || !piece.fixed_is_register)
                continue;
            if (found && piece.fixed != agreed)
                return false;  // they disagree, so they are not saying it
            agreed = piece.fixed;
            found = true;
        }
    }
    if (found)
        offset = agreed;
    return found;
}

std::vector<const Form *> Catalogue::plainly_doing(ghidra::OpCode opcode, int inputs,
                                                  bool including_context) const
{
    std::vector<const Form *> plain;
    for (const Form *form : doing(opcode)) {
        if (form->needs_context && !including_context)
            continue;
        // A form that writes something has to write it somewhere that can be
        // chosen. One that writes nothing - leaving a function, going
        // somewhere - is not disqualified by having nowhere to put an answer
        // it does not produce.
        if (form->writes && !form->writes_to.is_slot)
            continue;
        // A form reads two kinds of thing, and only one of them is asked for.
        //
        // A slot is a place the form leaves open, and something has to be put in
        // it. A fixed read is a register or a number the form names outright,
        // because the instruction has no operand for it: a return reads the
        // register the return address is in and takes no argument saying so.
        // Counting those as things to supply asked for one value too many and
        // threw every return away; requiring them to be slots threw the same
        // ones away for a second reason.
        //
        // So a fixed read is part of what the instruction is rather than part of
        // what is put into it, and whether the form really is the instruction
        // meant is settled where it always is, by reading the bytes back.
        // Which memory a load or a store touches is not one of its values.
        //
        // p-code names the space first because it describes machines that have
        // more than one and has to say which. It is not a number and not a
        // place: it comes back as neither, which made every load and every
        // store on every processor look like a form with something unnameable
        // in it. An instruction does not name a memory - the choice is in which
        // instruction it is - so it is passed over here as it is everywhere
        // else.
        // A branch's destination is not one of its values either. p-code
        // names where it goes first, and where it goes is settled by laying the
        // function out rather than by anything being put in a slot.
        const bool names_a_space =
            opcode == ghidra::CPUI_LOAD || opcode == ghidra::CPUI_STORE ||
            opcode == ghidra::CPUI_BRANCH || opcode == ghidra::CPUI_CBRANCH ||
            opcode == ghidra::CPUI_CALL;

        int to_fill = 0;
        bool all_known = true;
        for (size_t at = names_a_space ? 1 : 0; at < form->reads.size(); ++at) {
            const Form::Piece &piece = form->reads[at];
            if (piece.is_slot)
                ++to_fill;
            else if (!piece.is_fixed)
                all_known = false;  // worked out while decoding: nothing to name
        }
        if (all_known && to_fill == inputs)
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
    std::vector<Wanted> places;
    for (const std::vector<std::string> &names : registers) {
        Wanted one;
        one.names = names;
        places.push_back(std::move(one));
    }
    return write_mixed(form, places, bytes, used, error);
}

bool Catalogue::write_mixed(const Form &form, const std::vector<Wanted> &places,
                            std::vector<uint8_t> &bytes, std::vector<std::string> &used,
                            std::string &error, const std::vector<int> &roles)
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

    // Each value goes in the first slot that can hold it and is not already
    // holding something.
    //
    // Taking slots and values strictly in step assumes a form names its places
    // in the same order the operation names its values, and forms do not: an
    // instruction that copies a number into a register may name the number
    // first or the register first, and both are the same instruction. So the
    // pairing is made rather than assumed, and each slot is used once.
    std::vector<bool> filled(form.slots.size(), false);
    // What each slot was actually given, by the name the slot knew it under.
    // A form may name one slot twice - a compressed two-operand add reads and
    // writes the same slot - and a second place asking for the register that is
    // already there is asking for something the bits already say.
    std::vector<std::string> holding(form.slots.size());
    const uint64_t may_touch = ~form.fixed_mask;

    // The displacements an address is made of are set to nought first, and are
    // not offered to anything else. An instruction that reads from a register
    // is the one that reads from that register plus nothing.
    for (int which : form.zeroed) {
        if (which < 0 || static_cast<size_t>(which) >= form.slots.size())
            continue;
        const Form::Slot &slot = form.slots[which];
        for (const Form::Slot::Way &way : slot.numbers) {
            uint64_t room = 0;
            uint64_t nothing = 0;
            if (!place_in_field(way.field, form.shortest, 0, room, nothing))
                continue;
            word &= ~(room & may_touch);
            word = (word & ~(way.along_mask & may_touch)) |
                   (way.along_bits & way.along_mask & may_touch);
            break;
        }
        word = (word & ~form.fixed_mask) | form.fixed_bits;
        filled[which] = true;
    }

    for (size_t next = 0; next < places.size(); ++next) {
        bool placed = false;

        // Where this place belongs, when the caller knows. A form says which
        // slot it writes to and which it reads from, so an answer and its
        // operands go where they mean rather than wherever they fit.
        size_t only = form.slots.size();
        if (next < roles.size()) {
            // A place the form does not read from a slot cannot be given one.
            //
            // Saying nothing about where it goes meant it went wherever it
            // would fit, so a register asked for as a shift amount was written
            // into whatever field was still free - and the instruction that
            // came out shifted by sixty-three. A form that has nowhere for a
            // value is a form that cannot be used for it.
            if (roles[next] < 0 || static_cast<size_t>(roles[next]) >= form.slots.size()) {
                error = "this instruction has nowhere for one of those values";
                return false;
            }
            only = static_cast<size_t>(roles[next]);
        }

        for (size_t which = 0; which < form.slots.size() && !placed; ++which) {
            if (only < form.slots.size() && which != only)
                continue;
            if (filled[which]) {
                // Unless the slot is already holding the very register being
                // asked for. A form may name one slot twice - a compressed
                // two-operand add reads and writes the same one - and then the
                // answer and an operand are the same place by construction.
                // Refusing the second was refusing every instruction of that
                // shape even where the caller had asked for exactly the
                // register the bits already select, and the bits need no
                // further change to say so.
                //
                // Only where it is the same register: a different one in an
                // occupied slot is a form that cannot say what was asked, and
                // writing it anyway would encode one register where two were
                // meant.
                if (places[next].is_number || holding[which].empty())
                    continue;
                bool already = false;
                for (const std::string &name : places[next].names) {
                    if (name == holding[which]) {
                        already = true;
                        break;
                    }
                }
                if (!already)
                    continue;
                used.push_back(holding[which]);
                placed = true;
                break;
            }
            const Form::Slot &slot = form.slots[which];

            if (places[next].is_number) {
                // A number goes in a field the form left open for one, put back
                // the way the processor would take it out. A number too big for
                // the field is not this instruction's.
                if (slot.is_register() || !slot.is_placed())
                    continue;
                const size_t which_way =
                    places[next].way >= 0 &&
                            static_cast<size_t>(places[next].way) < slot.numbers.size()
                        ? static_cast<size_t>(places[next].way)
                        : 0;
                const Form::Slot::Way &way = slot.numbers[which_way];
                uint64_t room = 0;
                uint64_t sitting = 0;
                if (!place_in_field(way.field, form.shortest, places[next].number, room, sitting,
                                    places[next].bytes))
                    continue;

                word = (word & ~(room & may_touch)) | (sitting & may_touch);
                word = (word & ~(way.along_mask & may_touch)) |
                       (way.along_bits & way.along_mask & may_touch);
                word = (word & ~form.fixed_mask) | form.fixed_bits;
                used.push_back(std::to_string(places[next].number));
                filled[which] = true;
                placed = true;
                continue;
            }

            if (!slot.is_register())
                continue;

            // Whichever of this register's names the slot knows.
            auto found = slot.registers.end();
            std::string name_used;
            for (const std::string &name : places[next].names) {
                found = slot.registers.find(name);
                if (found != slot.registers.end()) {
                    name_used = name;
                    break;
                }
            }
            if (found == slot.registers.end())
                continue;

            // The form's own bits win: a slot may only touch what the form did
            // not insist on, because what it insisted on is what makes the
            // instruction that instruction.
            //
            // The bits that choose are cleared and this register's own set;
            // what every choice agrees on is set whichever register it is.
            const Form::Slot::Choice &choice = found->second;
            const uint64_t value =
                choice.bits | (choice.along_bits & choice.along_mask);
            word &= ~(slot.register_mask & may_touch);
            word |= value & slot.register_mask & may_touch;
            word = (word & ~slot.always_mask) | (slot.always_bits & slot.always_mask);
            word = (word & ~form.fixed_mask) | form.fixed_bits;
            used.push_back(name_used);
            filled[which] = true;
            holding[which] = name_used;
            placed = true;
        }

        if (!placed) {
            if (places[next].is_number)
                error = "this instruction has no field for a number like that";
            else
                error = "this instruction cannot put " +
                        (places[next].names.empty() ? std::string("that")
                                                    : places[next].names.front()) +
                        " where it was asked to";
            return false;
        }
    }

    {
        const size_t next = places.size();
        if (next < places.size()) {
            error = "this instruction has fewer places to fill than it was given";
            return false;
        }
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
