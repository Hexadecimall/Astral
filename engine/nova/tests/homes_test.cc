// Giving values somewhere to live.
//
// A value with no home is ordinary in the representation and impossible in an
// instruction, so something has to bridge the two. What is checked here is that
// it does, that it does not put values in registers that mean something else,
// and that it leaves alone the ones somebody already placed.
#include "catalogue.hh"
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

// Nothing is kept in the register holding the way back.
//
// A processor that says where a return address lives is easy. AARCH64 says it
// nowhere - no compiler specification it has names one - so that register was
// in every pool the instruction set offers, and seventeen of twenty recovered
// functions worked their answer out in x30 and then returned to it. Every one
// of those instructions decoded perfectly and named the right registers, which
// is exactly why only asking what the whole thing does can find it.
//
// The instruction set is asked instead: a return reads the register the address
// is in and takes no operand saying so, so the register its return forms name
// outright is the one.
void check_the_way_back_is_kept()
{
    for (const std::string &language :
         {std::string("AARCH64:LE:64:v8A"), std::string("MIPS:BE:32:default"),
          std::string("RISCV:LE:64:RV64GC")}) {
        ir::Target target;
        if (!target_for(language, target))
            continue;
        catalogue::Catalogue catalogue;
        std::string trouble;
        if (!catalogue.read(target, trouble)) {
            report(false, language + "'s instructions are read", trouble);
            continue;
        }

        uint64_t back_through = 0;
        if (!catalogue.return_through(target, back_through)) {
            report(false, language + " says where it keeps the way back",
                   "nothing said where a return address lives");
            continue;
        }
        report(true, language + " says where it keeps the way back",
               "at " + std::to_string(back_through));

        pcode::Sequence sequence = adding();
        std::vector<std::string> problems;
        if (!homes::give(sequence, target, problems, &catalogue)) {
            report(false, "  and still houses everything",
                   problems.empty() ? "" : problems.front());
            continue;
        }

        bool kept = true;
        for (const pcode::Block &block : sequence.blocks) {
            for (const pcode::Operation &operation : block.operations) {
                if (operation.writes && operation.output.where == pcode::Where::Register)
                    kept = kept && operation.output.offset != back_through;
                for (const pcode::Varnode &input : operation.inputs)
                    if (input.where == pcode::Where::Register)
                        kept = kept && input.offset != back_through;
            }
        }
        report(kept, "  and puts nothing in it",
               "a value was housed in the register the function returns through");
    }
}

// A value too wide for any register is refused, not quietly made narrower.
//
// A thirty-two bit processor has nowhere to put a sixty-four bit value, and
// recovered code is full of them - a length, an index, anything the source
// declared as eight bytes. Housing one in a four-byte register and shortening
// it to fit loses the top half of every such value, and the instructions that
// come out decode perfectly while computing on half a number.
//
// That is what happened: a register let go while one width was being placed
// went back on the list for that width whatever its own width was, so a
// four-byte register was handed to an eight-byte value, and the placing then
// trimmed the value to the register. Saying plainly that there is nowhere to
// put it is the honest answer, and the only one that is not wrong.
void check_a_value_too_wide_is_refused()
{
    ir::Target target;
    if (!target_for("MIPS:BE:32:default", target))
        return;
    catalogue::Catalogue catalogue;
    std::string trouble;
    if (!catalogue.read(target, trouble)) {
        report(false, "its instructions are read", trouble);
        return;
    }

    // This processor keeps values in registers of four bytes and has none of
    // eight, which is what makes it the case worth checking.
    std::vector<std::string> wide;
    const bool any_wide = target.value_registers(8, wide, trouble) && !wide.empty();
    report(!any_wide, "this processor has no register wide enough for eight bytes",
           any_wide ? wide.front() + " is one" : "");

    pcode::Operation adding;
    adding.opcode = ghidra::CPUI_INT_ADD;
    adding.writes = true;
    adding.output = homeless(16, 8);
    adding.inputs.push_back(homeless(0, 8));
    adding.inputs.push_back(homeless(8, 8));

    pcode::Block block;
    block.identifier = 1;
    block.operations.push_back(adding);
    pcode::Sequence sequence;
    sequence.name = "wide";
    sequence.entry = 1;
    sequence.blocks.push_back(block);

    std::vector<std::string> problems;
    const bool housed = homes::give(sequence, target, problems, &catalogue);
    report(!housed, "an eight-byte value is refused rather than housed",
           housed ? "it was given somewhere" : "");
    report(!problems.empty() && problems.front().find("wide enough") != std::string::npos,
           "and the reason says nothing is wide enough for it",
           problems.empty() ? "" : problems.front());

    // And nothing was narrowed on the way out.
    bool kept_its_width = true;
    for (const pcode::Block &one : sequence.blocks) {
        for (const pcode::Operation &operation : one.operations) {
            if (operation.writes)
                kept_its_width = kept_its_width && operation.output.size == 8;
            for (const pcode::Varnode &input : operation.inputs)
                kept_its_width = kept_its_width && input.size == 8;
        }
    }
    report(kept_its_width, "and the value is still eight bytes wide", "it was trimmed to fit");
}

// A narrow value on a processor that writes its biggest byte first still lands
// somewhere the processor has a name for.
//
// A value narrower than the register holding it sits at the far end of that
// register on a big-endian machine, and that is where it was put. But selection
// writes a register name into an instruction and finds the name by the exact
// address the value sits at, and MIPS names its registers only at four-byte
// boundaries - nothing inside one. So a one-byte value moved three bytes along
// had no name, and every instruction over it was refused: loads of single
// characters, the one-byte answers of comparisons, and the branches reading
// them, around two hundred and fifty refusals in all.
void check_a_narrow_value_lands_somewhere_named()
{
    ir::Target target;
    if (!target_for("MIPS:BE:32:default", target))
        return;
    catalogue::Catalogue catalogue;
    std::string trouble;
    if (!catalogue.read(target, trouble)) {
        report(false, "reads what MIPS can do", trouble);
        return;
    }

    // A one-byte answer, of the shape a comparison makes and a branch reads.
    pcode::Operation compare;
    compare.opcode = ghidra::CPUI_INT_EQUAL;
    compare.writes = true;
    compare.output = homeless(0, 1);
    compare.inputs.push_back(homeless(8, 4));
    compare.inputs.push_back(homeless(12, 4));

    pcode::Operation branch;
    branch.opcode = ghidra::CPUI_CBRANCH;
    branch.inputs.push_back(homeless(0, 1));

    pcode::Block block;
    block.identifier = 1;
    block.operations.push_back(compare);
    block.operations.push_back(branch);

    pcode::Sequence sequence;
    sequence.name = "compares";
    sequence.entry = 1;
    sequence.blocks.push_back(block);

    std::vector<std::string> problems;
    if (!homes::give(sequence, target, problems, &catalogue)) {
        report(false, "a one-byte value is housed on a big-endian processor",
               problems.empty() ? "" : problems.front());
        return;
    }

    bool every_one_named = true;
    std::string first_nameless;
    for (const pcode::Block &one : sequence.blocks) {
        for (const pcode::Operation &operation : one.operations) {
            auto look = [&](const pcode::Varnode &node) {
                if (node.where != pcode::Where::Register)
                    return;
                for (const auto &named : target.register_places) {
                    if (named.second.offset == node.offset)
                        return;
                }
                every_one_named = false;
                if (first_nameless.empty())
                    first_nameless = "nothing is named at " + std::to_string(node.offset);
            };
            if (operation.writes)
                look(operation.output);
            for (const pcode::Varnode &input : operation.inputs)
                look(input);
        }
    }
    report(every_one_named,
           "a one-byte value on a big-endian processor lands where a register is named",
           first_nameless);

    // And it is still one byte: the operation works in the width it was written
    // in, and widening it here would be a different operation.
    bool kept_its_width = true;
    for (const pcode::Block &one : sequence.blocks) {
        for (const pcode::Operation &operation : one.operations) {
            if (operation.writes && operation.opcode == ghidra::CPUI_INT_EQUAL)
                kept_its_width = operation.output.size == 1;
        }
    }
    report(kept_its_width, "and it is still one byte wide", "its width was changed to fit");
}

} // namespace

int main()
{
    if (initialize(nullptr) != ASTRAL_OK) {
        std::printf("FAIL no compiled specifications were found; set ASTRAL_SPECS\n");
        return 1;
    }
    check_a_value_too_wide_is_refused();

    check_everything_gets_somewhere();
    check_only_ordinary_registers_are_used();
    check_a_pinned_value_is_left_alone();
    check_nothing_to_do();
    check_a_value_fits_in_a_bigger_register();
    check_the_way_back_is_kept();
    check_a_narrow_value_lands_somewhere_named();

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
