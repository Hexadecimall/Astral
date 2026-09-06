// Giving values somewhere to live.
//
// A value with no home is ordinary in the representation and impossible in an
// instruction, so something has to bridge the two. What is checked here is that
// it does, that it does not put values in registers that mean something else,
// and that it leaves alone the ones somebody already placed.
#include "homes.hh"
#include "ir.hh"
#include "pcode.hh"

#include "session.hh"

#include <cstdio>
#include <set>
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

pcode::Varnode homeless(uint64_t which, int size)
{
    pcode::Varnode node;
    node.where = pcode::Where::Unique;
    node.offset = which;
    node.size = size;
    return node;
}

// Three values with no home: two added into a third.
pcode::Sequence adding()
{
    pcode::Operation add;
    add.opcode = ghidra::CPUI_INT_ADD;
    add.writes = true;
    add.output = homeless(8, 4);
    add.inputs.push_back(homeless(0, 4));
    add.inputs.push_back(homeless(4, 4));

    pcode::Operation leaving;
    leaving.opcode = ghidra::CPUI_RETURN;
    leaving.inputs.push_back(homeless(8, 4));

    pcode::Block block;
    block.identifier = 1;
    block.operations.push_back(add);
    block.operations.push_back(leaving);

    pcode::Sequence sequence;
    sequence.name = "adds";
    sequence.entry = 1;
    sequence.blocks.push_back(block);
    return sequence;
}

bool target_for(const std::string &language_id, ir::Target &target)
{
    std::string error;
    if (!ir::Target::from_language_id(language_id, target, error)) {
        report(false, "reads " + language_id, error);
        return false;
    }
    if (!target.read_specification(error)) {
        report(false, "reads the specification of " + language_id, error);
        return false;
    }
    return true;
}

void check_everything_gets_somewhere()
{
    ir::Target target;
    if (!target_for("MIPS:BE:32:default", target))
        return;

    pcode::Sequence sequence = adding();
    std::vector<std::string> problems;
    if (!homes::give(sequence, target, problems)) {
        report(false, "three values are given somewhere to live",
               problems.empty() ? "" : problems.front());
        return;
    }

    bool all_placed = true;
    std::set<uint64_t> where;
    for (const pcode::Block &block : sequence.blocks) {
        for (const pcode::Operation &operation : block.operations) {
            if (operation.writes) {
                all_placed = all_placed && operation.output.where == pcode::Where::Register;
                where.insert(operation.output.offset);
            }
            for (const pcode::Varnode &input : operation.inputs) {
                all_placed = all_placed && input.where == pcode::Where::Register;
                where.insert(input.offset);
            }
        }
    }
    report(all_placed, "every value ends up in a register", "one did not");

    // Two values alive at once cannot share, so an add of two things into a
    // third needs three places.
    report(where.size() == 3, "and values alive at the same time get different ones",
           std::to_string(where.size()) + " places for three values");
}

// The registers a processor keeps for something in particular are not free.
// Choosing them by address picked a thread pointer and a floating-point control
// word, and an instruction written over one of those is not one anybody meant.
void check_only_ordinary_registers_are_used()
{
    ir::Target target;
    if (!target_for("MIPS:BE:32:default", target))
        return;

    pcode::Sequence sequence = adding();
    std::vector<std::string> problems;
    if (!homes::give(sequence, target, problems))
        return;

    // The registers this processor keeps values in, which is every one the
    // calling convention has an opinion about. Not the ones a call passes
    // arguments in - that is four, and a recovered function has more alive at
    // once than that.
    std::vector<std::string> named;
    std::string trouble;
    if (!target.value_registers(4, named, trouble))
        return;

    std::set<uint64_t> ordinary;
    for (const std::string &one : named) {
        const ir::Target::RegisterPlace *place = target.register_place(one);
        if (place != nullptr)
            ordinary.insert(place->offset);
    }
    report(ordinary.size() > 8,
           "a processor keeps values in more registers than a call passes arguments in",
           std::to_string(ordinary.size()) + " of them");

    bool all_ordinary = true;
    for (const pcode::Block &block : sequence.blocks) {
        for (const pcode::Operation &operation : block.operations) {
            if (operation.writes)
                all_ordinary = all_ordinary && ordinary.count(operation.output.offset) != 0;
            for (const pcode::Varnode &input : operation.inputs)
                all_ordinary = all_ordinary && ordinary.count(input.offset) != 0;
        }
    }
    report(all_ordinary,
           "and they are registers for holding values rather than ones the processor keeps for "
           "itself",
           "something landed in a register that is not for holding values");
}

// A value somebody already placed keeps its place. That is what a pin is for,
// and moving one would undo the reason it was carried this far.
void check_a_pinned_value_is_left_alone()
{
    ir::Target target;
    if (!target_for("MIPS:BE:32:default", target))
        return;

    const ir::Target::RegisterPlace *pinned = target.register_place("a0");
    if (pinned == nullptr)
        return;

    pcode::Sequence sequence = adding();
    // The answer goes where it was told to go.
    sequence.blocks[0].operations[0].output.where = pcode::Where::Register;
    sequence.blocks[0].operations[0].output.offset = pinned->offset;
    sequence.blocks[0].operations[0].output.size = pinned->width;
    sequence.blocks[0].operations[1].inputs[0] = sequence.blocks[0].operations[0].output;

    std::vector<std::string> problems;
    if (!homes::give(sequence, target, problems)) {
        report(false, "a sequence with something already placed is housed",
               problems.empty() ? "" : problems.front());
        return;
    }

    report(sequence.blocks[0].operations[0].output.offset == pinned->offset,
           "a value that was already placed stays where it was put", "it was moved");

    // And nothing else is given that place, since the whole point of putting a
    // value somewhere is that nothing else goes there.
    bool nothing_else = true;
    for (const pcode::Varnode &input : sequence.blocks[0].operations[0].inputs)
        nothing_else = nothing_else && input.offset != pinned->offset;
    report(nothing_else, "and nothing else is put on top of it", "something else went there");
}

// Nothing to house is not a failure.
void check_nothing_to_do()
{
    ir::Target target;
    if (!target_for("MIPS:BE:32:default", target))
        return;

    pcode::Sequence empty;
    empty.name = "nothing";
    empty.entry = 1;
    pcode::Block block;
    block.identifier = 1;
    pcode::Operation leaving;
    leaving.opcode = ghidra::CPUI_RETURN;
    block.operations.push_back(leaving);
    empty.blocks.push_back(block);

    std::vector<std::string> problems;
    report(homes::give(empty, target, problems) && problems.empty(),
           "a sequence with nothing to house is housed without complaint",
           problems.empty() ? "" : problems.front());
}

// A value goes in a register it fits in, which is not the same as one its own
// size.
//
// A processor's registers come in the sizes it has, and those are not the sizes
// a program uses. RISC-V on sixty-four bits has no four-byte register at all, so
// a four-byte value housed only in registers of exactly its width had nowhere to
// go, and every function using one was refused for wanting more registers than
// the processor has - on a processor with thirty-two of them.
void check_a_value_fits_in_a_bigger_register()
{
    ir::Target target;
    if (!target_for("RISCV:LE:64:RV64GC", target))
        return;

    // Four-byte values on a processor whose registers are eight.
    report(target.register_place("a0") != nullptr && target.register_place("a0")->width == 8,
           "this processor's registers are wider than a four-byte value",
           "they were not eight bytes");

    pcode::Sequence sequence = adding();
    std::vector<std::string> problems;
    const bool housed = homes::give(sequence, target, problems);
    report(housed, "and a four-byte value is still given somewhere to live",
           problems.empty() ? "" : problems.front());
    if (!housed)
        return;

    // It keeps its own width, because that is the width the operation works in,
    // and the processor has instructions for four-byte arithmetic that say so.
    bool kept_its_width = true;
    for (const pcode::Block &block : sequence.blocks) {
        for (const pcode::Operation &operation : block.operations) {
            if (operation.writes)
                kept_its_width = kept_its_width && operation.output.size == 4;
            for (const pcode::Varnode &input : operation.inputs)
                kept_its_width = kept_its_width && input.size == 4;
        }
    }
    report(kept_its_width, "and keeps the width the operation works in", "it was widened");
}

} // namespace

int main()
{
    if (initialize(nullptr) != ASTRAL_OK) {
        std::printf("FAIL no compiled specifications were found; set ASTRAL_SPECS\n");
        return 1;
    }

    check_everything_gets_somewhere();
    check_only_ordinary_registers_are_used();
    check_a_pinned_value_is_left_alone();
    check_nothing_to_do();
    check_a_value_fits_in_a_bigger_register();

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
