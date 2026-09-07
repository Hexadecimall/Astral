// Lowering Nova into the representation, checked on source rather than on a
// tree built by hand.
//
// What is worth checking is not that something came out, but that the things a
// C tree drops came through: a parameter pinned to a register is still pinned,
// a function still knows the address it may not move from, and a body that is
// already instructions is still those bytes. Each of those is the reason for
// lowering this way instead of the other way, so each has a test.
#include "ir.hh"
#include "lexer.hh"
#include "lower_ir.hh"
#include "parser.hh"

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

std::string diagnostics_text(const std::vector<compiler::Diagnostic> &diagnostics)
{
    std::string text;
    for (const compiler::Diagnostic &diagnostic : diagnostics)
        text += "\n     " + std::to_string(diagnostic.line) + ": " + diagnostic.message;
    return text.empty() ? std::string("no diagnostics") : text;
}

// Reads Nova and lowers it, answering with the representation written out.
// Empty means it did not lower, and `why` says what was said about it.
std::string lowered_text(const std::string &source, const std::string &language_id,
                         std::string &why)
{
    std::vector<compiler::Diagnostic> diagnostics;
    const std::vector<Token> tokens = tokenise(source, diagnostics);

    Types types;
    Unit unit;
    if (!parse(tokens, types, unit, diagnostics)) {
        why = "it did not parse:" + diagnostics_text(diagnostics);
        return std::string();
    }

    ir::Target target;
    std::string error;
    if (!ir::Target::from_language_id(language_id, target, error)) {
        why = error;
        return std::string();
    }

    ir::Unit lowered;
    if (!lower_to_ir(unit, types, target, lowered, diagnostics)) {
        why = "it did not lower:" + diagnostics_text(diagnostics);
        return std::string();
    }
    if (lowered.functions.empty()) {
        why = "nothing came out";
        return std::string();
    }

    std::string text;
    for (const ir::Function &function : lowered.functions)
        text += ir::to_text(function);
    return text;
}

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

void check_plain_function()
{
    std::string why;
    const std::string text = lowered_text(
        "func answer(): i32 {\n"
        "    return 7;\n"
        "}\n",
        "AARCH64:LE:64:AppleSilicon", why);

    if (text.empty()) {
        report(false, "a function that returns a number lowers", why);
        return;
    }
    report(contains(text, "constant"), "the number it returns became a constant", text);
    report(contains(text, "return"), "the function leaves", text);
}

void check_arithmetic()
{
    std::string why;
    const std::string text = lowered_text(
        "func total(): i32 {\n"
        "    var running: i32 = 2;\n"
        "    running = running + 40;\n"
        "    return running;\n"
        "}\n",
        "AARCH64:LE:64:AppleSilicon", why);

    if (text.empty()) {
        report(false, "arithmetic over a local lowers", why);
        return;
    }
    report(contains(text, "add"), "the addition is an add", text);
    // A local is a frame slot, read and written through its address, which is
    // what the generator already does and what makes the two comparable.
    report(contains(text, "frameaddress"), "the local became a frame slot", text);
    report(contains(text, "store") && contains(text, "load"),
           "the local is written and read through that slot", text);
}

void check_pinned_parameter()
{
    std::string why;
    const std::string text = lowered_text(
        "func doubled(@w0): i32 {\n"
        "    return w0 + w0;\n"
        "}\n",
        "AARCH64:LE:64:AppleSilicon", why);

    if (text.empty()) {
        report(false, "a function whose parameter is a register lowers", why);
        return;
    }
    // This is the whole point. The C tree had nowhere to put it, so it was
    // compared against the ABI and thrown away; here it survives.
    report(contains(text, "@w0"), "a parameter pinned to a register is still pinned", text);
    report(contains(text, "level storage"),
           "a function that says storage and no types is read as level 1", text);

    // Below level 2 there is no type, so the register is the only thing that
    // says how wide the value is. `w0` and `x0` are the same processor and four
    // bytes apart, and taking the machine word would be wrong about one of them
    // every time.
    report(contains(text, "add.4"), "a four-byte register makes a four-byte addition", text);

    std::string wide_why;
    const std::string wide = lowered_text(
        "func doubled(@x0): i64 {\n"
        "    return x0 + x0;\n"
        "}\n",
        "AARCH64:LE:64:AppleSilicon", wide_why);
    report(!wide.empty() && contains(wide, "add.8"),
           "an eight-byte register makes an eight-byte addition",
           wide.empty() ? wide_why : wide);
}

void check_fixed_address()
{
    std::string why;
    const std::string text = lowered_text(
        "func check @ 0x100000500 (): i32 {\n"
        "    return 1;\n"
        "}\n",
        "AARCH64:LE:64:AppleSilicon", why);

    if (text.empty()) {
        // The syntax for pinning a function's address may not be written this
        // way; say what happened rather than claim the feature is broken.
        report(false, "a function pinned to an address lowers", why);
        return;
    }
    report(contains(text, "at 0x100000500"), "the address it may not move from travels with it",
           text);
}

void check_branch()
{
    std::string why;
    const std::string text = lowered_text(
        "func either(): i32 {\n"
        "    var chosen: i32 = 0;\n"
        "    if (chosen == 0) {\n"
        "        return 1;\n"
        "    } else {\n"
        "        return 2;\n"
        "    }\n"
        "}\n",
        "AARCH64:LE:64:AppleSilicon", why);

    if (text.empty()) {
        report(false, "an if with both halves lowers", why);
        return;
    }
    report(contains(text, "branch"), "the decision became a branch", text);
    report(contains(text, "equal"), "the comparison came through", text);
}

// The same source, lowered for two processors that disagree about how wide a
// machine word is. Nothing about the lowering is written per-processor, so the
// difference has to come from the target alone.
void check_target_changes_widths()
{
    std::string why;
    const std::string wide = lowered_text("func f(): i32 { return 1; }\n",
                                          "AARCH64:LE:64:AppleSilicon", why);
    const std::string narrow = lowered_text("func f(): i32 { return 1; }\n",
                                            "z80:LE:16:default", why);
    report(!wide.empty() && !narrow.empty(),
           "the same source lowers for a sixty-four bit and a sixteen bit machine",
           wide.empty() || narrow.empty() ? why : "");
}

// A dereference is as wide as what is pointed at, not as wide as the pointer or
// the machine word. `*(text as *char)` was coming out as an eight-byte load on
// a sixty-four bit processor, which read seven bytes that were never asked
// for, and on a thirty-two bit one asked for a value no register could hold.
// A `u64` local is still eight bytes: the width has to be the source's own,
// narrower in one place and no narrower in the other.
void check_dereference_is_as_wide_as_the_pointee()
{
    std::string why;
    const std::string text = lowered_text(
        "func f(text: i64): u64 {\n"
        "    var length: u64 = 7;\n"
        "    if (*((text + length) as *char) != '\\0') {\n"
        "        *((text + 1) as *char) = 'x';\n"
        "    }\n"
        "    return length;\n"
        "}\n",
        "AARCH64:LE:64:AppleSilicon", why);

    if (text.empty()) {
        report(false, "a dereference through a cast pointer lowers", why);
        return;
    }
    report(contains(text, "load.1"), "reading a *char is a one-byte load", text);
    report(contains(text, "store.1 ") || contains(text, "store.1.signed"),
           "writing through a *char is a one-byte store", text);
    report(contains(text, "load.8"), "the u64 local is still read eight bytes wide", text);
}

// Loops. Each shape has a different place the test sits and a different place
// `continue` goes, which is the whole of what distinguishes them.
void check_loops()
{
    {
        std::string why;
        const std::string text = lowered_text(
            "func counts(): i32 {\n"
            "    var seen: i32 = 0;\n"
            "    while (seen < 10) {\n"
            "        seen = seen + 1;\n"
            "    }\n"
            "    return seen;\n"
            "}\n",
            "AARCH64:LE:64:AppleSilicon", why);
        if (text.empty()) {
            report(false, "a while lowers", why);
        } else {
            report(contains(text, "branch"), "the while tests and branches", text);
            report(contains(text, "less"), "the comparison came through", text);
        }
    }

    {
        std::string why;
        const std::string text = lowered_text(
            "func once(): i32 {\n"
            "    var seen: i32 = 0;\n"
            "    do {\n"
            "        seen = seen + 1;\n"
            "    } while (seen < 2);\n"
            "    return seen;\n"
            "}\n",
            "AARCH64:LE:64:AppleSilicon", why);
        report(!text.empty() && contains(text, "branch"), "a do while lowers",
               text.empty() ? why : text);
    }

    // `loop` has nothing to test, so the only way out is a break, and the block
    // a break goes to has to exist for it to go there.
    {
        std::string why;
        const std::string text = lowered_text(
            "func breaks(): i32 {\n"
            "    loop {\n"
            "        break;\n"
            "    }\n"
            "    return 3;\n"
            "}\n",
            "AARCH64:LE:64:AppleSilicon", why);
        report(!text.empty() && contains(text, "jump"), "a loop with a break in it lowers",
               text.empty() ? why : text);
    }

    // A break outside any loop has nowhere to go and is refused rather than
    // quietly jumping somewhere.
    {
        std::string why;
        const std::string text = lowered_text(
            "func stray(): i32 {\n"
            "    break;\n"
            "    return 0;\n"
            "}\n",
            "AARCH64:LE:64:AppleSilicon", why);
        report(text.empty() && why.find("outside any loop") != std::string::npos,
               "a break outside any loop is refused", text.empty() ? why : text);
    }

    // A break in a nested loop leaves the loop it is written in, not the
    // outermost one, which is what the innermost-last stack is for.
    {
        std::string why;
        const std::string text = lowered_text(
            "func nested(): i32 {\n"
            "    var outer: i32 = 0;\n"
            "    while (outer < 3) {\n"
            "        while (outer < 2) {\n"
            "            break;\n"
            "        }\n"
            "        outer = outer + 1;\n"
            "    }\n"
            "    return outer;\n"
            "}\n",
            "AARCH64:LE:64:AppleSilicon", why);
        report(!text.empty(), "a break inside a nested loop lowers", why);
    }
}

void check_refuses_rather_than_drops()
{
    // A statement that is not lowered yet has to be refused. Quietly leaving it
    // out is machine code that does less than the source said, which is the
    // failure nothing downstream can catch.
    std::string why;
    const std::string text = lowered_text(
        "func counts(): i32 {\n"
        "    var numbers: i32 = {1, 2, 3};\n"
        "    return numbers;\n"
        "}\n",
        "AARCH64:LE:64:AppleSilicon", why);
    report(text.empty() && why.find("not lowered yet") != std::string::npos,
           "a statement that is not lowered yet is refused rather than dropped",
           text.empty() ? why : text);
}

// A string is bytes that have to be in the image, and the expression is where
// they are. Nothing about it is a value: what the code carries is an address,
// and the bytes travel with the unit until somebody lays it out.
void check_a_string_becomes_bytes_and_an_address()
{
    std::string why;
    const std::string text = lowered_text(
        "func greets(): *char {\n"
        "    return \"hello\";\n"
        "}\n",
        "AARCH64:LE:64:AppleSilicon", why);
    report(!text.empty() && contains(text, "globaladdress"),
           "a string in the source becomes the address of bytes in the image",
           text.empty() ? why : text);
}


// What a recovered function is actually written with.
//
// These are not exotic. Every one of them appeared in the first twenty
// functions a real decompilation produced, and each stopped the whole function
// dead until it lowered - so what is checked is that each comes through, on
// source shaped the way a recovered function is shaped rather than written to
// suit.
void check_what_recovered_code_is_written_with()
{
    const char *arm = "AARCH64:LE:64:AppleSilicon";
    std::string why;

    // An argument is not a local, and something indexed through one has to know
    // what an element of it is all the same.
    {
        const std::string text = lowered_text(
            "func first(buffer: *char): i32 {\n"
            "    return buffer[0] as i32;\n"
            "}\n",
            arm, why);
        report(!text.empty(), "something indexed through an argument lowers",
               text.empty() ? why : "");
    }

    // And and or are a decision, not an operation over two values. The right
    // half runs only when the left did not already settle it, which is what
    // stops a recovered condition reading past the end of what it is walking.
    {
        const std::string text = lowered_text(
            "func both(text: *char, length: i64): bool {\n"
            "    return (length != 0) && (text[0] != '\\0');\n"
            "}\n",
            arm, why);
        report(!text.empty() && contains(text, "branch"),
               "an and is lowered as the decision it is, so the second half may not run",
               text.empty() ? why : text);
    }

    // Not asks whether something is nothing. Flipping its bits answers a
    // different question: the negation of three is minus four, and three as a
    // truth is true.
    {
        const std::string text = lowered_text(
            "func absent(value: i32): bool {\n"
            "    return !value;\n"
            "}\n",
            arm, why);
        report(!text.empty() && contains(text, "equal") && !contains(text, "bitnot"),
               "a not asks whether something is nothing rather than flipping its bits",
               text.empty() ? why : text);
    }

    // Stepping something by one, where which value the expression is, is the
    // whole difference between the two spellings. Only the one written in front
    // is checked, because that is the one the parser reads - it does not take
    // the other yet, and a test cannot claim otherwise.
    {
        const std::string text = lowered_text(
            "func counts(): i32 {\n"
            "    var index: i32 = 0;\n"
            "    ++index;\n"
            "    return index;\n"
            "}\n",
            arm, why);
        report(!text.empty() && contains(text, "add"), "a step by one lowers",
               text.empty() ? why : text);
    }

    // A comma is two things one after the other, and recovered code uses it to
    // say that something was assigned on the way through a condition.
    {
        const std::string text = lowered_text(
            "func aside(value: i32): i32 {\n"
            "    var kept: i32 = 0;\n"
            "    return (kept = value, kept + 1);\n"
            "}\n",
            arm, why);
        report(!text.empty(), "a comma runs the first for what it does and answers with the second",
               text.empty() ? why : "");
    }
}

// A parameter nothing was said about goes where this processor's calling
// convention puts it. The source is the same every time; only the processor
// differs, and nothing about the lowering is written per-processor.
void check_unpinned_parameters_follow_the_convention()
{
    const char *source =
        "func adds(first: i32, second: i32): i32 {\n"
        "  return first + second;\n"
        "}\n";

    struct Case {
        const char *language;
        const char *first;
        const char *second;
        const char *what;
    };
    const Case cases[] = {
        {"AARCH64:LE:64:AppleSilicon", "@w0", "@w1", "AARCH64 puts the first two in w0 and w1"},
        {"RISCV:LE:64:default", "@a0", "@a1", "RISC-V puts them in a0 and a1"},
        // Nothing in a register at all: a thirty-two bit x86 reads both off the
        // stack, at the offsets its convention gives them.
        {"x86:LE:32:default", "@+0x4", "@+0x8",
         "thirty-two bit x86 reads both off the stack instead"},
    };

    for (const Case &one : cases) {
        std::string why;
        const std::string text = lowered_text(source, one.language, why);
        if (text.empty()) {
            report(false, one.what, why);
            continue;
        }
        report(contains(text, one.first) && contains(text, one.second), one.what, text);
    }

    // And a parameter that was pinned keeps what the source said, rather than
    // being moved to wherever the convention would have put it.
    {
        std::string why;
        const std::string text = lowered_text("func doubled(@w5): i32 {\n  return w5 + w5;\n}\n",
                                              "AARCH64:LE:64:AppleSilicon", why);
        report(!text.empty() && contains(text, "@w5"),
               "a parameter the source pinned stays where the source put it",
               text.empty() ? why : text);
    }
}

} // namespace

int main()
{
    if (initialize(nullptr) != ASTRAL_OK) {
        std::printf("FAIL no compiled specifications were found; set ASTRAL_SPECS\n");
        return 1;
    }

    check_plain_function();
    check_arithmetic();
    check_pinned_parameter();
    check_fixed_address();
    check_branch();
    check_target_changes_widths();
    check_dereference_is_as_wide_as_the_pointee();
    check_loops();
    check_unpinned_parameters_follow_the_convention();
    check_refuses_rather_than_drops();
    check_a_string_becomes_bytes_and_an_address();
    check_what_recovered_code_is_written_with();

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
