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

} // namespace

int main()
{
    if (initialize(nullptr) != ASTRAL_OK) {
        std::printf("FAIL no compiled specifications were found; set ASTRAL_SPECS\n");
        return 1;
    }

    check_signedness_picks_the_opcode();
    check_addition_does_not_care();
    check_the_two_that_read_backwards();
    check_a_pinned_value_is_its_register();
    check_raw_is_refused();
    check_it_computes_what_the_source_said();
    check_choosing_and_going();
    check_counting();
    check_reaching_and_choosing();

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
