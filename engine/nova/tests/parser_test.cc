// Checks on Nova's parser and formatter together.
//
// The strongest check a parser can pass is that what it reads, written back
// out and read again, comes out identical. Every case here is a piece of Nova
// at some level; the test formats it, parses the result, formats that, and
// demands the two texts match. Then it checks the things the text is supposed
// to know about itself: its level, its storage, its overlap.
#include "format.hh"
#include "lexer.hh"
#include "parser.hh"

#include <cstdio>
#include <string>
#include <vector>

using namespace astral_internal::nova;

namespace {

int passed = 0;
int failed = 0;

void report(bool ok, const std::string &name, const std::string &detail)
{
    if (ok) {
        ++passed;
        std::printf("ok   %s\n", name.c_str());
        return;
    }
    ++failed;
    std::printf("FAIL %s\n     %s\n", name.c_str(), detail.c_str());
}

std::string diagnostics_text(const std::vector<Diagnostic> &diagnostics)
{
    std::string text;
    for (const Diagnostic &d : diagnostics)
        text += "\n     " + std::to_string(d.line) + ":" + std::to_string(d.column) + " " + d.message;
    return text;
}

bool read(const std::string &source, Types &types, Unit &unit, std::vector<Diagnostic> &diagnostics)
{
    std::vector<Token> tokens = tokenise(source, diagnostics);
    return parse(tokens, types, unit, diagnostics);
}

// Parses, formats, parses the result, formats again. Returns the first
// formatting so a case can look at the canonical text too.
std::string round_trip(const std::string &name, const std::string &source, Unit *keep = nullptr,
                       Types *keep_types = nullptr)
{
    Types types;
    Unit unit;
    std::vector<Diagnostic> diagnostics;
    if (!read(source, types, unit, diagnostics)) {
        report(false, name, "first parse failed" + diagnostics_text(diagnostics));
        return "";
    }
    std::string first = format(unit, types);

    Types types2;
    Unit unit2;
    std::vector<Diagnostic> diagnostics2;
    if (!read(first, types2, unit2, diagnostics2)) {
        report(false, name, "second parse failed on:\n" + first + diagnostics_text(diagnostics2));
        return first;
    }
    std::string second = format(unit2, types2);
    report(first == second, name, "formatting is not a fixed point:\n--- first\n" + first + "--- second\n" + second);
    if (keep) {
        // Hand back the tree from the first parse; the types it points at
        // must outlive it, so hand those back too.
        *keep = std::move(unit);
        if (keep_types)
            *keep_types = std::move(types);
    }
    return first;
}

void expect_text(const std::string &name, const std::string &got, const std::string &want)
{
    report(got == want, name, "wanted:\n" + want + "got:\n" + got);
}

} // namespace

int main()
{
    // ---------------------------------------------------------- level 3
    {
        Unit unit;
        Types types;
        std::string text = round_trip("level 3 round trip",
            "func check(string: *i8): bool {\n"
            "    return (strcmp(string, \"astral\") == 0);\n"
            "}\n", &unit, &types);
        expect_text("level 3 canonical text", text,
            "func check(string: *i8): bool {\n"
            "    return (strcmp(string, \"astral\") == 0);\n"
            "}\n");
        report(unit.functions.size() == 1 && unit.functions[0].level == Level::Source,
               "level 3 is recognised", "one function at Level::Source");
    }

    // ---------------------------------------------------------- level 2
    {
        Unit unit;
        Types types;
        std::string text = round_trip("level 2 round trip",
            "func check(string: *i8 @ x0): bool @ w0 {\n"
            "    var result: i32 @ w0 = strcmp(string, \"astral\");\n"
            "    return (result == 0);\n"
            "}\n", &unit, &types);
        expect_text("level 2 canonical text", text,
            "func check(string: *i8 @ x0): bool @ w0 {\n"
            "    var result: i32 @ w0 = strcmp(string, \"astral\");\n"
            "    return (result == 0);\n"
            "}\n");
        const Function &f = unit.functions.at(0);
        report(f.level == Level::Typed, "level 2 is recognised", "Level::Typed");
        report(f.parameters.at(0).storage.kind == Storage::Kind::Register
                   && f.parameters.at(0).storage.register_name == "x0"
                   && f.result_storage.register_name == "w0",
               "storage reaches the tree", "x0 for the parameter, w0 for the result");
    }

    // ---------------------------------------------------------- level 1
    {
        Unit unit;
        Types types;
        std::string text = round_trip("level 1 round trip",
            "func check(@x0): @w0 {\n"
            "    x1 = &\"astral\";\n"
            "    call strcmp;\n"
            "    w0 = (w0 == 0);\n"
            "}\n", &unit, &types);
        expect_text("level 1 canonical text", text,
            "func check(@x0): @w0 {\n"
            "    x1 = &\"astral\";\n"
            "    strcmp();\n"
            "    w0 = (w0 == 0);\n"
            "}\n");
        report(unit.functions.at(0).level == Level::Storage, "level 1 is recognised", "Level::Storage");
    }

    // ---------------------------------------------------------- level 0
    {
        Unit unit;
        Types types;
        std::string text = round_trip("level 0 round trip",
            "func check @ 0x100000500 {\n"
            "    asm {\n"
            "        stp x29, x30, [sp, #-0x20]!\n"
            "        mov x29, sp\n"
            "    }\n"
            "}\n", &unit, &types);
        const Function &f = unit.functions.at(0);
        report(f.level == Level::Machine && f.has_address && f.address == 0x100000500,
               "level 0 is recognised with its address", "Level::Machine at 0x100000500");
        report(text.find("stp x29, x30, [sp, #-0x20]!") != std::string::npos,
               "assembly survives verbatim", text);
    }

    // ---------------------------------------------------------- main
    {
        std::string text = round_trip("main round trip",
            "var globalTerminalWidth: u32 @ 0x10000c0d0;\n"
            "val keyString: *i8 @ 0x100003f2a = \"astral\";\n"
            "\n"
            "func main(argumentCount: i32, arguments: **i8): i32 {\n"
            "    stack message: [i8; 64] @ -0x70;\n"
            "    if (argumentCount < 2) {\n"
            "        print(\"usage\\n\");\n"
            "        return 1;\n"
            "    }\n"
            "    if (!check(arguments[1])) {\n"
            "        print(\"wrong lol\\n\");\n"
            "        return 1;\n"
            "    }\n"
            "    print(\"correct\\n\");\n"
            "    return 7;\n"
            "}\n");
        report(text.find("stack message: [i8; 64] @ -0x70;") != std::string::npos,
               "a stack slot keeps its offset", text);
        report(text.find("val keyString: *i8 @ 0x100003f2a = \"astral\";") != std::string::npos,
               "a constant global keeps address and value", text);
    }

    // ---------------------------------------------------------- struct overlap
    {
        Unit unit;
        Types types;
        round_trip("overlapping struct round trip",
            "struct Header @ 0x100004000 {\n"
            "    var magic: u32 @ 0;\n"
            "    var size: u32 @ 4;\n"
            "    var flags: u16 @ 4;\n"
            "}\n"
            "func read(header: *Header): u32 {\n"
            "    return (header->size + header->flags);\n"
            "}\n", &unit, &types);
        const Record &r = unit.records.at(0);
        report(r.members.size() == 3 && r.members[1].offset == 4 && r.members[2].offset == 4,
               "members may overlap", "size and flags both at 4");
        report(unit.functions.at(0).level == Level::Source, "the reader is level 3", "");
    }

    // ---------------------------------------------------------- control flow
    {
        std::string text = round_trip("control flow round trip",
            "enum Kind { Number, Name, Plus = 10, Minus }\n"
            "func walk(items: *i32, count: i32): i32 {\n"
            "    var total: i32 = 0;\n"
            "    for (index in 0..count) {\n"
            "        total += items[index];\n"
            "    }\n"
            "    var attempts: i32 = 0;\n"
            "    loop {\n"
            "        attempts += 1;\n"
            "        if (attempts > 3) { break; }\n"
            "    }\n"
            "    while (total > 100) { total = (total >> 1); }\n"
            "    do { total -= 1; } while (total > 50);\n"
            "    match (total) {\n"
            "        0 { return -1; }\n"
            "        1 or 2 { return total; }\n"
            "        else { }\n"
            "    }\n"
            "    var kind: Kind = Kind.Minus;\n"
            "again:\n"
            "    if (total == 7) { goto again; }\n"
            "    return (total ? total : 1);\n"
            "}\n");
        report(text.find("for (index in 0..count) {") != std::string::npos, "for-in keeps its range", text);
        report(text.find("1 or 2 {") != std::string::npos, "match arms join with or", text);
        report(text.find("total >>= 1") == std::string::npos && text.find("total = total >> 1;") != std::string::npos,
               "assignment of a shift is not a truth value and gets no parentheses", text);
        report(text.find("if (attempts > 3) {") != std::string::npos, "conditions are parenthesised once", text);
    }

    // ---------------------------------------------------------- entry values
    {
        Unit unit;
        Types types;
        std::string text = round_trip("entry value round trip",
            "func save(): void {\n"
            "    globalSaved = w22@entry;\n"
            "}\n"
            "var globalSaved: u32 @ 0x10000c0d0;\n", &unit, &types);
        report(text.find("globalSaved = w22@entry;") != std::string::npos, "w22@entry is written back", text);
        report(unit.functions.at(0).level == Level::Typed,
               "reading a register makes a function level 2", "Level::Typed");
    }

    // ---------------------------------------------------------- documentation
    {
        std::string text = round_trip("statement documentation round trip",
            "func f(): i32 {\n"
            "    /* WARNING: Subroutine does not return */\n"
            "    exit(1);\n"
            "    #/ a note #\\\n"
            "    return 0;\n"
            "}\n");
        report(text.find("#/ WARNING: Subroutine does not return #\\\n    exit(1);") != std::string::npos,
               "a C block comment becomes a Nova note on the statement it precedes", text);
        report(text.find("#/ a note #\\\n    return 0;") != std::string::npos,
               "a Nova note stays on its statement", text);
    }

    // ---------------------------------------------------------- precedence
    {
        std::string text = round_trip("precedence round trip",
            "func f(a: i32, b: i32, c: i32): i32 {\n"
            "    return (a - (b - c)) * (a + b);\n"
            "}\n");
        report(text.find("return (a - (b - c)) * (a + b);") != std::string::npos,
               "parentheses that matter survive and ones that do not are dropped", text);
    }

    // ---------------------------------------------------------- casts
    {
        std::string text = round_trip("cast round trip",
            "func f(v: unknown32): u64 {\n"
            "    return (v as u64) + (-v as u32);\n"
            "}\n");
        report(text.find("unknown32") != std::string::npos, "unknown32 is spelled back", text);
    }

    // ---------------------------------------------------------- errors
    {
        Types types;
        Unit unit;
        std::vector<Diagnostic> diagnostics;
        bool ok = read("func broken(x: i32): i32 {\n    return x +;\n}\nfunc fine(): void { }\n",
                       types, unit, diagnostics);
        report(!ok && !diagnostics.empty() && diagnostics[0].line == 2,
               "a broken expression is reported on its line", diagnostics_text(diagnostics));
        report(unit.functions.size() >= 1, "parsing continues after the mistake",
               std::to_string(unit.functions.size()) + " function(s) read");
    }
    {
        Types types;
        Unit unit;
        std::vector<Diagnostic> diagnostics;
        bool ok = read("var count: nonsense = 1;\n", types, unit, diagnostics);
        report(!ok && diagnostics.size() >= 1 && diagnostics[0].message.find("nonsense") != std::string::npos,
               "an unknown type is named in the complaint", diagnostics_text(diagnostics));
    }
    {
        Types types;
        Unit unit;
        std::vector<Diagnostic> diagnostics;
        bool ok = read("func f(): void { var x: i32 @ nowhere; }\n", types, unit, diagnostics);
        report(!ok && diagnostics.size() >= 1 && diagnostics[0].message.find("register") != std::string::npos,
               "bad storage says what @ accepts", diagnostics_text(diagnostics));
    }

    std::printf("\nTotals: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
