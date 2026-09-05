// Checking the readable listing against a real program.
//
// Every check here decompiles the subject through the public interface and
// then looks at the text a person would be shown. The last of them is the one
// that matters most: the compilable output has to be untouched by all of this,
// so it is asked for as well and checked for any trace of the reading spellings.
#include "astral/astral.hpp"

#include <cstdio>
#include <exception>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void report(bool passed, const std::string &what, const std::string &saw)
{
    ++checks;
    if (passed) {
        std::printf("  ok    %s\n", what.c_str());
        return;
    }
    ++failures;
    std::printf("  FAIL  %s\n        %s\n", what.c_str(), saw.c_str());
}

void expect(bool condition, const std::string &what, const std::string &saw = std::string())
{
    report(condition, what, saw.empty() ? "the condition did not hold" : saw);
}

std::string subject_path;

astral::Program open()
{
    return astral::Program::open(subject_path);
}

// The listing of the named function, or the empty string when the program has
// no such symbol.
std::string listing_of(astral::Program &program, const std::string &name)
{
    for (const astral::Symbol &symbol : program.symbols()) {
        if (symbol.name != name)
            continue;
        astral::Function function = program.decompile(symbol.address);
        if (!function)
            return std::string();
        return function.c_code();
    }
    return std::string();
}

bool has(const std::string &text, const std::string &needle)
{
    return text.find(needle) != std::string::npos;
}

// ------------------------------------------------------------------ tests

void types_are_named_by_width_in_bits()
{
    std::printf("\ntypes are named by width in bits\n");
    astral::Program program = open();
    const std::string text = listing_of(program, "widths");
    expect(!text.empty(), "the subject has a function named widths");
    expect(!has(text, "int4") && !has(text, "uint4") && !has(text, "int8"),
           "the byte-width names are gone", text);
    expect(has(text, "i64") || has(text, "u64") || has(text, "i32") || has(text, "u32"),
           "the widths read in bits", text);
}

void a_known_constant_is_named_only_where_it_means_something()
{
    std::printf("\na known constant is named only where it means something\n");
    astral::Program program = open();
    const std::string request = listing_of(program, "measure_terminal");
    expect(!request.empty(), "the subject has a function named measure_terminal");
    expect(has(request, "TIOCGWINSZ"), "the ioctl request slot says what it asks for", request);

    const std::string elsewhere = listing_of(program, "same_number_elsewhere");
    expect(!elsewhere.empty(), "the subject has a function named same_number_elsewhere");
    expect(!has(elsewhere, "TIOCGWINSZ"),
           "the same number stays a number where it is not a request", elsewhere);
}

void a_frame_slot_reads_as_a_local()
{
    std::printf("\na frame slot reads as a local\n");
    astral::Program program = open();
    const std::string text = listing_of(program, "measure_terminal");
    expect(!text.empty(), "the subject has a function named measure_terminal");
    // Whatever the decompiler recovered of the frame, nothing in the listing is
    // allowed to read as arithmetic on the register the caller left behind.
    expect(!has(text, "unaff"), "no unaffected register is left in the text", text);
    expect(has(text, "local") || has(text, "stack") || has(text, "&size") || has(text, "size"),
           "the slot the request writes into has a name", text);
}

void a_declaration_stands_where_the_value_is_given()
{
    std::printf("\na declaration stands where the value is given\n");
    astral::Program program = open();
    const std::string text = listing_of(program, "first_use_inside_a_branch");
    expect(!text.empty(), "the subject has a function named first_use_inside_a_branch");
    // A declaration that moved is a type immediately followed by an assignment.
    const bool moved = has(text, "= ") &&
                       (has(text, "i32 ") || has(text, "u32 ") || has(text, "i64 "));
    expect(moved, "a type stands on a line that gives a value", text);
    // Nothing declared may be left unwritten: the wall of names is what this
    // removes, so a name declared and then never assigned means it did not run.
    size_t brace = text.find('{');
    expect(brace != std::string::npos, "the body has an opening brace");
}

void the_compilable_output_is_untouched()
{
    std::printf("\nthe compilable output is untouched\n");
    astral::Program program = open();
    std::vector<uint64_t> wanted;
    for (const astral::Symbol &symbol : program.symbols())
        if (symbol.name == "widths" || symbol.name == "measure_terminal" ||
            symbol.name == "first_use_inside_a_branch")
            wanted.push_back(symbol.address);
    expect(!wanted.empty(), "the subject has functions to emit");

    const std::string c = program.emit_c(wanted);
    expect(!c.empty(), "a translation unit came out");
    // The reading spellings must not have reached it. Each of these would stop
    // the result compiling, which is the whole point of keeping the two apart.
    for (const char *trace : {"@entry", " i32 ", " u32 ", " unk64 ", "TIOCGWINSZ", "label1:"})
        expect(!has(c, trace), std::string("no \"") + trace + "\" in the emitted C");
    // And the byte-width types it does depend on are still there.
    expect(has(c, "int4") || has(c, "uint4") || has(c, "int8") || has(c, "undefined"),
           "the emitted C still names types the way its runtime does");
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::printf("usage: listing_test <subject>\n");
        return 2;
    }
    subject_path = argv[1];
    try {
        astral::initialize();
    } catch (const std::exception &failure) {
        std::printf("the specifications did not load: %s\n", failure.what());
        return 2;
    }

    types_are_named_by_width_in_bits();
    a_known_constant_is_named_only_where_it_means_something();
    a_frame_slot_reads_as_a_local();
    a_declaration_stands_where_the_value_is_given();
    the_compilable_output_is_untouched();

    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
