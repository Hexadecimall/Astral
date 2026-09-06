#include "catalogue.hh"

#include "semantics.hh"
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
struct Constraint {
    uint64_t mask = 0;
    uint64_t bits = 0;
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
            whole.mask |= pattern->getMask(0, 4, false);
            whole.bits |= pattern->getValue(0, 4, false);
        }
        found.emplace(made, whole);
    }

    // A node deciding on context is deciding on what the processor already
    // knew rather than on what is written, so it adds nothing to the bits an
    // instruction has to have.
    if (node->isContextDecision()) {
        for (int i = 0; i < node->numChildren(); ++i)
            walk(node->getChild(i), sofar, found);
        return;
    }

    const int start = node->getStartBit();
    const int size = node->getBitSize();
    for (int i = 0; i < node->numChildren(); ++i) {
        Constraint below = sofar;
        // Going to the i-th child is what happens when those bits are i.
        if (size > 0 && size < 64 && start >= 0 && start + size <= 64) {
            const uint64_t mask = ((static_cast<uint64_t>(1) << size) - 1)
                                  << static_cast<unsigned>(start);
            below.mask |= mask;
            below.bits |= (static_cast<uint64_t>(i) << static_cast<unsigned>(start)) & mask;
        }
        walk(node->getChild(i), below, found);
    }
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
            form.fixed_mask = pattern->second.mask;
            form.fixed_bits = pattern->second.bits & pattern->second.mask;
        }
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

std::vector<const Form *> Catalogue::plainly_doing(ghidra::OpCode opcode, int inputs) const
{
    std::vector<const Form *> plain;
    for (const Form *form : doing(opcode)) {
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
