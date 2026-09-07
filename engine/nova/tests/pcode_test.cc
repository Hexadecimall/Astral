// Writing the representation as p-code.
//
// p-code is the language every processor here is already described in, so this
// is the step that decides whether one back end can reach all of them or only
// the ones somebody wrote an encoder for. What is worth checking is that the
// operations which differ by signedness pick different p-code - the machine has
// two instructions for those and choosing wrongly is wrong quietly - and that a
// value pinned to a register lands in the register file rather than in a place
// invented for it.
#include "ir.hh"
#include "lexer.hh"
#include "lower_ir.hh"
#include "parser.hh"
#include "pcode.hh"

#include "session.hh"

#include <cstdio>
#include <cstring>
#include <map>
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

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Builds a function by hand, so the operation being checked is exactly the one
// meant rather than whatever the front end happened to produce.
std::string written(ir::Operation operation, int width, bool is_signed,
                    const std::string &language_id, std::string &why)
{
    ir::Target target;
    if (!ir::Target::from_language_id(language_id, target, why))
        return std::string();
    if (!target.read_specification(why))
        return std::string();

    ir::Builder builder("checked");
    builder.block();
    const ir::Value left = builder.constant(3, width);
    const ir::Value right = builder.constant(4, width);
    const ir::Value answer = builder.binary(operation, left, right, width, is_signed);
    builder.ret(answer);

    ir::Function function;
    std::vector<std::string> problems;
    if (!builder.finish(function, problems)) {
        why = problems.empty() ? "the function would not build" : problems.front();
        return std::string();
    }

    pcode::Sequence sequence;
    if (!pcode::to_pcode(function, target, sequence, problems)) {
        why = problems.empty() ? "it would not be written" : problems.front();
        return std::string();
    }
    return pcode::to_text(sequence);
}

// A comparison answers in one byte, however wide the things compared.
//
// The width of an operation is the width of what it works on, and for a
// comparison that is not the width of its answer - comparing two eight-byte
// values asks an eight-byte question and gets a yes or a no. Giving the answer
// the operands' width asks for somewhere eight bytes wide to keep a yes in,
// which a thirty-two bit processor has not got, so every comparison over a long
// value was refused for want of a register to hold one bit.
//
// The value each side reads has to agree, too: a value is placed once and
// remembered, so narrowing only what was emitted leaves the branch that reads
// the answer still asking for the wider one, which is a different value.
void check_a_truth_is_one_byte()
{
    ir::Target target;
    std::string why;
    if (!ir::Target::from_language_id("MIPS:BE:32:default", target, why) ||
        !target.read_specification(why)) {
        report(false, "the processor is read", why);
        return;
    }

    ir::Builder builder("asks");
    builder.block();
    const ir::Value left = builder.constant(3, 8);
    const ir::Value right = builder.constant(4, 8);
    const ir::Value answer = builder.binary(ir::Operation::Equal, left, right, 8, false);
    builder.ret(answer);

    ir::Function function;
    std::vector<std::string> problems;
    pcode::Sequence sequence;
    if (!builder.finish(function, problems) ||
        !pcode::to_pcode(function, target, sequence, problems)) {
        report(false, "a comparison of two eight-byte values is written",
               problems.empty() ? "" : problems.front());
        return;
    }

    // The answer is one byte, and everything that reads it says one byte too.
    bool answer_is_a_byte = false;
    bool read_as_a_byte = true;
    uint64_t truth = 0;
    for (const pcode::Block &block : sequence.blocks) {
        for (const pcode::Operation &operation : block.operations) {
            if (operation.writes && operation.opcode == ghidra::CPUI_INT_EQUAL) {
                answer_is_a_byte = operation.output.size == 1;
                truth = operation.output.offset;
            }
        }
    }
    report(answer_is_a_byte, "a comparison of eight-byte values answers in one byte",
           "it answered in more");

    for (const pcode::Block &block : sequence.blocks) {
        for (const pcode::Operation &operation : block.operations) {
            if (operation.opcode == ghidra::CPUI_INT_EQUAL)
                continue;
            for (const pcode::Varnode &input : operation.inputs) {
                if (input.where == pcode::Where::Unique && input.offset == truth)
                    read_as_a_byte = read_as_a_byte && input.size == 1;
            }
        }
    }
    report(read_as_a_byte, "and whatever reads that answer reads one byte",
           "something read it wider than it was written");
}

// p-code has no signed values, only signed operations, so the same comparison
// written twice has to come out as two different opcodes. Getting this wrong is
// wrong quietly: the bits are the same and only the answer differs.
void check_signedness_picks_the_opcode()
{
    struct Case {
        ir::Operation operation;
        const char *when_signed;
        const char *when_not;
        const char *what;
    };
    const Case cases[] = {
        {ir::Operation::Less, "INT_SLESS", "INT_LESS", "a less than"},
        {ir::Operation::LessOrEqual, "INT_SLESSEQUAL", "INT_LESSEQUAL", "a less or equal"},
        {ir::Operation::Divide, "INT_SDIV", "INT_DIV", "a division"},
        {ir::Operation::Remainder, "INT_SREM", "INT_REM", "a remainder"},
        {ir::Operation::ShiftRight, "INT_SRIGHT", "INT_RIGHT", "a shift right"},
    };

    for (const Case &one : cases) {
        std::string why;
        const std::string when_signed =
            written(one.operation, 4, true, "AARCH64:LE:64:AppleSilicon", why);
        const std::string when_not =
            written(one.operation, 4, false, "AARCH64:LE:64:AppleSilicon", why);
        if (when_signed.empty() || when_not.empty()) {
            report(false, std::string(one.what) + " is written", why);
            continue;
        }
        report(contains(when_signed, one.when_signed),
               std::string(one.what) + " over signed values is " + one.when_signed, when_signed);
        report(contains(when_not, one.when_not),
               std::string(one.what) + " over unsigned values is " + one.when_not, when_not);
    }
}

// Adding does not care about signedness, because the bits come out the same
// either way. There is one opcode for it and both spellings have to reach it.
void check_addition_does_not_care()
{
    std::string why;
    const std::string when_signed =
        written(ir::Operation::Add, 4, true, "AARCH64:LE:64:AppleSilicon", why);
    const std::string when_not =
        written(ir::Operation::Add, 4, false, "AARCH64:LE:64:AppleSilicon", why);
    report(!when_signed.empty() && contains(when_signed, "INT_ADD") &&
               !when_not.empty() && contains(when_not, "INT_ADD"),
           "an addition is the same operation whether or not it is signed",
           when_signed.empty() ? why : when_signed);
}

// The two whose p-code names read backwards from what they are.
void check_the_two_that_read_backwards()
{
    ir::Target target;
    std::string why;
    if (!ir::Target::from_language_id("AARCH64:LE:64:AppleSilicon", target, why) ||
        !target.read_specification(why)) {
        report(false, "the target is read", why);
        return;
    }

    for (const auto &one : {std::make_pair(ir::Operation::Negate, "INT_2COMP"),
                            std::make_pair(ir::Operation::BitNot, "INT_NEGATE")}) {
        ir::Builder builder("checked");
        builder.block();
        const ir::Value only = builder.constant(1, 4);
        ir::Instruction instruction;
        instruction.operation = one.first;
        instruction.width = 4;
        instruction.arguments.push_back(only);
        instruction.result = builder.value();
        const ir::Value answer = builder.emit(std::move(instruction));
        builder.ret(answer);

        ir::Function function;
        std::vector<std::string> problems;
        if (!builder.finish(function, problems)) {
            report(false, "it builds", problems.empty() ? "" : problems.front());
            continue;
        }
        pcode::Sequence sequence;
        if (!pcode::to_pcode(function, target, sequence, problems)) {
            report(false, "it is written", problems.empty() ? "" : problems.front());
            continue;
        }
        const std::string text = pcode::to_text(sequence);
        report(contains(text, one.second),
               std::string("it comes out as ") + one.second +
                   ", whatever the name looks like it means",
               text);
    }
}

// A pinned value has to land in the register file, at the offset the
// specification gives for that register. Anything else means the pin was
// carried all this way and then dropped at the last step.
void check_a_pinned_value_is_its_register()
{
    std::vector<compiler::Diagnostic> diagnostics;
    const std::string source = "func doubled(@w0): i32 {\n    return w0 + w0;\n}\n";
    const std::vector<Token> tokens = tokenise(source, diagnostics);

    Types types;
    Unit unit;
    if (!parse(tokens, types, unit, diagnostics)) {
        report(false, "the source parses", "it did not");
        return;
    }

    ir::Target target;
    std::string why;
    if (!ir::Target::from_language_id("AARCH64:LE:64:AppleSilicon", target, why)) {
        report(false, "the target is read", why);
        return;
    }

    ir::Unit lowered;
    if (!lower_to_ir(unit, types, target, lowered, diagnostics) || lowered.functions.empty()) {
        report(false, "it lowers", "it did not");
        return;
    }

    pcode::Sequence sequence;
    std::vector<std::string> problems;
    if (!pcode::to_pcode(lowered.functions.front(), lowered.target, sequence, problems)) {
        report(false, "it is written as p-code", problems.empty() ? "" : problems.front());
        return;
    }
    const std::string text = pcode::to_text(sequence);

    report(contains(text, "(register,"), "a pinned value lands in the register file", text);
    report(contains(text, "INT_ADD"), "the addition is an addition", text);
    // w0 is four bytes on this processor, and the varnode has to say so: a
    // register named in p-code carries its width, and the wrong width there is
    // the wrong instruction later.
    report(contains(text, ", 4)"), "the register's own width came with it", text);
}

// A body that is already instructions has no p-code spelling here, and is
// refused rather than written as something it is not.
void check_raw_is_refused()
{
    ir::Target target;
    std::string why;
    if (!ir::Target::from_language_id("AARCH64:LE:64:AppleSilicon", target, why))
        return;

    ir::Builder builder("already instructions");
    builder.block();
    ir::Instruction raw;
    raw.operation = ir::Operation::Raw;
    raw.bytes = {0x00, 0x01};
    builder.emit(std::move(raw));
    builder.ret();

    ir::Function function;
    std::vector<std::string> problems;
    if (!builder.finish(function, problems)) {
        report(false, "it builds", problems.empty() ? "" : problems.front());
        return;
    }

    pcode::Sequence sequence;
    std::vector<std::string> written_problems;
    const bool wrote = pcode::to_pcode(function, target, sequence, written_problems);
    report(!wrote && !written_problems.empty(),
           "a body that is already instructions is refused rather than written as something else",
           wrote ? "it was written" : "it gave no reason");
}


// Running what was written, which is the only way to know a translation kept
// the meaning. Everything before this compares text; this compares answers.
namespace {

// Nova in, answer out: parsed, lowered, written as p-code and run.
bool answer_for(const std::string &source, const std::string &language_id,
                const std::map<std::string, uint64_t> &given, uint64_t &value, std::string &why)
{
    std::vector<compiler::Diagnostic> diagnostics;
    const std::vector<Token> tokens = tokenise(source, diagnostics);
    Types types;
    Unit unit;
    if (!parse(tokens, types, unit, diagnostics)) {
        why = "it did not parse";
        return false;
    }

    ir::Target target;
    if (!ir::Target::from_language_id(language_id, target, why))
        return false;
    if (!target.read_specification(why))
        return false;

    ir::Unit lowered;
    if (!lower_to_ir(unit, types, target, lowered, diagnostics) || lowered.functions.empty()) {
        why = "it did not lower";
        return false;
    }

    pcode::Sequence sequence;
    std::vector<std::string> problems;
    if (!pcode::to_pcode(lowered.functions.front(), lowered.target, sequence, problems)) {
        why = problems.empty() ? "it was not written" : problems.front();
        return false;
    }

    // Registers are set by the offset the specification gives them, which is
    // what p-code means by a register.
    pcode::Machine machine;
    for (const auto &one : given) {
        const ir::Target::RegisterPlace *place = lowered.target.register_place(one.first);
        if (place == nullptr) {
            why = "this processor has no register called " + one.first;
            return false;
        }
        machine.registers[place->offset] = one.second;
    }

    const pcode::Answer answer = pcode::run(sequence, machine);
    if (!answer.ok) {
        why = answer.error;
        return false;
    }
    value = answer.value;
    return true;
}

} // namespace

void check_it_computes_what_the_source_said()
{
    const char *arm = "AARCH64:LE:64:AppleSilicon";

    // A pinned parameter, doubled. Twenty-one in, forty-two out, and the
    // twenty-one is put in the register the source named.
    {
        uint64_t value = 0;
        std::string why;
        const bool ran = answer_for("func doubled(@w0): i32 {\n    return w0 + w0;\n}\n", arm,
                                    {{"w0", 21}}, value, why);
        report(ran && value == 42, "doubling a pinned register gives back twice it",
               ran ? "got " + std::to_string(value) : why);
    }

    // A local, which goes through a frame slot: written, read back, added to.
    {
        uint64_t value = 0;
        std::string why;
        const bool ran = answer_for(
            "func total(): i32 {\n"
            "    var running: i32 = 2;\n"
            "    running = running + 40;\n"
            "    return running;\n"
            "}\n",
            arm, {}, value, why);
        report(ran && value == 42, "a local written and read back holds what was put in it",
               ran ? "got " + std::to_string(value) : why);
    }

    // A branch, taken and not taken, from the same source with different input.
    {
        const char *source =
            "func which(@w0): i32 {\n"
            "    if (w0 == 0) {\n"
            "        return 10;\n"
            "    } else {\n"
            "        return 20;\n"
            "    }\n"
            "}\n";
        uint64_t taken = 0;
        uint64_t otherwise = 0;
        std::string why;
        const bool first = answer_for(source, arm, {{"w0", 0}}, taken, why);
        const bool second = answer_for(source, arm, {{"w0", 7}}, otherwise, why);
        report(first && second && taken == 10 && otherwise == 20,
               "a branch goes both ways, and the right way each time",
               first && second ? "got " + std::to_string(taken) + " and " +
                                     std::to_string(otherwise)
                               : why);
    }

    // A loop, which has to go round the right number of times. This is the one
    // that catches a branch wired to the wrong block: everything else still
    // reads correctly when a loop runs once or forever.
    {
        uint64_t value = 0;
        std::string why;
        const bool ran = answer_for(
            "func counts(): i32 {\n"
            "    var seen: i32 = 0;\n"
            "    while (seen < 10) {\n"
            "        seen = seen + 1;\n"
            "    }\n"
            "    return seen;\n"
            "}\n",
            arm, {}, value, why);
        report(ran && value == 10, "a loop goes round until its test says to stop",
               ran ? "got " + std::to_string(value) : why);
    }

    // The same source on a processor with different registers and a different
    // word. Nothing about any of this was written per-processor.
    {
        uint64_t value = 0;
        std::string why;
        const bool ran = answer_for(
            "func total(): i32 {\n"
            "    var running: i32 = 2;\n"
            "    running = running + 40;\n"
            "    return running;\n"
            "}\n",
            "x86:LE:64:default", {}, value, why);
        report(ran && value == 42, "the same source gives the same answer on another processor",
               ran ? "got " + std::to_string(value) : why);
    }

    // Signed and unsigned comparison differ on the same bits, which is the
    // whole reason two opcodes exist. Minus one is above everything unsigned
    // and below everything signed.
    {
        ir::Target target;
        std::string why;
        if (ir::Target::from_language_id(arm, target, why) && target.read_specification(why)) {
            for (bool is_signed : {false, true}) {
                ir::Builder builder("compares");
                builder.block();
                const ir::Value minus_one = builder.constant(0xffffffffu, 4);
                const ir::Value one = builder.constant(1, 4);
                const ir::Value answer =
                    builder.binary(ir::Operation::Less, minus_one, one, 4, is_signed);
                builder.ret(answer);

                ir::Function function;
                std::vector<std::string> problems;
                if (!builder.finish(function, problems))
                    continue;
                pcode::Sequence sequence;
                if (!pcode::to_pcode(function, target, sequence, problems))
                    continue;
                const pcode::Answer ran = pcode::run(sequence, pcode::Machine());
                const uint64_t wanted = is_signed ? 1 : 0;
                report(ran.ok && ran.value == wanted,
                       is_signed ? "minus one is less than one when the comparison is signed"
                                 : "the same bits are not less than one when it is not",
                       ran.ok ? "got " + std::to_string(ran.value) : ran.error);
            }
        }
    }

    // A loop that never finishes stops rather than hanging whatever ran it.
    {
        ir::Target target;
        std::string why;
        if (ir::Target::from_language_id(arm, target, why)) {
            ir::Builder builder("forever");
            const uint32_t round = builder.block();
            builder.jump(round);
            ir::Function function;
            std::vector<std::string> problems;
            if (builder.finish(function, problems)) {
                pcode::Sequence sequence;
                if (pcode::to_pcode(function, target, sequence, problems)) {
                    pcode::Machine machine;
                    machine.budget = 1000;
                    const pcode::Answer ran = pcode::run(sequence, machine);
                    report(!ran.ok && !ran.error.empty(),
                           "something that never finishes is stopped rather than run forever",
                           ran.ok ? "it finished" : ran.error);
                }
            }
        }
    }
}


// The shapes that are not a straight line: choosing between many, and going
// somewhere by name.
void check_choosing_and_going()
{
    const char *arm = "AARCH64:LE:64:AppleSilicon";
    const char *picking =
        "func pick(@w0): i32 {\n"
        "  match (w0) {\n"
        "    1 { return 100; }\n"
        "    2 { return 200; }\n"
        "    else { return 900; }\n"
        "  }\n"
        "}\n";

    struct Case {
        uint64_t given;
        uint64_t wanted;
        const char *what;
    };
    const Case cases[] = {
        {1, 100, "a match takes the arm whose value it is"},
        {2, 200, "and the next arm when it is that one instead"},
        {9, 900, "and the else arm when it is none of them"},
    };
    for (const Case &one : cases) {
        uint64_t value = 0;
        std::string why;
        const bool ran = answer_for(picking, arm, {{"w0", one.given}}, value, why);
        report(ran && value == one.wanted, one.what,
               ran ? "got " + std::to_string(value) : why);
    }

    // A goto reaches a label written after it, which is the case that needs the
    // block to exist before it has been reached. The statement in between can
    // never run, and has to go somewhere anyway.
    {
        uint64_t value = 0;
        std::string why;
        const bool ran = answer_for(
            "func jumps(): i32 {\n"
            "  goto done;\n"
            "  return 1;\n"
            "  label done:\n"
            "  return 7;\n"
            "}\n",
            arm, {}, value, why);
        report(ran && value == 7,
               "a goto reaches a label written after it, past a statement that cannot run",
               ran ? "got " + std::to_string(value) : why);
    }
}


// Counting over a range, where the two spellings differ by exactly one turn and
// `continue` has to still advance the count.
void check_counting()
{
    const char *arm = "AARCH64:LE:64:AppleSilicon";
    const char *summing =
        "func f(): i32 {\n"
        "  var total: i32 = 0;\n"
        "  for (step in RANGE) {\n"
        "    total = total + step;\n"
        "  }\n"
        "  return total;\n"
        "}\n";

    auto with = [&](const std::string &range) {
        std::string source(summing);
        const size_t at = source.find("RANGE");
        source.replace(at, 5, range);
        return source;
    };

    {
        uint64_t value = 0;
        std::string why;
        const bool ran = answer_for(with("0..5"), arm, {}, value, why);
        report(ran && value == 10, "a range stops before its end: nought to four is ten",
               ran ? "got " + std::to_string(value) : why);
    }
    {
        uint64_t value = 0;
        std::string why;
        const bool ran = answer_for(with("0..=5"), arm, {}, value, why);
        report(ran && value == 15, "and includes it when written with ..=: nought to five is fifteen",
               ran ? "got " + std::to_string(value) : why);
    }

    // A continue that skipped the step would be a loop that never ends, so this
    // finishing at all is the thing being checked.
    {
        uint64_t value = 0;
        std::string why;
        const bool ran = answer_for(
            "func f(): i32 {\n"
            "  var total: i32 = 0;\n"
            "  for (step in 0..5) {\n"
            "    continue;\n"
            "  }\n"
            "  return total;\n"
            "}\n",
            arm, {}, value, why);
        report(ran && value == 0, "continuing still advances the count, so the loop finishes",
               ran ? "got " + std::to_string(value) : why);
    }
}


// The expressions that reach into something, or choose between two things, or
// change how wide a value is.
void check_reaching_and_choosing()
{
    const char *arm = "AARCH64:LE:64:AppleSilicon";

    {
        uint64_t value = 0;
        std::string why;
        const bool ran = answer_for("func f(): i32 { return sizeof(i32); }\n", arm, {}, value, why);
        report(ran && value == 4, "how big a thing is, is known before anything runs",
               ran ? "got " + std::to_string(value) : why);
    }

    // A conditional is an if that answers with something, so both halves have
    // to leave their answer in the same place.
    {
        uint64_t taken = 0;
        uint64_t otherwise = 0;
        std::string why;
        const bool first = answer_for("func f(): i32 { var a: i32 = 5; return a > 3 ? 11 : 22; }\n",
                                      arm, {}, taken, why);
        const bool second = answer_for("func f(): i32 { var a: i32 = 1; return a > 3 ? 11 : 22; }\n",
                                       arm, {}, otherwise, why);
        report(first && second && taken == 11 && otherwise == 22,
               "a conditional answers with whichever half its condition chose",
               first && second ? "got " + std::to_string(taken) + " and " +
                                     std::to_string(otherwise)
                               : why);
    }

    // Narrowing keeps the low bytes and nothing else, so three hundred in one
    // byte is forty-four.
    {
        uint64_t value = 0;
        std::string why;
        const bool ran =
            answer_for("func f(): i32 { var a: i32 = 300; return a as u8; }\n", arm, {}, value, why);
        report(ran && value == 44, "narrowing a value keeps the bytes that fit and no others",
               ran ? "got " + std::to_string(value) : why);
    }

    // A member is read and written at the offset its record gives it, so two
    // members of the same thing have to be two different places.
    {
        uint64_t value = 0;
        std::string why;
        const bool ran = answer_for(
            "struct Point { var x: i32; var y: i32; }\n"
            "func f(): i32 {\n"
            "  var where: Point;\n"
            "  where.x = 4;\n"
            "  where.y = 9;\n"
            "  return where.x + where.y;\n"
            "}\n",
            arm, {}, value, why);
        report(ran && value == 13, "two members of one record are two different places",
               ran ? "got " + std::to_string(value) : why);
    }
}


// Calling, which is the one thing a function cannot do alone.
namespace {

// Runs the named function out of a file that may hold several, so that a call
// has something to reach.
bool answer_from(const std::string &source, const std::string &entry,
                 const std::string &language_id, uint64_t &value, std::string &why)
{
    std::vector<compiler::Diagnostic> diagnostics;
    const std::vector<Token> tokens = tokenise(source, diagnostics);
    Types types;
    Unit unit;
    if (!parse(tokens, types, unit, diagnostics)) {
        why = "it did not parse";
        return false;
    }

    ir::Target target;
    if (!ir::Target::from_language_id(language_id, target, why))
        return false;

    ir::Unit lowered;
    if (!lower_to_ir(unit, types, target, lowered, diagnostics)) {
        why = diagnostics.empty() ? "it did not lower" : diagnostics.back().message;
        return false;
    }

    std::vector<pcode::Sequence> written;
    for (const ir::Function &function : lowered.functions) {
        pcode::Sequence sequence;
        std::vector<std::string> problems;
        if (!pcode::to_pcode(function, lowered.target, sequence, problems)) {
            why = problems.empty() ? "it was not written" : problems.front();
            return false;
        }
        written.push_back(std::move(sequence));
    }

    pcode::Machine machine;
    for (const pcode::Sequence &sequence : written)
        machine.others[sequence.name] = &sequence;

    for (const pcode::Sequence &sequence : written) {
        if (sequence.name != entry)
            continue;
        const pcode::Answer answer = pcode::run(sequence, machine);
        if (!answer.ok) {
            why = answer.error;
            return false;
        }
        value = answer.value;
        return true;
    }
    why = "there is nothing called " + entry;
    return false;
}

} // namespace

void check_calling()
{
    const char *arm = "AARCH64:LE:64:AppleSilicon";

    // The argument is put where the callee will look for it, and the answer
    // comes back where the caller looks. Neither end was told the other's
    // choice: both asked the same specification.
    {
        uint64_t value = 0;
        std::string why;
        const bool ran = answer_from(
            "func twice(value: i32): i32 {\n  return value + value;\n}\n"
            "func main(): i32 {\n  return twice(21);\n}\n",
            "main", arm, value, why);
        report(ran && value == 42, "a call agrees with what it calls about where the argument is",
               ran ? "got " + std::to_string(value) : why);
    }

    // Two arguments, so the second has to go somewhere different from the
    // first, and in the right order.
    {
        uint64_t value = 0;
        std::string why;
        const bool ran = answer_from(
            "func subtract(first: i32, second: i32): i32 {\n  return first - second;\n}\n"
            "func main(): i32 {\n  return subtract(50, 8);\n}\n",
            "main", arm, value, why);
        report(ran && value == 42, "two arguments go to two places, and in the order written",
               ran ? "got " + std::to_string(value) : why);
    }

    // A call inside a call, which only works if the answer is taken before the
    // registers are used again.
    {
        uint64_t value = 0;
        std::string why;
        const bool ran = answer_from(
            "func twice(value: i32): i32 {\n  return value + value;\n}\n"
            "func main(): i32 {\n  return twice(twice(10));\n}\n",
            "main", arm, value, why);
        report(ran && value == 40, "a call whose argument is another call answers correctly",
               ran ? "got " + std::to_string(value) : why);
    }

    // Calling something that is not there stops rather than guessing.
    {
        uint64_t value = 0;
        std::string why;
        const bool ran = answer_from("func main(): i32 {\n  return missing(1);\n}\n", "main", arm,
                                     value, why);
        report(!ran && why.find("nothing here called") != std::string::npos,
               "calling something that is not there stops rather than guessing",
               ran ? "it ran" : why);
    }
}


// A frame slot has to become somewhere real, which means the register the
// processor measures frames from plus the offset.
void check_frames_become_addresses()
{
    std::vector<compiler::Diagnostic> diagnostics;
    const std::string source =
        "func total(): i32 {\n  var running: i32 = 2;\n  return running + 40;\n}\n";
    const std::vector<Token> tokens = tokenise(source, diagnostics);
    Types types;
    Unit unit;
    if (!parse(tokens, types, unit, diagnostics)) {
        report(false, "the source parses", "it did not");
        return;
    }

    ir::Target target;
    std::string why;
    if (!ir::Target::from_language_id("AARCH64:LE:64:AppleSilicon", target, why)) {
        report(false, "the target is read", why);
        return;
    }

    ir::Unit lowered;
    if (!lower_to_ir(unit, types, target, lowered, diagnostics) || lowered.functions.empty()) {
        report(false, "it lowers", "it did not");
        return;
    }

    pcode::Sequence sequence;
    std::vector<std::string> problems;
    if (!pcode::to_pcode(lowered.functions.front(), lowered.target, sequence, problems)) {
        report(false, "it is written", problems.empty() ? "" : problems.front());
        return;
    }
    const std::string text = pcode::to_text(sequence);

    // The stack register, at the offset the specification gives it, added to.
    const ir::Target::RegisterPlace *stack = lowered.target.register_place("sp");
    if (stack == nullptr) {
        report(false, "this processor has a stack register", "it has none");
        return;
    }
    char wanted[64];
    std::snprintf(wanted, sizeof wanted, "INT_ADD (register, 0x%llx,",
                  static_cast<unsigned long long>(stack->offset));
    report(contains(text, wanted), "a frame slot is the stack register plus an offset", text);

    // Two four-byte values added together answer in four bytes. Falling through
    // to the machine word made this eight, which is an instruction wider than
    // anything asked for.
    report(contains(text, "INT_ADD (unique, 0x14, 4) (unique, 0x18, 4)") ||
               !contains(text, ", 8)\n"),
           "adding two four-byte values answers in four", text);
}


// Taking the room the locals need, and giving it back.
void check_the_frame_is_taken_and_given_back()
{
    const char *arm = "AARCH64:LE:64:AppleSilicon";

    // A function with two ways out gives the room back at both. Giving it back
    // at one of them is a stack that never comes back.
    {
        uint64_t taken = 0;
        uint64_t otherwise = 0;
        std::string why;
        const char *source =
            "func which(@w0): i32 {\n"
            "  var held: i32 = 5;\n"
            "  if (w0 == 0) {\n"
            "    return held;\n"
            "  } else {\n"
            "    return held + 1;\n"
            "  }\n"
            "}\n";
        const bool first = answer_for(source, arm, {{"w0", 0}}, taken, why);
        const bool second = answer_for(source, arm, {{"w0", 1}}, otherwise, why);
        report(first && second && taken == 5 && otherwise == 6,
               "a function with two ways out works through both of them",
               first && second ? "got " + std::to_string(taken) + " and " +
                                     std::to_string(otherwise)
                               : why);
    }

    // A function that touches no frame takes none.
    {
        std::vector<compiler::Diagnostic> diagnostics;
        const std::string source = "func plain(@w0): i32 {\n  return w0 + w0;\n}\n";
        const std::vector<Token> tokens = tokenise(source, diagnostics);
        Types types;
        Unit unit;
        ir::Target target;
        std::string why;
        if (parse(tokens, types, unit, diagnostics) &&
            ir::Target::from_language_id(arm, target, why)) {
            ir::Unit lowered;
            if (lower_to_ir(unit, types, target, lowered, diagnostics) &&
                !lowered.functions.empty()) {
                report(lowered.functions.front().frame_bytes == 0,
                       "a function that touches no frame takes none",
                       std::to_string(lowered.functions.front().frame_bytes) + " bytes");

                pcode::Sequence sequence;
                std::vector<std::string> problems;
                if (pcode::to_pcode(lowered.functions.front(), lowered.target, sequence,
                                    problems)) {
                    const std::string text = pcode::to_text(sequence);
                    report(!contains(text, "INT_SUB (register"),
                           "and so does not move the stack at all", text);
                }
            }
        }
    }

    // One that does, takes exactly what its locals need and gives it back.
    {
        std::vector<compiler::Diagnostic> diagnostics;
        const std::string source =
            "func holds(): i32 {\n  var first: i32 = 1;\n  var second: i32 = 2;\n"
            "  return first + second;\n}\n";
        const std::vector<Token> tokens = tokenise(source, diagnostics);
        Types types;
        Unit unit;
        ir::Target target;
        std::string why;
        if (parse(tokens, types, unit, diagnostics) &&
            ir::Target::from_language_id(arm, target, why)) {
            ir::Unit lowered;
            if (lower_to_ir(unit, types, target, lowered, diagnostics) &&
                !lowered.functions.empty()) {
                report(lowered.functions.front().frame_bytes == 8,
                       "two four-byte locals need eight bytes of frame",
                       std::to_string(lowered.functions.front().frame_bytes) + " bytes");
            }
        }
        uint64_t value = 0;
        const bool ran = answer_for(source, arm, {}, value, why);
        report(ran && value == 3, "and the function still answers correctly",
               ran ? "got " + std::to_string(value) : why);
    }
}

} // namespace

// Leaving is two things, and the second one carries nothing.
//
// A processor's return instruction says nothing whatever about a value: it
// reads the register the return address is in and goes there. A caller finds an
// answer only because the convention agreed beforehand where one would be
// sitting, so what a function does on the way out is put the answer in that
// place and then go. Handing the value to the return instead asked every
// processor for a return that takes an argument, and none has one.
// Splitting a value too wide for a register, checked by running it.
//
// A wide operation rewritten as operations over halves is only right if the
// answer comes out the same, so the same sequence is run twice: once whole, and
// once after splitting. Anything that disagrees is a wrong translation, which is
// the one kind of mistake that decodes perfectly and is therefore invisible
// without this.
//
// It also watches which operations come out. Two of the ways of splitting used
// operations no processor here has a single instruction for - a carry, and a
// one-byte boolean join - so the split succeeded and selection then refused the
// whole function. Producing only operations a machine plausibly has is part of
// being right.
void check_wide_operations_split_and_still_mean_it()
{
    ir::Target target;
    std::string trouble;
    if (!ir::Target::from_language_id("MIPS:BE:32:default", target, trouble) ||
        !target.read_specification(trouble)) {
        report(false, "a processor to split for", trouble);
        return;
    }

    // Building the operations here rather than lowering source to them, because
    // what is under test is one pass and the shapes it has to handle, not the
    // ones a particular piece of source happens to produce.
    uint64_t next = 0;
    auto wide = [&]() {
        pcode::Varnode node;
        node.where = pcode::Where::Unique;
        node.offset = next;
        node.size = 8;
        next += 8;
        return node;
    };
    auto number = [](uint64_t value, int size) {
        pcode::Varnode node;
        node.where = pcode::Where::Constant;
        node.offset = value;
        node.size = size;
        return node;
    };
    auto step = [](ghidra::OpCode opcode, const pcode::Varnode &into,
                   const std::vector<pcode::Varnode> &from) {
        pcode::Operation made;
        made.opcode = opcode;
        made.writes = true;
        made.output = into;
        made.inputs = from;
        return made;
    };

    struct Case {
        const char *what;
        ghidra::OpCode opcode;
        uint64_t left;
        uint64_t right;
        bool right_is_amount;
    };

    // Values chosen so the answer depends on what crosses between the halves:
    // an addition that carries out of the low half, a subtraction that borrows
    // into it, and shifts that move bits over the join in both directions.
    const std::vector<Case> cases = {
        {"adding carries out of the low half", ghidra::CPUI_INT_ADD,
         0x00000001ffffffffull, 0x0000000000000002ull, false},
        {"adding without a carry", ghidra::CPUI_INT_ADD, 0x0000000100000001ull,
         0x0000000200000002ull, false},
        {"subtracting borrows into the high half", ghidra::CPUI_INT_SUB,
         0x0000000200000001ull, 0x0000000000000002ull, false},
        {"an exclusive-or over both halves", ghidra::CPUI_INT_XOR, 0xdeadbeefcafef00dull,
         0x0123456789abcdefull, false},
        {"shifting up over the join", ghidra::CPUI_INT_LEFT, 0x00000000ffff0001ull, 20,
         true},
        {"shifting up by a whole half and more", ghidra::CPUI_INT_LEFT,
         0x00000000abcd1234ull, 40, true},
        {"shifting down over the join", ghidra::CPUI_INT_RIGHT, 0xffff000100000000ull, 20,
         true},
        {"shifting down by a whole half and more", ghidra::CPUI_INT_RIGHT,
         0xabcd123400000000ull, 40, true},
        {"shifting a signed value down over the join", ghidra::CPUI_INT_SRIGHT,
         0xffffffff80000000ull, 12, true},
        {"shifting a signed value down past the join", ghidra::CPUI_INT_SRIGHT,
         0x8000000000000000ull, 40, true},
        {"shifting by nothing at all", ghidra::CPUI_INT_LEFT, 0x0123456789abcdefull, 0,
         true},
    };

    bool all_agreed = true;
    bool all_nameable = true;
    std::string first_disagreement;
    std::string first_unnameable;

    for (const Case &one : cases) {
        next = 0;
        const pcode::Varnode left = wide();
        const pcode::Varnode right = wide();
        const pcode::Varnode got = wide();

        pcode::Varnode answered;
        answered.where = pcode::Where::Unique;
        answered.offset = next;
        answered.size = 1;
        next += 8;

        pcode::Block block;
        block.identifier = 0;
        block.operations.push_back(
            step(ghidra::CPUI_COPY, left, {number(one.left, 8)}));
        if (one.right_is_amount) {
            block.operations.push_back(
                step(one.opcode, got, {left, number(one.right, 4)}));
        } else {
            block.operations.push_back(
                step(ghidra::CPUI_COPY, right, {number(one.right, 8)}));
            block.operations.push_back(step(one.opcode, got, {left, right}));
        }

        // The answer is compared inside the sequence rather than read out of it,
        // because a wide value has no single place to be read from once it has
        // been split, and a comparison is the operation the splitting has to get
        // right anyway.
        const pcode::Varnode wanted = wide();
        block.operations.push_back(
            step(ghidra::CPUI_COPY, wanted, {number(0, 8)}));
        block.operations.push_back(
            step(ghidra::CPUI_INT_EQUAL, answered, {got, wanted}));

        pcode::Operation leaving;
        leaving.opcode = ghidra::CPUI_RETURN;
        block.operations.push_back(leaving);

        pcode::Sequence whole;
        whole.name = one.what;
        whole.entry = 0;
        whole.blocks.push_back(block);
        whole.answer = got;  // read out whole, before anything is split

        const pcode::Answer before = pcode::run(whole, pcode::Machine());
        if (!before.ok) {
            all_agreed = false;
            if (first_disagreement.empty())
                first_disagreement = std::string(one.what) + ": " + before.error;
            continue;
        }

        // And now the same thing again, with the wide value carried in two, and
        // the answer read as its two halves put back together.
        pcode::Sequence split = whole;
        std::vector<std::string> problems;
        if (!pcode::split_wide(split, target, 4, problems)) {
            all_agreed = false;
            if (first_disagreement.empty())
                first_disagreement = std::string(one.what) + ": " +
                                     (problems.empty() ? "refused" : problems.front());
            continue;
        }

        // Where the two halves of the answer ended up. The pass writes them, so
        // the sequence is asked rather than guessed at: the last two operations
        // that wrote a half of `got` name them.
        // A split value has no single place to be read from any more, so the
        // answer is taken out through operations the pass also has to handle:
        // the low half by taking the low bytes, the high half by shifting down
        // first. Registers are what survives into the answer, so both go there.
        const ir::Target::RegisterPlace *low_reg = target.register_place("v0");
        const ir::Target::RegisterPlace *high_reg = target.register_place("v1");
        if (low_reg == nullptr || high_reg == nullptr) {
            all_agreed = false;
            first_disagreement = "this processor has no v0 and v1";
            break;
        }
        pcode::Varnode into_low;
        into_low.where = pcode::Where::Register;
        into_low.offset = low_reg->offset;
        into_low.size = 4;
        pcode::Varnode into_high = into_low;
        into_high.offset = high_reg->offset;

        // SUBPIECE of nothing takes the low half; the high half is the same
        // value shifted down by one half, which the pass also has to split.
        pcode::Sequence reading = whole;
        auto &ops = reading.blocks.front().operations;
        ops.insert(ops.end() - 1,
                   step(ghidra::CPUI_SUBPIECE, into_low, {got, number(0, 4)}));
        pcode::Varnode moved = wide();
        ops.insert(ops.end() - 1, step(ghidra::CPUI_INT_RIGHT, moved, {got, number(32, 4)}));
        ops.insert(ops.end() - 1,
                   step(ghidra::CPUI_SUBPIECE, into_high, {moved, number(0, 4)}));
        reading.answer = pcode::Varnode();

        std::vector<std::string> more;
        if (!pcode::split_wide(reading, target, 4, more)) {
            all_agreed = false;
            if (first_disagreement.empty())
                first_disagreement = std::string(one.what) + ": " +
                                     (more.empty() ? "refused" : more.front());
            continue;
        }
        const pcode::Answer after = pcode::run(reading, pcode::Machine());
        if (!after.ok) {
            all_agreed = false;
            if (first_disagreement.empty())
                first_disagreement = std::string(one.what) + ": " + after.error;
            continue;
        }
        uint64_t rebuilt = 0;
        auto low_seen = after.registers.find(low_reg->offset);
        auto high_seen = after.registers.find(high_reg->offset);
        if (low_seen != after.registers.end())
            rebuilt |= low_seen->second & 0xffffffffull;
        if (high_seen != after.registers.end())
            rebuilt |= (high_seen->second & 0xffffffffull) << 32;

        if (rebuilt != before.value) {
            all_agreed = false;
            if (first_disagreement.empty()) {
                char said[256];
                std::snprintf(said, sizeof said, "%s: whole gave %llx, split gave %llx",
                              one.what, (unsigned long long)before.value,
                              (unsigned long long)rebuilt);
                first_disagreement = said;
            }
        }

        // Nothing may come out that a processor has no single instruction for.
        // A carry and a one-byte boolean join both did, and both were refused at
        // selection after splitting had already said it succeeded.
        for (const pcode::Operation &made : split.blocks.front().operations) {
            if (made.opcode == ghidra::CPUI_INT_CARRY ||
                made.opcode == ghidra::CPUI_BOOL_AND ||
                made.opcode == ghidra::CPUI_BOOL_OR) {
                all_nameable = false;
                if (first_unnameable.empty())
                    first_unnameable = std::string(one.what) + " produced a " +
                                       pcode::opcode_name(made.opcode);
            }
            if (made.writes && made.output.size > 4 &&
                made.output.where == pcode::Where::Unique) {
                all_nameable = false;
                if (first_unnameable.empty())
                    first_unnameable =
                        std::string(one.what) + " left a value eight bytes wide behind";
            }
        }
    }

    report(all_agreed, "a value too wide for a register means the same carried in two",
           first_disagreement);
    report(all_nameable,
           "and splitting one leaves only operations a processor has an instruction for",
           first_unnameable);
}

void check_leaving_puts_the_answer_in_its_place()
{
    ir::Target target;
    std::string why;
    if (!ir::Target::from_language_id("MIPS:BE:32:default", target, why) ||
        !target.read_specification(why)) {
        report(false, "the processor is read", why);
        return;
    }

    ir::Builder builder("answers");
    builder.block();
    builder.ret(builder.constant(42, 4));

    ir::Function function;
    std::vector<std::string> problems;
    if (!builder.finish(function, problems)) {
        report(false, "a function that answers with something is built",
               problems.empty() ? "" : problems.front());
        return;
    }

    pcode::Sequence sequence;
    if (!pcode::to_pcode(function, target, sequence, problems)) {
        report(false, "and written as p-code", problems.empty() ? "" : problems.front());
        return;
    }

    // Where the answer goes is what this processor's convention says, not
    // anything chosen here: v0 is where a MIPS function leaves one.
    const ir::Target::RegisterPlace *expected = target.register_place("v0");
    report(expected != nullptr && sequence.answer.where == pcode::Where::Register &&
               sequence.answer.offset == expected->offset,
           "a function knows where it leaves its answer", "it named no place");

    // And the return itself reads nothing at all.
    bool leaves_empty_handed = false;
    for (const pcode::Block &block : sequence.blocks) {
        for (const pcode::Operation &operation : block.operations) {
            if (operation.opcode == ghidra::CPUI_RETURN)
                leaves_empty_handed = operation.inputs.empty();
        }
    }
    report(leaves_empty_handed, "and going back reads nothing, the way an instruction that goes "
                                "back does",
           "the return was handed a value no processor's return instruction takes");

    // What it answers with is still forty-two, read from where it was put.
    const pcode::Answer ran = pcode::run(sequence, pcode::Machine());
    report(ran.ok && ran.value == 42, "and the answer is still what the function said it was",
           ran.ok ? "got " + std::to_string(ran.value) : ran.error);
}

int main()
{
    if (initialize(nullptr) != ASTRAL_OK) {
        std::printf("FAIL no compiled specifications were found; set ASTRAL_SPECS\n");
        return 1;
    }

    check_a_truth_is_one_byte();
    check_signedness_picks_the_opcode();
    check_addition_does_not_care();
    check_the_two_that_read_backwards();
    check_a_pinned_value_is_its_register();
    check_raw_is_refused();
    check_it_computes_what_the_source_said();
    check_choosing_and_going();
    check_counting();
    check_reaching_and_choosing();
    check_calling();
    check_frames_become_addresses();
    check_the_frame_is_taken_and_given_back();
    check_leaving_puts_the_answer_in_its_place();
    check_wide_operations_split_and_still_mean_it();

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
