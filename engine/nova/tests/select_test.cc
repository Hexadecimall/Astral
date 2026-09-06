// Choosing instructions and writing them, checked by reading them back.
//
// Every claim here is settled by handing the bytes to the same reading the
// decompiler does. That is the only check worth making: bytes that look right
// and mean something else are exactly the failure this is trying to avoid, and
// nothing but the processor's own reading can tell the difference.
#include "catalogue.hh"
#include "ir.hh"
#include "pcode.hh"
#include "select.hh"
#include "specification.hh"

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

// One operation over three named registers, written for a processor.
bool write_add(const std::string &language_id, const std::string &into, const std::string &left,
               const std::string &right, std::vector<uint8_t> &bytes, int &length,
               std::string &why)
{
    ir::Target target;
    if (!ir::Target::from_language_id(language_id, target, why))
        return false;
    if (!target.read_specification(why))
        return false;

    catalogue::Catalogue catalogue;
    if (!catalogue.read(target, why))
        return false;

    auto named = [&](const std::string &name, pcode::Varnode &node) {
        const ir::Target::RegisterPlace *place = target.register_place(name);
        if (place == nullptr)
            return false;
        node.where = pcode::Where::Register;
        node.offset = place->offset;
        node.size = place->width;
        return true;
    };

    pcode::Operation adding;
    adding.opcode = ghidra::CPUI_INT_ADD;
    adding.writes = true;
    pcode::Varnode one;
    pcode::Varnode two;
    if (!named(into, adding.output) || !named(left, one) || !named(right, two)) {
        why = "this processor has no such register";
        return false;
    }
    adding.inputs.push_back(one);
    adding.inputs.push_back(two);

    pcode::Block block;
    block.identifier = 1;
    block.operations.push_back(adding);

    pcode::Sequence sequence;
    sequence.name = "adds";
    sequence.entry = 1;
    sequence.blocks.push_back(block);

    std::vector<select::Chosen> chosen;
    std::vector<std::string> problems;
    select::write(sequence, target, catalogue, chosen, problems);
    if (chosen.empty()) {
        why = problems.empty() ? "nothing came out" : problems.front();
        return false;
    }
    bytes = chosen.front().bytes;
    length = chosen.front().form->shortest;
    return true;
}

// An add of three registers on MIPS, written and then read back.
void check_an_instruction_is_written()
{
    std::vector<uint8_t> bytes;
    int length = 0;
    std::string why;
    if (!write_add("MIPS:BE:32:default", "a0", "a1", "a2", bytes, length, why)) {
        report(false, "an add of three registers is written", why);
        return;
    }

    // What a MIPS manual says it is.
    const std::vector<uint8_t> wanted = {0x00, 0xa6, 0x20, 0x20};
    report(bytes == wanted, "an add of three registers comes out as the bytes it should",
           std::to_string(bytes.size()) + " bytes, first " +
               (bytes.empty() ? "none" : std::to_string(bytes.front())));

    // And the processor agrees, which is the check that matters.
    std::string trouble;
    const std::string reads = reads_as("MIPS:BE:32:default", bytes, trouble);
    report(reads.find("add") != std::string::npos && reads.find("a0") != std::string::npos &&
               reads.find("a1") != std::string::npos && reads.find("a2") != std::string::npos,
           "and the processor reads them back as that add",
           reads.empty() ? trouble : reads);
}

// The shortest way is not always the right way, and this is the case that
// proves it. MIPS can add in two bytes, but only while it is in another mode;
// those two bytes in an ordinary MIPS program are a floating-point store.
void check_shorter_is_not_taken_when_it_means_something_else()
{
    ir::Target target;
    std::string why;
    if (!ir::Target::from_language_id("MIPS:BE:32:default", target, why))
        return;
    catalogue::Catalogue catalogue;
    if (!catalogue.read(target, why))
        return;

    // There is a shorter form, and it does say it adds.
    int shortest = 0;
    for (const catalogue::Form *form : catalogue.plainly_doing(ghidra::CPUI_INT_ADD, 2, true)) {
        if (shortest == 0 || form->shortest < shortest)
            shortest = form->shortest;
    }
    report(shortest == 2, "this processor has a two-byte way of adding",
           std::to_string(shortest) + " bytes");

    // And it was not taken, because it does not read back as an add.
    std::vector<uint8_t> bytes;
    int length = 0;
    if (!write_add("MIPS:BE:32:default", "a0", "a1", "a2", bytes, length, why)) {
        report(false, "and something was still written", why);
        return;
    }
    report(length == 4,
           "and the four-byte way was taken instead, because the shorter one means something else",
           std::to_string(length) + " bytes were chosen");
}

// A register a processor has not got is refused rather than written as some
// other register that happens to fit.
void check_a_register_it_has_not_got()
{
    std::vector<uint8_t> bytes;
    int length = 0;
    std::string why;
    const bool wrote = write_add("MIPS:BE:32:default", "nonesuch", "a1", "a2", bytes, length, why);
    report(!wrote && !why.empty(), "a register this processor has not got is refused",
           wrote ? "it was written anyway" : why);
}

// A value with nowhere to live cannot be named in an instruction, and saying so
// is the honest answer: giving it somewhere is register allocation, which is
// work that has not been done rather than something this can invent.
void check_a_value_with_no_home()
{
    ir::Target target;
    std::string why;
    if (!ir::Target::from_language_id("MIPS:BE:32:default", target, why) ||
        !target.read_specification(why))
        return;
    catalogue::Catalogue catalogue;
    if (!catalogue.read(target, why))
        return;

    pcode::Operation adding;
    adding.opcode = ghidra::CPUI_INT_ADD;
    adding.writes = true;
    adding.output.where = pcode::Where::Unique;  // nowhere yet
    adding.output.size = 4;
    pcode::Varnode one;
    one.where = pcode::Where::Unique;
    one.size = 4;
    adding.inputs.push_back(one);
    adding.inputs.push_back(one);

    pcode::Block block;
    block.identifier = 1;
    block.operations.push_back(adding);
    pcode::Sequence sequence;
    sequence.name = "homeless";
    sequence.entry = 1;
    sequence.blocks.push_back(block);

    std::vector<select::Chosen> chosen;
    std::vector<std::string> problems;
    const bool clean = select::write(sequence, target, catalogue, chosen, problems);
    report(!clean && chosen.empty() && !problems.empty(),
           "a value with nowhere to live is refused rather than given somewhere invented",
           problems.empty() ? "it was written" : problems.front());
    report(!problems.empty() && problems.front().find("no home") != std::string::npos,
           "and the reason says so", problems.empty() ? "" : problems.front());
}

} // namespace

int main()
{
    if (initialize(nullptr) != ASTRAL_OK) {
        std::printf("FAIL no compiled specifications were found; set ASTRAL_SPECS\n");
        return 1;
    }

    check_an_instruction_is_written();
    check_shorter_is_not_taken_when_it_means_something_else();
    check_a_register_it_has_not_got();
    check_a_value_with_no_home();

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
