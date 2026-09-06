// Reading what a processor's instructions mean out of its own specification.
//
// The claim being checked is not that some number came back. It is that the
// numbers differ between processors in the ways those processors actually
// differ, because that is the difference between reading a specification and
// producing something that merely looks like an answer.
#include "catalogue.hh"
#include "ir.hh"

#include "session.hh"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace astral_internal;
using namespace astral_internal::nova;

namespace {

int passed = 0;
int failed = 0;

void report(bool ok, const std::string &what, const std::string &saw)
{
    if (ok) {
        ++passed;
        std::printf("ok   %s\n", what.c_str());
        return;
    }
    ++failed;
    std::printf("FAIL %s\n     %s\n", what.c_str(), saw.c_str());
}

bool catalogue_for(const std::string &language_id, catalogue::Catalogue &out)
{
    ir::Target target;
    std::string error;
    if (!ir::Target::from_language_id(language_id, target, error)) {
        report(false, "reads " + language_id, error);
        return false;
    }
    if (!out.read(target, error)) {
        report(false, "reads the instructions of " + language_id, error);
        return false;
    }
    return true;
}

// The shortest any of the forms doing this operation can be written in.
int shortest_doing(const catalogue::Catalogue &catalogue, ghidra::OpCode opcode)
{
    int shortest = 0;
    for (const catalogue::Form *form : catalogue.doing(opcode)) {
        if (shortest == 0 || form->shortest < shortest)
            shortest = form->shortest;
    }
    return shortest;
}

void check_a_processor_is_read()
{
    catalogue::Catalogue arm;
    if (!catalogue_for("AARCH64:LE:64:AppleSilicon", arm))
        return;

    report(arm.size() > 1000, "a real processor has a great many instruction forms",
           std::to_string(arm.size()) + " forms");
    report(!arm.doing(ghidra::CPUI_INT_ADD).empty(), "and some of them add",
           std::to_string(arm.doing(ghidra::CPUI_INT_ADD).size()) + " forms add");

    // Every form indexed by an operation does exactly that operation and
    // nothing else, which is what makes it choosable without reasoning about
    // what else it did.
    bool all_single = true;
    for (const catalogue::Form *form : arm.doing(ghidra::CPUI_INT_ADD))
        all_single = all_single && form->does.size() == 1;
    report(all_single, "a form indexed by an operation does only that operation", "one did more");

    // An instruction takes room. A form claiming to fit in nothing is a form
    // that was not read properly.
    bool all_sized = true;
    for (const catalogue::Form *form : arm.doing(ghidra::CPUI_INT_ADD))
        all_sized = all_sized && form->shortest > 0;
    report(all_sized, "and takes some room to write", "one claimed to take none");
}

// Every instruction on this processor is four bytes, and on that one they are
// not. Reading a specification gets that right; anything assuming a width does
// not.
void check_lengths_are_the_processors_own()
{
    catalogue::Catalogue arm;
    if (catalogue_for("AARCH64:LE:64:AppleSilicon", arm)) {
        report(shortest_doing(arm, ghidra::CPUI_INT_ADD) == 4,
               "an AARCH64 instruction is four bytes, and adding is no exception",
               std::to_string(shortest_doing(arm, ghidra::CPUI_INT_ADD)) + " bytes");
    }

    // A processor with a compressed encoding can add in two bytes, which is the
    // whole reason a size budget has anything to choose between.
    catalogue::Catalogue riscv;
    if (catalogue_for("RISCV:LE:64:default", riscv)) {
        const int shortest = shortest_doing(riscv, ghidra::CPUI_INT_ADD);
        report(shortest > 0 && shortest < 4,
               "a RISC-V with compressed instructions can add in fewer than four bytes",
               std::to_string(shortest) + " bytes");
        report(riscv.doing(ghidra::CPUI_INT_ADD).size() > 1,
               "and has more than one way to do it, which is what there is to choose between",
               std::to_string(riscv.doing(ghidra::CPUI_INT_ADD).size()) + " forms");
    }

    // And one whose instructions are as short as a byte.
    catalogue::Catalogue small;
    if (catalogue_for("z80:LE:16:default", small)) {
        report(shortest_doing(small, ghidra::CPUI_INT_ADD) == 1,
               "a z80 adds in a single byte", std::to_string(shortest_doing(small, ghidra::CPUI_INT_ADD)) + " bytes");
        report(small.size() < 500, "and has few forms altogether, being a small processor",
               std::to_string(small.size()) + " forms");
    }
}

// A processor without an instruction does not have one, and the reading has to
// say so rather than finding something close.
void check_what_a_processor_has_not_got()
{
    catalogue::Catalogue small;
    if (!catalogue_for("z80:LE:16:default", small))
        return;

    // The z80 cannot multiply. Nothing in it does, and a catalogue that claimed
    // otherwise would be choosing an instruction that does not exist.
    report(small.doing(ghidra::CPUI_INT_MULT).empty(),
           "a z80 has no instruction that multiplies, and none is offered",
           std::to_string(small.doing(ghidra::CPUI_INT_MULT).size()) + " were offered");

    catalogue::Catalogue arm;
    if (catalogue_for("AARCH64:LE:64:AppleSilicon", arm)) {
        report(!arm.doing(ghidra::CPUI_INT_MULT).empty(),
               "while a processor that can multiply offers something that does",
               "it offered nothing");
    }
}

// Reading it twice gives the same answer, because the specification is kept
// rather than read again.
void check_reading_twice()
{
    catalogue::Catalogue first;
    catalogue::Catalogue second;
    if (!catalogue_for("RISCV:LE:64:default", first))
        return;
    if (!catalogue_for("RISCV:LE:64:default", second))
        return;
    report(first.size() == second.size() &&
               first.doing(ghidra::CPUI_INT_ADD).size() ==
                   second.doing(ghidra::CPUI_INT_ADD).size(),
           "reading a processor twice says the same thing both times",
           std::to_string(first.size()) + " against " + std::to_string(second.size()));
}


// The shape of a form, which is what a selection has to match.
//
// These ask what a processor can do at all, so forms that are real only under
// some setting of it are included. Which of those may actually be written into
// a particular program is the selector's question, not this one's.
//
// A form does not name registers. It says "whatever was written in the second
// slot", and which register that is, is decided when an instruction is written.
// So the question a selection asks is not "does this add" but "does this add
// two things I can choose and put the answer where I choose".
void check_shapes()
{
    catalogue::Catalogue arm;
    if (catalogue_for("AARCH64:LE:64:AppleSilicon", arm)) {
        const std::vector<const catalogue::Form *> plain =
            arm.plainly_doing(ghidra::CPUI_INT_ADD, 2, true);
        report(!plain.empty(), "a register machine adds two things it is given into a third",
               std::to_string(plain.size()) + " such forms");

        // Every piece of a plain form is a slot, which is what plain means.
        bool all_slots = true;
        for (const catalogue::Form *form : plain) {
            all_slots = all_slots && form->writes && form->writes_to.is_slot;
            for (const catalogue::Form::Piece &piece : form->reads)
                all_slots = all_slots && piece.is_slot;
        }
        report(all_slots, "and every part of such a form is something to be filled in",
               "one part was not");

        // Copying takes one thing, not two, and asking for the wrong number
        // finds nothing rather than something close.
        report(arm.plainly_doing(ghidra::CPUI_COPY, 1, true).size() > 0 &&
                   arm.plainly_doing(ghidra::CPUI_COPY, 2, true).empty(),
               "copying reads one thing, and asking for two finds none",
               std::to_string(arm.plainly_doing(ghidra::CPUI_COPY, 2, true).size()) + " with two");
    }

    // An accumulator machine adds into a particular register rather than into
    // one it is told. It has forms that add, and none of them is plain - which
    // is the difference a selection has to cope with, and a catalogue that
    // reported them as plain would be choosing an instruction that cannot put
    // the answer where it was asked to.
    catalogue::Catalogue small;
    if (catalogue_for("z80:LE:16:default", small)) {
        report(!small.doing(ghidra::CPUI_INT_ADD).empty(),
               "a z80 has instructions that add", "it has none");
        report(small.plainly_doing(ghidra::CPUI_INT_ADD, 2, true).empty(),
               "but none that adds into a register it is told, because it adds into one register",
               std::to_string(small.plainly_doing(ghidra::CPUI_INT_ADD, 2, true).size()) + " were plain");
    }

    // A compressed encoding shows up as a plain form that is shorter, which is
    // what a size budget picks.
    catalogue::Catalogue riscv;
    if (catalogue_for("RISCV:LE:64:default", riscv)) {
        int shortest = 0;
        for (const catalogue::Form *form : riscv.plainly_doing(ghidra::CPUI_INT_ADD, 2, true)) {
            if (shortest == 0 || form->shortest < shortest)
                shortest = form->shortest;
        }
        report(shortest == 2, "a compressed encoding is a plain form that takes two bytes",
               std::to_string(shortest) + " bytes");
    }
}


// The bits a form insists on. These come off the tree that decides which form a
// stream of bits means, accumulated from the root down, because the pattern at
// the bottom holds only what was left to distinguish by then.
//
// What is claimed here is only what has been checked: that forms carry bits,
// and that forms of the same operation carry different ones. Whether those bits
// in that order are what a processor would actually accept has not been checked
// and is not asserted.
void check_fixed_bits()
{
    catalogue::Catalogue arm;
    if (catalogue_for("AARCH64:LE:64:AppleSilicon", arm)) {
        size_t known = 0;
        for (const catalogue::Form &form : arm.all()) {
            if (form.fixed_mask != 0)
                ++known;
        }
        report(known > arm.size() - 5,
               "nearly every form says which bits it insists on",
               std::to_string(known) + " of " + std::to_string(arm.size()));

        // What a form insists on has to be within what it insists about.
        bool consistent = true;
        for (const catalogue::Form &form : arm.all())
            consistent = consistent && (form.fixed_bits & ~form.fixed_mask) == 0;
        report(consistent, "and insists on nothing outside the bits it named",
               "one insisted on a bit it had not named");
    }

    // Two forms of the same operation are two forms because they differ, and if
    // they did not differ here there would be no telling them apart.
    catalogue::Catalogue riscv;
    if (catalogue_for("RISCV:LE:64:default", riscv)) {
        const std::vector<const catalogue::Form *> adding =
            riscv.plainly_doing(ghidra::CPUI_INT_ADD, 2, true);
        bool all_different = true;
        for (size_t i = 0; i < adding.size(); ++i) {
            for (size_t j = i + 1; j < adding.size(); ++j) {
                if (adding[i]->fixed_mask == adding[j]->fixed_mask &&
                    adding[i]->fixed_bits == adding[j]->fixed_bits)
                    all_different = false;
            }
        }
        report(adding.size() > 1 && all_different,
               "two ways of adding on one processor are told apart by their bits",
               all_different ? std::to_string(adding.size()) + " ways" : "two were identical");
    }
}


// The bits, against instructions that are written down in the world.
//
// This is the check that says the reading is right rather than merely
// self-consistent: a form's bits have to agree with an instruction somebody
// else wrote, and a big-endian processor and a little-endian one have to both
// come out right, since that is where getting it wrong would show.
void check_bits_against_real_instructions()
{
    // MIPS adds with the SPECIAL opcode and a function code of twenty in hex,
    // and adds unsigned with twenty-one. Both are written down in every MIPS
    // manual, and both are big-endian, so the bytes and the number agree.
    catalogue::Catalogue mips;
    if (catalogue_for("MIPS:BE:32:default", mips)) {
        bool found_add = false;
        bool found_addu = false;
        for (const catalogue::Form *form : mips.plainly_doing(ghidra::CPUI_INT_ADD, 2, true)) {
            if (form->fixed_mask == 0xfc00003full && form->fixed_bits == 0x00000020ull)
                found_add = true;
            if (form->fixed_mask == 0xfc00003full && form->fixed_bits == 0x00000021ull)
                found_addu = true;
        }
        report(found_add, "a MIPS add is the SPECIAL opcode and a function code of twenty",
               "no form said so");
        report(found_addu, "and an unsigned one is twenty-one", "no form said so");
    }

    // AARCH64 writes its bytes the other way round, so an add whose word begins
    // 8b has 8b as its first byte and therefore last in a number read this way.
    // A real add has to agree with something, and with very few things.
    catalogue::Catalogue arm;
    if (catalogue_for("AARCH64:LE:64:AppleSilicon", arm)) {
        // add x0, x1, x2, as bytes in the order they are written.
        const uint64_t written = 0x2000028bull;
        int agreeing = 0;
        for (const catalogue::Form &form : arm.all()) {
            if (form.fixed_mask != 0 && (written & form.fixed_mask) == form.fixed_bits)
                ++agreeing;
        }
        report(agreeing > 0 && agreeing < 20,
               "a real AARCH64 add agrees with a few forms and not with thousands",
               std::to_string(agreeing) + " forms agreed");

        // And something that is not an instruction at all should agree with
        // very little, or the bits are not saying anything.
        int nonsense = 0;
        for (const catalogue::Form &form : arm.all()) {
            if (form.fixed_mask != 0 && (0xffffffffull & form.fixed_mask) == form.fixed_bits)
                ++nonsense;
        }
        report(nonsense < agreeing * 40,
               "and the bits narrow things down rather than agreeing with everything",
               std::to_string(nonsense) + " agreed with all-ones");
    }
}


// Writing an instruction, which is the whole point of reading a specification
// rather than writing a back end.
//
// Nothing here knows what a MIPS add looks like. It knows what the file that
// describes MIPS says, and the instruction that comes out is compared against
// the one a MIPS manual gives - so this is checked against the world rather
// than against itself.
void check_writing_an_instruction()
{
    catalogue::Catalogue mips;
    if (!catalogue_for("MIPS:BE:32:default", mips))
        return;

    const catalogue::Form *adding = nullptr;
    for (const catalogue::Form *form : mips.plainly_doing(ghidra::CPUI_INT_ADD, 2, true)) {
        if (form->fixed_mask == 0xfc00003full && form->fixed_bits == 0x00000020ull) {
            adding = form;
            break;
        }
    }
    if (adding == nullptr) {
        report(false, "the form that adds two registers is there", "it was not found");
        return;
    }

    std::vector<uint8_t> bytes;
    std::string error;
    const bool wrote = catalogue::Catalogue::write(*adding, {"a0", "a1", "a2"}, bytes, error);
    if (!wrote) {
        report(false, "an add of three named registers can be written", error);
        return;
    }

    // add a0, a1, a2 is 00 a6 20 20, which is what a MIPS manual says it is.
    const std::vector<uint8_t> wanted = {0x00, 0xa6, 0x20, 0x20};
    std::string got;
    for (uint8_t one : bytes) {
        char pair[4];
        std::snprintf(pair, sizeof pair, "%02x", one);
        got += pair;
    }
    report(bytes == wanted, "an add of three named registers comes out as the bytes it should",
           got + " rather than 00a62020");

    // Asking for a register the slot cannot hold is refused rather than written
    // as something else.
    {
        std::vector<uint8_t> nowhere;
        std::string why;
        const bool allowed =
            catalogue::Catalogue::write(*adding, {"nonesuch", "a1", "a2"}, nowhere, why);
        report(!allowed && !why.empty(),
               "a register the instruction cannot hold is refused rather than written as another",
               allowed ? "it was written anyway" : why);
    }

    // And giving it more registers than it has places for.
    {
        std::vector<uint8_t> nowhere;
        std::string why;
        const bool allowed = catalogue::Catalogue::write(
            *adding, {"a0", "a1", "a2", "a3", "t0", "t1", "t2"}, nowhere, why);
        report(!allowed, "and more registers than it has places for is refused too",
               allowed ? "it was written anyway" : why);
    }
}

} // namespace

int main()
{
    if (initialize(nullptr) != ASTRAL_OK) {
        std::printf("FAIL no compiled specifications were found; set ASTRAL_SPECS\n");
        return 1;
    }

    check_a_processor_is_read();
    check_lengths_are_the_processors_own();
    check_what_a_processor_has_not_got();
    check_reading_twice();
    check_shapes();
    check_fixed_bits();
    check_bits_against_real_instructions();
    check_writing_an_instruction();

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
