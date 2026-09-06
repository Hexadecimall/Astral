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

#include <cstdint>
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

    // Where one of an operation's values comes from.
    //
    // A form does not name registers. It says "whatever was written in the
    // second slot", and which register that is, is decided when an instruction
    // is written rather than when the form was. So a value is either one of the
    // form's own slots, which something has to be put into, or a fixed thing
    // the form always uses.
    struct Piece {
        bool is_slot = false;  // one of the form's operands, to be filled in
        int slot = -1;         // which one
        bool is_fixed = false; // a number the form always uses
        uint64_t fixed = 0;
    };

    // What the one operation writes to and reads from, when the form does
    // exactly one thing. This is the shape a selection has to match: an
    // addition whose two inputs are slots is a register-register add, and one
    // whose second input is fixed is an add of a particular amount.
    Piece writes_to;
    bool writes = false;
    std::vector<Piece> reads;

    // What it does, as the sequence of p-code operations its template holds.
    // One operation is the common case and the useful one; a form whose
    // template is longer does several things at once and is harder to choose
    // for, which is a reason to know how many there are.
    std::vector<ghidra::OpCode> does;

    // One of the places a form leaves open, and what can go in it.
    struct Slot {
        // Where in the instruction it is written, counted the way the bits
        // above are: from the top, over the bytes in the order they are
        // written. Both are -1 when it could not be worked out, which happens
        // for an operand built out of several pieces or one that stands for
        // another table.
        int first_bit = -1;
        int last_bit = -1;

        // The registers this slot can name, against the bits that name them.
        //
        // A slot holding a register is not a number written into a field: the
        // field picks one register out of a set, and which number picks which
        // register is the processor's business. So what is kept is the answer -
        // to use this register, put these bits in.
        //
        // Empty when the slot holds a number rather than a register.
        std::map<std::string, uint64_t> registers;
        // The bits those choices occupy, so writing one can clear them first.
        uint64_t register_mask = 0;

        bool is_register() const { return !registers.empty(); }
        bool is_placed() const { return first_bit >= 0 && last_bit >= first_bit; }
    };

    // Every place the form leaves open, in the order it names them - which is
    // the order the operations above refer to them by.
    std::vector<Slot> slots;

    // How many pieces of it are filled in when it is written: registers,
    // immediates, and anything else the form leaves open.
    int operands = 0;

    // The shortest it can be, in bytes. On a processor with one instruction
    // length this is that length; where forms differ, it is the first thing a
    // size budget would sort them by.
    int shortest = 0;

    // The bits this form always has, and which of them are fixed.
    //
    // An instruction is these bits with the slots filled in around them. Two
    // forms of the same operation differ here: on AARCH64 an add of two
    // registers and an add of a number are the same operation with different
    // fixed bits, and this is what says which is which.
    //
    // `fixed_mask` has a bit set wherever the form insists on a value, and
    // `fixed_bits` is what it insists on there. A zero mask means the bits
    // could not be worked out, which is said rather than passed off as a form
    // that insists on nothing.
    //
    // These are the instruction's BYTES in the order they are written, packed
    // with the first byte most significant - not the instruction read as a
    // number. On a big-endian processor those are the same thing and on a
    // little-endian one they are reversed, which is the difference between
    // these agreeing with a real instruction and agreeing with nothing.
    //
    // That was checked rather than assumed. A MIPS add comes out as
    // mask fc00003f over bits 00000020, which is what a MIPS add is; the one
    // beside it ends 21, which is what an unsigned MIPS add is. An AARCH64 add
    // ends 8b, which is what an AARCH64 add begins with, because AARCH64 writes
    // its bytes the other way round.
    //
    // What they are good for is narrowing: a real instruction agrees with very
    // few forms. Narrowing to exactly one, and filling the slots around these
    // bits, is what is still to do.
    uint64_t fixed_mask = 0;
    uint64_t fixed_bits = 0;

    // Whether reaching this form depended on something the processor already
    // knew rather than on the bits of the instruction.
    //
    // This is how a processor with more than one way of encoding instructions
    // says which one it is in. A form reached only under some such setting is a
    // real instruction, but only while the processor is in that mode: written
    // into a stream that is not, the same bytes mean something else entirely.
    // On MIPS the shortest way to add is two bytes rather than four, and those
    // two bytes in an ordinary MIPS program are a floating-point store.
    //
    // So a form that needs context is not simply shorter, and choosing it for
    // being shorter is choosing a different instruction.
    bool needs_context = false;

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

    // The forms that do this one operation with every value a slot - the
    // plainest shape there is, and the one a selection reaches for first: two
    // things in, one thing out, nothing about it fixed.
    //
    // Forms that only exist under some setting of the processor are left out
    // unless asked for, because they are not interchangeable with the ones that
    // always apply.
    std::vector<const Form *> plainly_doing(ghidra::OpCode opcode, int inputs,
                                            bool including_context = false) const;

    // The forms whose whole meaning is this one operation. These are the ones
    // worth choosing between: a form that does one thing can be selected for
    // that thing without reasoning about what else it did.
    const std::vector<const Form *> &doing(ghidra::OpCode opcode) const;

    // Every form, in the order the specification wrote them.
    const std::vector<Form> &all() const { return forms_; }

    // What was read, said the way a person would want to be told: how many
    // forms there are, and how many of them do exactly one thing.
    std::string summary() const;

    // Writes one instruction: the form's own bits, with the named registers put
    // in the slots that hold registers, in the order those slots come.
    //
    // This is the whole point of reading a specification rather than writing a
    // back end. Nothing here knows what a MIPS add looks like; it knows what
    // the file that describes MIPS says, and that is enough to write one.
    //
    // Returns false and says why when a slot cannot hold the register asked
    // for, which is a real answer: not every register can go in every place.
    static bool write(const Form &form, const std::vector<std::string> &registers,
                      std::vector<uint8_t> &bytes, std::string &error);

private:
    std::vector<Form> forms_;
    std::map<ghidra::OpCode, std::vector<const Form *>> by_operation_;
};

} // namespace catalogue
} // namespace nova
} // namespace astral_internal

#endif
