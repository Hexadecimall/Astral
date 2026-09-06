// The intermediate representation: what it says about a processor, and what it
// refuses to accept about a function.
//
// The first half reads real specifications rather than a table written here,
// because the whole point of describing a target instead of listing one is that
// the answer comes from the same files the decompiler already reads. The
// processors checked are the awkward ones: the machine whose instructions and
// data disagree about byte order, the sixty-four bit machine with four-byte
// pointers, and the ones whose addresses are neither thirty-two nor sixty-four
// bits wide. A representation that gets those right gets the ordinary ones
// right for free.
//
// The second half feeds the checker functions that are wrong in each of the
// ways a function can be wrong, because a check that only ever sees correct
// input is not a check.
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

void expect(bool ok, const std::string &what) { report(ok, what, "it was not so"); }

void expect_equal(long long got, long long wanted, const std::string &what)
{
    char saw[128];
    std::snprintf(saw, sizeof saw, "got %lld, wanted %lld", got, wanted);
    report(got == wanted, what, saw);
}

// Reads a target, reporting the failure rather than asserting, so one missing
// specification does not hide every other answer.
bool target_for(const std::string &language_id, ir::Target &target)
{
    std::string error;
    if (ir::Target::from_language_id(language_id, target, error)) {
        return true;
    }
    report(false, "reads " + language_id, error);
    return false;
}

void check_targets()
{
    ir::Target target;

    // Instructions little-endian, data big-endian. One flag cannot hold this,
    // which is the reason there are two.
    if (target_for("ARM:LEBE:32:v8LEInstruction", target)) {
        expect(!target.instruction_big_endian,
               "ARM LEBE reads its instructions little-endian");
        expect(target.data_big_endian, "ARM LEBE reads its data big-endian");
    }

    // The ordinary case, where both halves agree.
    if (target_for("x86:LE:64:default", target)) {
        expect(!target.instruction_big_endian && !target.data_big_endian,
               "x86 is little-endian throughout");
        expect_equal(target.address_bits, 64, "x86-64 addresses are sixty-four bits");
        expect_equal(target.pointer_bytes, 8, "x86-64 pointers are eight bytes");
    }

    if (target_for("MIPS:BE:64:default", target)) {
        expect(target.instruction_big_endian && target.data_big_endian,
               "big-endian MIPS is big-endian throughout");
    }

    // The other way a processor disagrees with itself. Its id says BE with no
    // hint of the split, and the specification says instructionEndian="little";
    // reading only the id calls this big-endian throughout, which is wrong
    // about every instruction on it.
    if (target_for("AARCH64:BE:64:v8A", target)) {
        expect(!target.instruction_big_endian,
               "big-endian AARCH64 still reads little-endian instructions");
        expect(target.data_big_endian, "big-endian AARCH64 reads big-endian data");
    }

    // Sixteen bits, which nothing that assumes a machine word is eight bytes
    // will get right.
    if (target_for("z80:LE:16:default", target)) {
        expect_equal(target.address_bits, 16, "a z80 addresses sixteen bits");
        expect_equal(target.pointer_bytes, 2, "a z80 pointer is two bytes");
    }

    // Twenty-four, which is not a whole number of anything convenient and is
    // why the byte count rounds up rather than dividing.
    if (target_for("PIC-24F:LE:24:default", target)) {
        expect_equal(target.address_bits, 24, "a PIC-24 addresses twenty-four bits");
        expect_equal(target.pointer_bytes, 3, "a PIC-24 pointer is three bytes");
    }

    // A processor whose pointers are narrower than its registers. The
    // specification says so with a truncation, whose size is a count of bytes
    // and not a width in bits; reading it as bits gave every truncated language
    // one-byte pointers, which is not a pointer anywhere.
    if (target_for("AARCH64:LE:32:ilp32", target))
        expect_equal(target.pointer_bytes, 4, "an ilp32 pointer is four bytes");
    if (target_for("PowerPC:BE:64:64-32addr", target))
        expect_equal(target.pointer_bytes, 4,
                     "a PowerPC addressing thirty-two bits has four-byte pointers");
    if (target_for("MIPS:LE:64:64-32addr", target))
        expect_equal(target.pointer_bytes, 4,
                     "a MIPS addressing thirty-two bits has four-byte pointers");

    // What a description cannot answer, it does not pretend to. Whether an
    // address counts one byte or two, and whether code and data are separate
    // memories, are properties of the address spaces in the compiled
    // specification; nineteen of these languages are word-addressed and
    // twenty-three are Harvard, so a confident default would be wrong about
    // forty-two of them.
    if (target_for("avr8:LE:16:default", target)) {
        expect(!target.spaces_read,
               "a target read from a description says its spaces were not read");
        expect_equal(target.address_unit_bytes, 0,
                     "an unread address unit is nothing, not a claim that it is one");
    }

    // A language id with a compiler on the end names the same language.
    {
        ir::Target with_compiler;
        std::string error;
        const bool read = ir::Target::from_language_id("x86:LE:64:default:gcc", with_compiler,
                                                       error);
        report(read && with_compiler.language_id == "x86:LE:64:default",
               "an id with a compiler on the end still names its language",
               read ? with_compiler.language_id : error);
    }

    // Something that is not a processor is refused, and says so.
    {
        ir::Target nothing;
        std::string error;
        const bool read = ir::Target::from_language_id("Nonesuch:LE:32:default", nothing, error);
        report(!read && !error.empty(), "a processor with no specification is refused",
               read ? "it was accepted" : "it gave no reason");
    }
}

// A function that does nothing but return, which is the smallest correct one.
ir::Function returning_function()
{
    ir::Function function;
    function.name = "returns";
    function.entry = 1;

    ir::Block block;
    block.identifier = 1;

    ir::Instruction constant;
    constant.operation = ir::Operation::Constant;
    constant.result.identifier = 10;
    constant.immediate = 7;
    constant.width = 4;
    block.instructions.push_back(constant);

    ir::Instruction leave;
    leave.operation = ir::Operation::Return;
    leave.arguments.push_back(constant.result);
    block.instructions.push_back(leave);

    function.blocks.push_back(block);
    return function;
}

void check_verification()
{
    {
        ir::Function function = returning_function();
        std::vector<std::string> problems;
        report(ir::verify(function, problems), "a function that only returns is accepted",
               problems.empty() ? "" : problems.front());
    }

    // A value given twice makes every question about where it lives
    // unanswerable, so it is the first thing checked.
    {
        ir::Function function = returning_function();
        ir::Instruction again;
        again.operation = ir::Operation::Constant;
        again.result.identifier = 10;  // already given
        again.width = 4;
        function.blocks[0].instructions.insert(function.blocks[0].instructions.begin(), again);
        std::vector<std::string> problems;
        expect(!ir::verify(function, problems), "a value given twice is refused");
    }

    // Reading something nothing gives is what becomes machine code for a
    // register nobody wrote to.
    {
        ir::Function function = returning_function();
        function.blocks[0].instructions.back().arguments[0].identifier = 999;
        std::vector<std::string> problems;
        expect(!ir::verify(function, problems), "reading a value nothing gives is refused");
    }

    // A block has to have a way out.
    {
        ir::Function function = returning_function();
        function.blocks[0].instructions.pop_back();
        std::vector<std::string> problems;
        expect(!ir::verify(function, problems), "a block with no way out is refused");
    }

    // And only one, at the end.
    {
        ir::Function function = returning_function();
        ir::Instruction extra;
        extra.operation = ir::Operation::Return;
        function.blocks[0].instructions.insert(function.blocks[0].instructions.begin(), extra);
        std::vector<std::string> problems;
        expect(!ir::verify(function, problems), "leaving before the end of a block is refused");
    }

    // Going somewhere that does not exist.
    {
        ir::Function function = returning_function();
        function.blocks[0].instructions.back().operation = ir::Operation::Jump;
        function.blocks[0].instructions.back().successors.push_back(77);
        std::vector<std::string> problems;
        expect(!ir::verify(function, problems), "going to a block that does not exist is refused");
    }

    // The entry has to be a block that is there.
    {
        ir::Function function = returning_function();
        function.entry = 42;
        std::vector<std::string> problems;
        expect(!ir::verify(function, problems), "an entry that is not a block is refused");
    }
}

// A width of zero asks for the target's own word, which is the only way an
// instruction can be written once and mean the right thing on a z80 and on
// an x86-64.
void check_widths()
{
    ir::Instruction instruction;
    instruction.width = 0;

    ir::Target wide;
    wide.word_bytes = 8;
    expect_equal(ir::width_of(instruction, wide), 8, "an unsaid width is the target's word");

    ir::Target narrow;
    narrow.word_bytes = 2;
    expect_equal(ir::width_of(instruction, narrow), 2,
                 "the same instruction is two bytes wide on a two-byte machine");

    instruction.width = 4;
    expect_equal(ir::width_of(instruction, narrow), 4, "a width that was said is kept");
}

// Storage is the whole reason this representation exists, so what it looks like
// written down is checked rather than assumed: a value pinned to a register has
// to still say so after lowering, or the pin was lost on the way.
void check_writing()
{
    ir::Function function = returning_function();
    function.name = "pinned";
    function.address = 0x100000500;
    function.budget = 52;

    ir::Value parameter;
    parameter.identifier = 1;
    parameter.storage.kind = Storage::Kind::Register;
    parameter.storage.register_name = "x0";
    function.parameters.push_back(parameter);

    const std::string text = ir::to_text(function);

    report(text.find("@x0") != std::string::npos,
           "a value pinned to a register still says so", text);
    report(text.find("within 52 bytes") != std::string::npos,
           "a size budget is part of what the function is", text);
    report(text.find("at 0x100000500") != std::string::npos,
           "a fixed address is part of what the function is", text);
    report(text.find("(entry)") != std::string::npos, "the entry block is marked", text);

    // Every kind of storage the language can write has to come back out.
    {
        ir::Function frame_pinned = returning_function();
        ir::Value local;
        local.identifier = 2;
        local.storage.kind = Storage::Kind::Frame;
        local.storage.offset = -0x70;
        frame_pinned.parameters.push_back(local);
        const std::string written = ir::to_text(frame_pinned);
        report(written.find("@-0x70") != std::string::npos,
               "a frame offset is written the way it was read", written);
    }
    {
        ir::Function on_entry = returning_function();
        ir::Value held;
        held.identifier = 3;
        held.storage.kind = Storage::Kind::EntryRegister;
        held.storage.register_name = "x29";
        on_entry.parameters.push_back(held);
        const std::string written = ir::to_text(on_entry);
        report(written.find("@x29@entry") != std::string::npos,
               "what a register held on entry is not confused with the register", written);
    }

    // A function that is only bytes is still a function, which is what makes
    // every level compile.
    {
        ir::Function machine;
        machine.name = "already_instructions";
        machine.level = Level::Machine;
        machine.entry = 1;
        ir::Block block;
        block.identifier = 1;
        ir::Instruction raw;
        raw.operation = ir::Operation::Raw;
        raw.bytes = {0x00, 0x01, 0x02, 0x03};
        block.instructions.push_back(raw);
        ir::Instruction leave;
        leave.operation = ir::Operation::Return;
        block.instructions.push_back(leave);
        machine.blocks.push_back(block);

        std::vector<std::string> problems;
        report(ir::verify(machine, problems), "a function that is only bytes is accepted",
               problems.empty() ? "" : problems.front());
        const std::string written = ir::to_text(machine);
        report(written.find("level machine") != std::string::npos &&
                   written.find("4 bytes") != std::string::npos,
               "bytes that were already instructions are kept as they were", written);
    }
}

// The builder keeps two rules so that whatever lowers into this does not have
// to: a value is numbered once, and a block is left exactly once. Both are
// checked here by breaking them.
void check_building()
{
    // A conditional, built the way a lowering would build one.
    {
        ir::Builder builder("chooses");
        Storage in_first;
        in_first.kind = Storage::Kind::Register;
        in_first.register_name = "x0";
        const ir::Value given = builder.parameter(nullptr, in_first);

        const uint32_t start = builder.block();
        const ir::Value zero = builder.constant(0, 4);
        const ir::Value same = builder.binary(ir::Operation::Equal, given, zero, 4, false);

        const uint32_t when_zero = builder.block();
        const uint32_t otherwise = builder.block();

        builder.resume(start);
        builder.branch(same, when_zero, otherwise);

        builder.resume(when_zero);
        builder.ret(builder.constant(1, 4));

        builder.resume(otherwise);
        builder.ret(builder.constant(2, 4));

        ir::Function built;
        std::vector<std::string> problems;
        report(builder.finish(built, problems), "a branch built the ordinary way comes out valid",
               problems.empty() ? "" : problems.front());
        expect_equal(static_cast<long long>(built.blocks.size()), 3,
                     "the three blocks that were opened are all there");
        expect(built.entry == start, "the first block opened is the entry");
        report(ir::to_text(built).find("@x0") != std::string::npos,
               "the parameter kept the register it was pinned to", ir::to_text(built));
    }

    // Every value the builder hands out is a different one, which is what makes
    // single assignment true by construction rather than by care.
    {
        ir::Builder builder("counts");
        builder.block();
        const ir::Value first = builder.constant(1, 4);
        const ir::Value second = builder.constant(1, 4);
        expect(first.identifier != second.identifier,
               "two values are never given the same number");
        builder.ret();
    }

    // Writing into a block that has already been left is the mistake worth
    // catching where it happens.
    {
        ir::Builder builder("writes past the end");
        builder.block();
        builder.ret();
        builder.constant(9, 4);
        expect(builder.failed(), "writing into a block already left is refused");
    }

    // And a block that was never opened cannot be written into either.
    {
        ir::Builder builder("resumes nothing");
        builder.resume(17);
        expect(builder.failed(), "resuming a block that was never opened is refused");
    }

    // A builder that was misused says so instead of handing back a function.
    {
        ir::Builder builder("never finished");
        builder.block();  // opened and never left
        ir::Function built;
        std::vector<std::string> problems;
        expect(!builder.finish(built, problems),
               "a function whose block has no way out is not handed back");
    }
}

} // namespace

int main()
{
    // The library's own start-up, rather than Ghidra's: the specifications ship
    // in a flat tree here, so the directories holding them are collected before
    // the decompiler is handed them, and that is what `initialize` does.
    if (initialize(nullptr) != ASTRAL_OK) {
        std::printf("FAIL no compiled specifications were found; set ASTRAL_SPECS\n");
        return 1;
    }

    check_targets();
    check_verification();
    check_widths();
    check_writing();
    check_building();

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
