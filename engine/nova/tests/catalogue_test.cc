// Reading what a processor's instructions mean out of its own specification.
//
// The claim being checked is not that some number came back. It is that the
// numbers differ between processors in the ways those processors actually
// differ, because that is the difference between reading a specification and
// producing something that merely looks like an answer.
#include "catalogue.hh"
#include "ir.hh"

#include "session.hh"

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

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
