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

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
