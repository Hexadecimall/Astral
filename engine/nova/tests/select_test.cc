// Choosing instructions and writing them, checked by reading them back.
//
// Every claim here is settled by handing the bytes to the same reading the
// decompiler does. That is the only check worth making: bytes that look right
// and mean something else are exactly the failure this is trying to avoid, and
// nothing but the processor's own reading can tell the difference.
#include "catalogue.hh"
#include "homes.hh"
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

// One operation on its own, written for a processor.
bool write_one(const std::string &language_id, const pcode::Operation &operation,
               std::vector<uint8_t> &bytes, std::string &why)
{
    ir::Target target;
    if (!ir::Target::from_language_id(language_id, target, why) ||
        !target.read_specification(why))
        return false;
    catalogue::Catalogue catalogue;
    if (!catalogue.read(target, why))
        return false;

    pcode::Block block;
    block.identifier = 1;
    block.operations.push_back(operation);
    pcode::Sequence sequence;
    sequence.name = "one";
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
    return true;
}

// Going back from a function is not the only thing a processor calls a return,
// and the others are not interchangeable with it. MIPS has `deret`, which goes
// back to wherever a debug exception came from; its specification says it is a
// return in the same words `jr ra` is, it is the same four bytes long, and it
// is the only one of the two that does exactly one thing - so everything that
// sorts forms by shape or by size picks it. What tells them apart is the
// register the address is in, which the compiler specification names.
void check_a_return_goes_back_through_the_return_address()
{
    pcode::Operation leaving;
    leaving.opcode = ghidra::CPUI_RETURN;

    std::vector<uint8_t> bytes;
    std::string why;
    const bool wrote = write_one("MIPS:BE:32:default", leaving, bytes, why);

    // Whatever comes out, it must not be the debug-exception return: that
    // instruction is privileged, and a function ending in one does not go back
    // to its caller.
    std::string trouble;
    const std::string reads =
        wrote ? reads_as("MIPS:BE:32:default", bytes, trouble) : std::string();
    report(!wrote || reads.find("deret") == std::string::npos,
           "a return is not written as the instruction that returns from a debug exception",
           "it was written as: " + reads);

    // And when it cannot be written, the reason says the bytes do not mean a
    // return rather than only that nothing fitted - because meaning is what was
    // checked, and a reader who is told a form was refused wants to know that
    // the refusal came from asking the processor rather than from a rule here.
    report(wrote || why.find("do not mean a return") != std::string::npos,
           "and when none can be written the reason says the bytes do not mean a return", why);
}

// An instruction that names no register is checked by what it means.
//
// Reading bytes back as text settles whether the right registers went in the
// right fields, and for a return there are none to check - so any two bytes
// that decode at all pass, and the shortest wins. On MIPS the shortest is a
// coprocessor store, which reads back perfectly and is not a return. What tells
// them apart is what the processor says they do.
void check_meaning_is_asked_for_and_not_only_spelling()
{
    std::string trouble;

    // `jr ra` means a return that goes back through ra.
    const std::vector<Meaning> going_back =
        means_as("MIPS:BE:32:default", {0x03, 0xe0, 0x00, 0x08}, trouble);
    bool has_return = false;
    for (const Meaning &one : going_back)
        has_return = has_return || one.opcode == ghidra::CPUI_RETURN;
    report(has_return, "the processor says its return instruction means a return",
           going_back.empty() ? trouble : "it meant something else");

    // The two bytes that would have been chosen for being shortest mean a
    // store, and nothing about reading them back as text says so.
    const std::vector<Meaning> shorter =
        means_as("MIPS:BE:32:default", {0xe8, 0x2e}, trouble);
    bool shorter_returns = false;
    for (const Meaning &one : shorter)
        shorter_returns = shorter_returns || one.opcode == ghidra::CPUI_RETURN;
    report(!shorter_returns && !shorter.empty(),
           "and the shorter bytes that read back cleanly do not mean one",
           shorter.empty() ? trouble : "they meant a return after all");
}

// A form that names a register outright is still a form.
//
// A return reads the register the return address is in, and the instruction has
// no operand saying so - the form names it. Counting that as something the
// caller has to supply asked for one value too many, and requiring every read
// to be a slot threw the same forms away for a second reason. Either way every
// return on every processor was discarded.
void check_a_form_that_names_its_own_register_is_offered()
{
    ir::Target target;
    std::string why;
    if (!ir::Target::from_language_id("RISCV:LE:64:RV64GC", target, why) ||
        !target.read_specification(why))
        return;
    catalogue::Catalogue catalogue;
    if (!catalogue.read(target, why))
        return;

    const std::vector<const catalogue::Form *> forms =
        catalogue.plainly_doing(ghidra::CPUI_RETURN, 0, true);
    report(!forms.empty(), "a return that reads no operand is offered a form to be written as",
           "none of this processor's returns can be chosen");

    // And what those forms read is a register rather than a number, since the
    // offset alone cannot say which.
    bool any_register = false;
    for (const catalogue::Form *form : forms) {
        for (const catalogue::Form::Piece &piece : form->reads)
            any_register = any_register || (piece.is_fixed && piece.fixed_is_register);
    }
    report(any_register, "and the register it names is known to be a register",
           "the form's fixed read was not recognised as a place");
}

// A whole function, from what somebody wrote to bytes the processor agrees
// with.
//
// Each piece of this is checked on its own elsewhere. What is checked here is
// that they meet: a function that answers with something takes room for nothing,
// works out its answer, puts it where a caller will look, and goes back through
// the link register - and every instruction of it is one this processor has.
void check_a_whole_function()
{
    ir::Target target;
    std::string why;
    if (!ir::Target::from_language_id("MIPS:BE:32:default", target, why) ||
        !target.read_specification(why)) {
        report(false, "the processor is read", why);
        return;
    }
    catalogue::Catalogue catalogue;
    if (!catalogue.read(target, why)) {
        report(false, "its instructions are read", why);
        return;
    }

    ir::Builder builder("sum");
    builder.block();
    builder.ret(builder.binary(ir::Operation::Add, builder.constant(20, 4),
                               builder.constant(22, 4), 4, true));

    ir::Function function;
    std::vector<std::string> problems;
    if (!builder.finish(function, problems)) {
        report(false, "a function that adds and answers is built",
               problems.empty() ? "" : problems.front());
        return;
    }

    pcode::Sequence sequence;
    if (!pcode::to_pcode(function, target, sequence, problems) ||
        !homes::give(sequence, target, problems)) {
        report(false, "and written as p-code with every value housed",
               problems.empty() ? "" : problems.front());
        return;
    }

    std::vector<select::Chosen> chosen;
    const bool clean = select::write(sequence, target, catalogue, chosen, problems);
    report(clean, "every operation of a whole function is written",
           problems.empty() ? "" : problems.front());

    // And the processor reads every one of them back as an instruction, the
    // last of which goes back through the link register.
    bool all_real = !chosen.empty();
    std::string last;
    for (const select::Chosen &one : chosen) {
        std::string trouble;
        last = reads_as("MIPS:BE:32:default", one.bytes, trouble);
        all_real = all_real && !last.empty();
    }
    report(all_real, "and the processor reads each of them back as an instruction",
           last.empty() ? "one was not an instruction" : last);
    report(last.find("jr") != std::string::npos && last.find("ra") != std::string::npos,
           "and the last of them goes back through the link register", last);
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
    check_a_return_goes_back_through_the_return_address();
    check_a_form_that_names_its_own_register_is_offered();
    check_meaning_is_asked_for_and_not_only_spelling();
    check_a_whole_function();

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
