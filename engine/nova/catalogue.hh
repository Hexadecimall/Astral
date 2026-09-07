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
#include <set>
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
        bool is_fixed = false; // a place or a number the form always uses
        uint64_t fixed = 0;

        // Whether that fixed thing is a register rather than a number, since
        // the offset alone cannot say: a register a hundred and twenty-four
        // bytes into the register file and the number a hundred and
        // twenty-four are the same offset with nothing to tell them apart.
        bool fixed_is_register = false;
    };

    // What the form writes to and reads from. This is the shape a selection has
    // to match: an addition whose two inputs are slots is a register-register
    // add, and one whose second input is fixed is an add of a particular
    // amount.
    Piece writes_to;
    bool writes = false;
    std::vector<Piece> reads;

    // What it does.
    //
    // Almost no real instruction is one p-code operation. An AARCH64 add is
    // seven: two that move values into place, the addition itself, and four
    // that set the condition flags. A MIPS return is seven, six of which are
    // the instruction-set bit it clears on the way. Holding out for forms whose
    // whole template is one operation found the handful of clean ones on each
    // processor and missed most of what a processor can do.
    //
    // So this is what the form does in the sense that matters: the one
    // operation whose result reaches what the form writes, with the moves
    // either side of it followed through. What the rest of the template did is
    // below, in `also_writes`.
    std::vector<ghidra::OpCode> does;

    // The registers the form disturbs besides what it writes to.
    //
    // These are the flags, mostly. An instruction that adds also says whether
    // the answer was zero or negative or carried, and that is not optional -
    // it is what the instruction is. Choosing one is fine as long as nothing
    // being relied on lives in those registers, which is a question about the
    // function being written rather than about the form, so the answer is
    // recorded here and asked there.
    std::vector<uint64_t> also_writes;

    // And the slots it writes to besides the one it writes its answer to.
    //
    // An instruction that writes back changes the register it was given: `ldr
    // x6, [x7]!` reads through x7 and leaves x7 somewhere else. Which register
    // that is, is not known until one is put in the slot, so what is recorded
    // is the slot - and whether anything was relying on whatever went in it is
    // a question about the function, asked where the choosing happens.
    std::vector<int> also_writes_slots;

    // Slots that have to hold nought for the form to mean what it says.
    //
    // A load or a store does not take an address, it takes the pieces an
    // address is made of: a register and a displacement, added together inside
    // the instruction. Given an address already in a register, the instruction
    // that means "read from there" is that one with the displacement set to
    // nothing - so the register is the operand and the rest are noughts, and
    // saying which is what lets the form be chosen at all.
    std::vector<int> zeroed;

    // The question a branch asks, when it asks one itself.
    //
    // No processor computes a truth into a register in one instruction and none
    // needs to: the instruction that acts on a comparison is the comparison.
    // MIPS writes `beq rs, rt, somewhere`, whose template compares and then
    // branches on the answer, so what the form is choosable for is that
    // comparison rather than a branch on a truth somebody else worked out.
    // CPUI_COPY means it branches on a value it was given.
    ghidra::OpCode compares = ghidra::CPUI_COPY;

    // One of the places a form leaves open, and what can go in it.
    struct Slot {
        // Where in the instruction a number written here goes.
        //
        // Not a range of bits. A field is read by taking some of the
        // instruction's bytes in the order they are written, assembling them
        // the way its token assembles, and shifting - and on a processor whose
        // instructions are written least significant byte first those bytes are
        // not in the order the bits are. Describing it as a bit range assumed
        // they were, which put every AARCH64 register field one byte-swap away
        // from where it belongs: writing x1 into the destination set the bits
        // of the opcode instead.
        //
        // Unplaced when it could not be worked out, which happens for an
        // operand built out of several fields or one standing for another
        // table.
        struct Field {
            int first_byte = -1;      // the bytes it is in, as written
            int last_byte = -1;
            int shift = 0;            // where in them the value sits
            int width = 0;            // how many bits of value
            bool big_endian = false;  // how those bytes assemble

            bool is_placed() const { return first_byte >= 0 && width > 0; }
        };

        // One way of putting a number here, with what the way to it demands.
        //
        // An operand that takes a number is often a table rather than a field:
        // AARCH64 spells a constant with movz, whose operand is a table of
        // sixteen-bit fields shifted by different amounts, and RISC-V's immI is
        // a table of one. Each of those is a different field in different bits,
        // reached by different bits, so they are kept as alternatives rather
        // than merged.
        struct Way {
            Field field;
            uint64_t along_mask = 0;
            uint64_t along_bits = 0;
        };
        std::vector<Way> numbers;

        // One way of naming a register in this slot.
        //
        // A slot holding a register is not a number written into a field: the
        // field picks one register out of a set, and which number picks which
        // register is the processor's business. So what is kept is the answer -
        // to use this register, put these bits in.
        //
        // The way to it is kept with it rather than pooled. A slot is often a
        // table of tables, and different branches of it put the register in
        // different bits: reaching one branch demands bits that reaching
        // another forbids. Pooling those demands mixes two encodings that
        // cannot both hold, and writing a register then set bits belonging to
        // the path it did not take.
        struct Choice {
            uint64_t bits = 0;        // what picks this register
            uint64_t along_mask = 0;  // what the way to it insisted on
            uint64_t along_bits = 0;
        };

        // The registers this slot can name. Empty when it holds a number.
        std::map<std::string, Choice> registers;

        // The bits that choose, which are the bits the choices disagree in.
        //
        // That is what choosing means and it needs no other definition: a bit
        // every choice sets the same way is not choosing anything, it is part
        // of what the instruction is. Taking every bit a choice mentions
        // instead released the opcode along with the field and left the form
        // insisting on nothing at all.
        uint64_t register_mask = 0;

        // And what they all agree on, which the instruction has to have
        // whichever register goes in.
        uint64_t always_mask = 0;
        uint64_t always_bits = 0;

        bool is_register() const { return !registers.empty(); }
        bool is_placed() const { return !numbers.empty(); }
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

    // The registers this processor keeps ordinary values in.
    //
    // A calling convention has an opinion about the floating-point file too -
    // it says which of those are preserved and which are destroyed - so asking
    // it put d10 among the places a value might live, and nothing that adds can
    // name d10. Which registers it passes floats in does not settle it either:
    // the ones a call preserves are never named there, and d10 is one of those.
    //
    // The instruction set is the thing that knows. A register something can be
    // added into, or copied into, is a register a value can live in, and that
    // is read off the same specification as everything else here.
    std::set<uint64_t> registers_for_values(const ir::Target &target) const;

    // Where this processor keeps the address a function goes back to, when its
    // compiler specification does not say.
    //
    // It has to come from somewhere, because a value put in that register is a
    // return that goes to whatever the value was. AARCH64 names no return
    // address in any of its specifications, so nothing stopped x30 being handed
    // out as somewhere to keep a number, and seventeen of twenty recovered
    // functions worked out their answer in the register holding the way back
    // and then went there.
    //
    // The instruction set says it. A return reads the register the address is
    // in and takes no operand saying so, so the register its return forms name
    // outright is the one - and that is read off the specification rather than
    // known in advance. Only when they all name the SAME register: `jr` returns
    // through whichever register it is given, so MIPS and RISC-V name five
    // between them and mean nothing by it, and both of those say `ra` in the
    // specification that is asked first.
    bool return_through(const ir::Target &target, uint64_t &offset) const;

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

    // The same, where each place may go by more than one name.
    //
    // One register often has several: RISC-V calls the same one a0 and x10, and
    // a form lists it under whichever name that form was written with. So what
    // is offered per slot is every name the register goes by, and the slot uses
    // the one it knows. Trying every combination of names instead grows past
    // reason on a processor with many aliases, and picking per slot is the same
    // answer without the growth.
    static bool write_any(const Form &form,
                          const std::vector<std::vector<std::string>> &registers,
                          std::vector<uint8_t> &bytes, std::vector<std::string> &used,
                          std::string &error);

    // One place a value goes: a register under any of its names, or a number.
    //
    // Most instructions take both. An add of a register and a number is a
    // different form from an add of two registers, and the number goes into a
    // field of its own rather than into a register slot, so what is asked for
    // has to be able to say which it is.
    struct Wanted {
        std::vector<std::string> names;  // a register, under any of these
        bool is_number = false;
        uint64_t number = 0;

        // Whether that number is the value wanted or the bits to put in the
        // field. They are the same only when the operand is the field; where
        // one stands for a table that shifts or extends what it holds, what
        // goes in is not what comes out, and the caller works out which bits
        // give the value by asking the processor.
        bool is_raw_field = false;
        int way = 0;  // which of the slot's ways to use, when there are several
    };

    // Writes an instruction whose places may be registers or numbers.
    //
    // `roles`, when given, says which of the form's slots each place belongs in
    // - the form knows which slot it writes to and which it reads from, so an
    // answer and its operands need not be guessed at by trying slots until one
    // fits. Guessing gave `add x2, x1, x0` for an add of x1 and x2 into x0:
    // every register went somewhere it could go, and none went where it meant.
    // Empty leaves the pairing to be worked out, which is right for a caller
    // that only has registers and no roles for them.
    static bool write_mixed(const Form &form, const std::vector<Wanted> &places,
                            std::vector<uint8_t> &bytes, std::vector<std::string> &used,
                            std::string &error, const std::vector<int> &roles = {});

private:
    std::vector<Form> forms_;
    std::map<ghidra::OpCode, std::vector<const Form *>> by_operation_;
};

} // namespace catalogue
} // namespace nova
} // namespace astral_internal

#endif
