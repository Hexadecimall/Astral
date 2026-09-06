// The decompiler's settings, checked against a real program.
//
// What matters most here is the last group: a program decompiled with nothing
// set has to come out exactly as it always did, so the whole point of the
// table is that it changes nothing until someone asks it to.
#include "astral/astral.h"
#include "astral/astral.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>
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

// Every function in the program, printed the way the command line prints it.
std::string whole_program(astral::Program &program)
{
    std::vector<uint64_t> wanted;
    for (const astral::Symbol &symbol : program.symbols())
        if (symbol.is_function)
            wanted.push_back(symbol.address);
    std::string out;
    for (uint64_t address : wanted) {
        try {
            out += program.decompile(address).c_code();
        } catch (const std::exception &) {
            out += "<refused>";
        }
    }
    return out;
}

void check_table()
{
    const int count = astral_option_count();
    expect(count > 20, "the table describes the settings",
           "only " + std::to_string(count) + " settings");

    std::set<std::string> names;
    bool everyFieldFilled = true;
    for (int index = 0; index < count; ++index) {
        const std::string name = astral_option_name(index);
        if (!names.insert(name).second)
            expect(false, "every setting has its own name", "twice: " + name);
        if (std::string(astral_option_group(index)).empty()
            || std::string(astral_option_label(index)).empty()
            || std::string(astral_option_explanation(index)).empty())
            everyFieldFilled = false;
        // A default the setting itself would refuse would make the reset
        // button write something the engine cannot take.
        if (astral_option_check(name.c_str(), astral_option_default(index)) != ASTRAL_OK)
            expect(false, "every default is a value the setting takes",
                   name + " defaults to " + astral_option_default(index));
    }
    expect(everyFieldFilled, "every setting has a group, a label and an explanation");
    expect(astral_option_index("maxlinewidth") >= 0, "maxlinewidth is offered");

    // These four are commands rather than settings: they name an action, a
    // rule or a language, and offering them as settings would let the window
    // put the decompiler into a state nothing else expects.
    for (const char *withheld : {"setlanguage", "setaction", "currentaction", "togglerule"})
        expect(astral_option_index(withheld) < 0,
               std::string("'") + withheld + "' is not offered as a setting",
               std::string(withheld) + " is in the table");
}

void check_refusals()
{
    expect(astral_option_check("maxlinewidth", "wide") != ASTRAL_OK,
           "a word is refused where a number belongs");
    const std::string message = astral_last_error();
    expect(message.find("maxlinewidth") != std::string::npos
               && message.find("number") != std::string::npos,
           "the refusal says which setting and what was expected", message);
    expect(astral_option_check("maxlinewidth", "5") != ASTRAL_OK,
           "a number below the range is refused");
    expect(astral_option_check("braceformat.function", "sideways") != ASTRAL_OK,
           "a brace style that does not exist is refused");
    expect(std::string(astral_last_error()).find("same") != std::string::npos,
           "the refusal lists the styles that do exist", astral_last_error());
    expect(astral_option_check("noSuchSetting", "on") != ASTRAL_OK,
           "a setting that does not exist is refused");
    expect(astral_option_check("maxlinewidth", "80") == ASTRAL_OK,
           "a good value is accepted");

    astral::Program program = open();
    bool threw = false;
    try {
        program.set_setting("maxlinewidth", "not a number");
    } catch (const std::exception &) {
        threw = true;
    }
    expect(threw, "the program refuses a bad value too");
    expect(program.setting("maxlinewidth") == "100",
           "a refused value is not remembered", program.setting("maxlinewidth"));

    // A value the table allows but the program cannot honour: only the engine
    // knows which prototype models a compiler specification carries.
    threw = false;
    try {
        program.set_setting("protoeval", "noSuchModel");
    } catch (const std::exception &) {
        threw = true;
    }
    expect(threw, "the engine's own refusal is reported");
    expect(program.setting("protoeval") == "default",
           "a value the engine refused is not remembered", program.setting("protoeval"));
}

void check_defaults_change_nothing()
{
    astral::Program plain = open();
    const std::string before = whole_program(plain);
    expect(!before.empty(), "the subject decompiles");

    astral::Program configured = open();
    int sent = 0;
    for (int index = 0; index < astral_option_count(); ++index) {
        // Only what the engine acts on; the rest change the emitted C or the
        // window rather than what the decompiler prints.
        if (astral_option_scope(index) != ASTRAL_OPTION_ENGINE)
            continue;
        configured.set_setting(astral_option_name(index), astral_option_default(index));
        ++sent;
    }
    expect(sent > 20, "every engine setting was sent", std::to_string(sent) + " sent");
    const std::string after = whole_program(configured);
    expect(before == after, "setting every option to its default changes nothing",
           "the output differs once the defaults are sent");
}

void check_options_reach_the_engine()
{
    astral::Program narrow = open();
    narrow.set_setting("maxlinewidth", "24");
    astral::Program wide = open();
    expect(whole_program(narrow) != whole_program(wide),
           "the line width reaches the decompiler");

    // A value the setting does not already stand at, so a difference in the
    // output is the setting arriving rather than a coincidence. The default
    // is read rather than assumed, since it is allowed to change.
    astral::Program braces = open();
    const std::string standing = braces.setting("braceformat.function");
    const char *other = standing == "next" ? "skip" : "next";
    braces.set_setting("braceformat.function", other);
    const std::string moved = whole_program(braces);
    expect(moved != whole_program(wide), "the brace style reaches the decompiler");
    // A brace of its own on a line can only be the function's: every other
    // block in this subject opens on the line that opened it.
    const std::string standard = whole_program(wide);
    expect(standard.find("\n{\n") == std::string::npos,
           "a function's brace starts out on the line its signature is");
    expect(moved.find("\n{\n") != std::string::npos,
           "and the setting moves it onto a line of its own");

    astral::Program plain = open();
    plain.set_setting("nullprinting", "on");
    // Nothing forces this subject to hold a null constant, so the check is
    // that the setting took rather than that the text changed.
    expect(plain.setting("nullprinting") == "on", "a setting reads back as it was set",
           plain.setting("nullprinting"));
    expect(plain.setting("maxlinewidth") == "100",
           "a setting never given reads back as its default", plain.setting("maxlinewidth"));
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: options_test <binary>\n");
        return 2;
    }
    subject_path = argv[1];

    astral::Library library;
    (void)library;

    std::printf("the settings table\n");
    check_table();
    std::printf("bad values\n");
    check_refusals();
    std::printf("options reaching the engine\n");
    check_options_reach_the_engine();
    std::printf("defaults\n");
    check_defaults_change_nothing();

    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
