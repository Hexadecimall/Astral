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
        if (wrote->getOpcode() != ghidra::CPUI_COPY || wrote->numInput() < 1)
            return Form::Piece();      // computed on the way: not a place
        value = wrote->getIn(0);
        before = at;
    }
    return Form::Piece();
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
        size_t only_at = 0;
        if (templ != nullptr) {
            size_t at = 0;
            for (const ghidra::OpTpl *operation : templ->getOpvec()) {
                if (operation != nullptr && !is_bookkeeping(operation->getOpcode())) {
                    form.does.push_back(operation->getOpcode());
                    only = operation;
                    only_at = at;
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

            if (ends_at != nullptr && ends_at->numInput() > 0) {
                // What produced the answer, with the moves followed through.
                const ghidra::OpTpl *doing = ends_at;
                size_t doing_at = ends_at_index;
                for (int steps = 0; steps < 8 && doing->getOpcode() == ghidra::CPUI_COPY &&
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

                bool nameable = doing != ends_at || doing->getOpcode() != ghidra::CPUI_COPY;
                std::vector<Form::Piece> reads;
                for (int input = 0; input < doing->numInput(); ++input) {
                    const Form::Piece piece =
                        through_moves(operations, doing_at, doing->getIn(input));
                    if (!piece.is_slot && !piece.is_fixed)
                        nameable = false;
                    reads.push_back(piece);
                }

                if (nameable) {
                    // The registers the rest of the template disturbs, so that
                    // choosing this form is a decision somebody can make.
                    std::vector<uint64_t> disturbed;
                    for (size_t at = 0; at < operations.size(); ++at) {
                        const ghidra::OpTpl *operation = operations[at];
                        if (operation == nullptr || operation == doing ||
                            operation == ends_at || is_bookkeeping(operation->getOpcode()))
                            continue;
                        const Form::Piece wrote = read_piece(operation->getOut());
                        if (wrote.is_fixed && wrote.fixed_is_register)
                            disturbed.push_back(wrote.fixed);
                    }

                    form.does.assign(1, doing->getOpcode());
                    form.writes = true;
                    form.writes_to = read_piece(ends_at->getOut());
                    form.reads = std::move(reads);
                    form.also_writes = std::move(disturbed);
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

        for (int slot = 0; slot < made->getNumOperands(); ++slot)
            form.slots.push_back(read_slot(made->getOperand(slot), form.shortest, by_offset));

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
            const int width = slot.last_bit - slot.first_bit + 1;
            const int shift = form.shortest * 8 - 1 - slot.last_bit;
            if (width <= 0 || width >= 64 || shift < 0)
                continue;
            form.fixed_mask &= ~(((static_cast<uint64_t>(1) << width) - 1)
                                 << static_cast<unsigned>(shift));
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
        int to_fill = 0;
        bool all_known = true;
        for (const Form::Piece &piece : form->reads) {
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

    // Each value goes in the first slot that can hold it and is not already
    // holding something.
    //
    // Taking slots and values strictly in step assumes a form names its places
    // in the same order the operation names its values, and forms do not: an
    // instruction that copies a number into a register may name the number
    // first or the register first, and both are the same instruction. So the
    // pairing is made rather than assumed, and each slot is used once.
    std::vector<bool> filled(form.slots.size(), false);
    const uint64_t may_touch = ~form.fixed_mask;

    for (size_t next = 0; next < places.size(); ++next) {
        bool placed = false;

        for (size_t which = 0; which < form.slots.size() && !placed; ++which) {
            if (filled[which])
                continue;
            const Form::Slot &slot = form.slots[which];

            if (places[next].is_number) {
                // A number goes in a field the form left open for one.
                if (slot.is_register() || !slot.is_placed())
                    continue;
                const int width = slot.last_bit - slot.first_bit + 1;
                const int shift = form.shortest * 8 - 1 - slot.last_bit;
                if (width <= 0 || width >= 64 || shift < 0)
                    continue;
                const uint64_t room = ((static_cast<uint64_t>(1) << width) - 1)
                                      << static_cast<unsigned>(shift);
                const uint64_t sitting =
                    (places[next].number << static_cast<unsigned>(shift)) & room;
                // A number too big for the field is not this instruction's.
                if ((sitting >> static_cast<unsigned>(shift)) != places[next].number)
                    continue;

                word = (word & ~(room & may_touch)) | (sitting & may_touch);
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
            word = (word & ~(slot.register_mask & may_touch)) | (found->second & may_touch);
            word |= slot.along_the_way_bits & may_touch;
            word = (word & ~form.fixed_mask) | form.fixed_bits;
            used.push_back(name_used);
            filled[which] = true;
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
