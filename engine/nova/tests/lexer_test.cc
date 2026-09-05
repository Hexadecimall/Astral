// Checks on Nova's tokeniser.
//
// Runs on its own and answers with a non-zero status when anything failed, so
// it works both by hand and from a test runner. The cases are the ones the
// emitter and a person editing recovered code actually write: hexadecimal
// addresses, register names after `@`, documentation blocks, and the Fusion
// number literals Nova inherits.
#include "lexer.hh"

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

// The tokens as one line, so a case can say what it expects without building a
// vector by hand. Punctuation and names appear as themselves; a number appears
// as its value; a string appears in quotes.
std::string shape(const std::string &source, std::vector<Diagnostic> &diagnostics)
{
    std::vector<Token> tokens = tokenise(source, diagnostics);
    std::string text;
    for (const Token &token : tokens) {
        if (token.kind == Token::Kind::End)
            break;
        if (!text.empty())
            text += " ";
        switch (token.kind) {
        case Token::Kind::Name:
        case Token::Kind::Punctuation:
            text += token.text;
            break;
        case Token::Kind::Integer:
        case Token::Kind::Character:
            text += std::to_string(token.integer_value);
            break;
        case Token::Kind::Float:
            text += "float";
            break;
        case Token::Kind::String:
            text += "\"" + token.bytes + "\"";
            break;
        case Token::Kind::Documentation:
            text += "doc(" + token.text + ")";
            break;
        case Token::Kind::Assembly:
            text += "asm(" + token.text + ")";
            break;
        case Token::Kind::End:
            break;
        }
    }
    return text;
}

void check(const std::string &name, const std::string &source, const std::string &want)
{
    std::vector<Diagnostic> diagnostics;
    std::string got = shape(source, diagnostics);
    std::string detail = "wanted [" + want + "]\n     got    [" + got + "]";
    for (const Diagnostic &diagnostic : diagnostics)
        detail += "\n     " + std::to_string(diagnostic.line) + ": " + diagnostic.message;
    report(got == want && diagnostics.empty(), name, detail);
}

} // namespace

int main()
{
    check("a function at the source level",
          "func check(string: *i8): bool {\n"
          "    return (strcmp(string, \"astral\") == 0);\n"
          "}\n",
          "func check ( string : * i8 ) : bool { return ( strcmp ( string , \"astral\" ) == 0 ) ; }");

    check("storage annotations",
          "func check(string: *i8 @ x0): bool @ w0 { }",
          "func check ( string : * i8 @ x0 ) : bool @ w0 { }");

    check("an address and a frame offset",
          "var terminalWidth: u32 @ 0x10000c0d0;\nstack message: [i8; 64] @ -0x70;",
          "var terminalWidth : u32 @ 4295016656 ; "
          "stack message : [ i8 ; 64 ] @ - 112 ;");

    check("Fusion number literals",
          "0xFF 0b1010 0o755 1_000_000 'A'",
          "255 10 493 1000000 65");

    check("comments are dropped and documentation is kept",
          "# this is gone\n#/ this stays #\\\nvar count = 1;",
          "doc( this stays ) var count = 1 ;");

    check("the pointer arrow survives",
          "header->magic = 1;",
          "header -> magic = 1 ;");

    check("ranges do not become two dots",
          "for (index in 0..10) { }",
          "for ( index in 0 .. 10 ) { }");

    check("an inclusive range",
          "for (index in 0..=10) { }",
          "for ( index in 0 ..= 10 ) { }");

    check("an asm body is handed over whole, comment character and all",
          "asm { stp x29, x30, [sp, #-0x20]! }",
          "asm asm( stp x29, x30, [sp, #-0x20]! )");

    check("C comments are read: // is dropped and /* */ is kept",
          "var a = 1; // gone\n/* WARNING: kept */ var b = 2;",
          "var a = 1 ; doc( WARNING: kept ) var b = 2 ;");

    check("escapes in a string",
          "\"one\\ttwo\\n\"",
          "\"one\ttwo\n\"");

    report(is_keyword("func") && is_keyword("stack") && !is_keyword("check"),
           "keywords are recognised", "func, stack are keywords and check is not");

    report(is_register_name("x0") && is_register_name("w22") && is_register_name("d3")
               && is_register_name("sp") && !is_register_name("string")
               && !is_register_name("check"),
           "register names are recognised by shape",
           "x0, w22, d3 and sp are registers; string and check are not");

    std::printf("\nTotals: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
