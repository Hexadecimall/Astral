// What a processor's instructions mean, read out of its own specification.
//
// This is the half of the specification the decompiler uses backwards. Every
// instruction form a processor has is written down once, as a bit pattern and
// as what it does in p-code. Reading bytes uses the pattern to find the form
// and then reports the meaning. Writing bytes is the same table read the other
// way: given a meaning, which forms produce it.
//
// So nothing here is a back end for any processor. It is a reading of the file
// that describes one, and the same reading works for every processor a
// specification exists for - which is every processor the decompiler can
// already take apart.
//
// What this does not do is choose. Knowing that a machine has eleven forms that
// add is not the same as knowing which to use, and picking between them is
// where a size budget and pinned registers come in. This only says what there
// is to pick from.
#ifndef ASTRAL_NOVA_CATALOGUE_HH
#define ASTRAL_NOVA_CATALOGUE_HH

#include "ir.hh"
#include "opcodes.hh"

#include <map>
#include <string>
#include <vector>

namespace astral_internal {
namespace nova {
namespace catalogue {

// One instruction form, as the specification describes it.
struct Form {
    // A form has no name here. What a person would call it is worked out while
    // one is being printed, from an instance that has had its operands filled
    // in, and there is no instance yet - only the form. Where it was written is
    // recorded instead, which is enough to go and look.

    // What it does, as the sequence of p-code operations its template holds.
    // One operation is the common case and the useful one; a form whose
    // template is longer does several things at once and is harder to choose
    // for, which is a reason to know how many there are.
    std::vector<ghidra::OpCode> does;

    // How many pieces of it are filled in when it is written: registers,
    // immediates, and anything else the form leaves open.
    int operands = 0;

    // The shortest it can be, in bytes. On a processor with one instruction
    // length this is that length; where forms differ, it is the first thing a
    // size budget would sort them by.
    int shortest = 0;

    // Where in the specification it was written, so a form that behaves oddly
    // can be looked at rather than guessed about.
    int line = 0;
};

// Every form a processor has, indexed by what it does.
class Catalogue {
public:
    // Reads the forms out of `target`'s specification. Returns false and says
    // why when the specification cannot be read.
    bool read(const ir::Target &target, std::string &error);

    // How many forms were read.
    size_t size() const { return forms_.size(); }

    // The forms whose whole meaning is this one operation. These are the ones
    // worth choosing between: a form that does one thing can be selected for
    // that thing without reasoning about what else it did.
    const std::vector<const Form *> &doing(ghidra::OpCode opcode) const;

    // Every form, in the order the specification wrote them.
    const std::vector<Form> &all() const { return forms_; }

    // What was read, said the way a person would want to be told: how many
    // forms there are, and how many of them do exactly one thing.
    std::string summary() const;

private:
    std::vector<Form> forms_;
    std::map<ghidra::OpCode, std::vector<const Form *>> by_operation_;
};

} // namespace catalogue
} // namespace nova
} // namespace astral_internal

#endif
