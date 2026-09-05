// Checks on the C front end.
//
// Runs on its own and answers with a non-zero status when anything failed, so
// it works both by hand and from a test runner. Every case says what it wanted
// and what it got, because a failure here is usually about one construct in a
// large file.
#include "front.hh"
#include "lexer.hh"
#include "parser.hh"
#include "sema.hh"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace astral_internal::compiler;

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

std::string diagnostics_text(const std::vector<Diagnostic> &diagnostics, bool errors_only)
{
    std::string text;
    for (const Diagnostic &diagnostic : diagnostics) {
        bool warning = diagnostic.message.compare(0, 9, "warning: ") == 0;
        if (errors_only && warning)
            continue;
        text += std::to_string(diagnostic.line) + ":" + std::to_string(diagnostic.column) +
                ": " + diagnostic.message + "\n";
    }
    return text;
}

// Reads one source and hands back the unit, so a case can look at the tree.
struct Parsed {
    TypeStore types;
    Unit unit;
    std::vector<Diagnostic> diagnostics;
    bool ok = false;
};

void read(Parsed &parsed, const std::string &source)
{
    parsed.ok = parse_unit(source, parsed.types, parsed.unit, parsed.diagnostics);
}

void expect_clean(const std::string &name, const std::string &source)
{
    Parsed parsed;
    read(parsed, source);
    report(parsed.ok, name, "did not read cleanly:\n     " +
                                diagnostics_text(parsed.diagnostics, true));
}

const Function *find(const Unit &unit, const std::string &name)
{
    for (const Function &function : unit.functions)
        if (function.name == name && function.body)
            return &function;
    return nullptr;
}

// The first expression statement in a function's body.
const Expression *first_expression(const Function &function)
{
    if (!function.body)
        return nullptr;
    for (const StatementPtr &statement : function.body->body)
        if (statement->kind == Statement::Kind::Expression)
            return statement->value.get();
    return nullptr;
}

// ---------------------------------------------------------------- the cases

void test_declarations()
{
    expect_clean("a function definition and a prototype",
                 "int add(int a, int b);\n"
                 "int add(int a, int b) { return a + b; }\n");
    expect_clean("storage classes and qualifiers",
                 "static const int limit = 3;\n"
                 "extern volatile int elsewhere;\n"
                 "static int use(void) { return limit; }\n");
    expect_clean("a typedef", "typedef unsigned int handle;\n"
                              "handle open(void) { handle h = 0; return h; }\n");
    expect_clean("pointers, arrays and argv",
                 "int main(int argc, char **argv) { return argv[0][0] + argc; }\n");
    expect_clean("an array parameter",
                 "int main(int argc, char *argv[]) { return *argv[argc - 1]; }\n");
    expect_clean("a struct definition and a reference",
                 "struct point { int x; int y; };\n"
                 "int span(struct point *p) { return p->x + p->y; }\n");
    expect_clean("an enum",
                 "enum colour { red, green = 4, blue };\n"
                 "int pick(void) { return blue; }\n");

    // The declarator that appears in real output and is easy to get wrong.
    Parsed parsed;
    read(parsed, "uint64_t (*g100004008)();\n");
    bool found = false;
    for (const Variable &global : parsed.unit.globals)
        if (global.name == "g100004008") {
            found = global.type != nullptr && global.type->kind == Type::Kind::Pointer &&
                    global.type->target != nullptr &&
                    global.type->target->kind == Type::Kind::Function &&
                    global.type->target->result != nullptr &&
                    global.type->target->result->width == 8;
        }
    report(found, "a pointer to a function returning uint64_t",
           "the declarator did not come out as a pointer to a function");

    expect_clean("a parenthesised declarator and a called function pointer",
                 "int run(void) { int (*f)(int); f = 0; return f(1); }\n");
    expect_clean("an abstract parameter declarator", "void setlocale(unsigned long, unsigned long);\n"
                                                     "void go(void) { setlocale(2, 0); }\n");
    expect_clean("a two-dimensional array",
                 "int table[3][4];\n"
                 "int at(int i, int j) { return table[i][j]; }\n");
}

void test_statements()
{
    expect_clean("every statement shape",
                 "int walk(int n) {\n"
                 "  int total = 0;\n"
                 "  int i;\n"
                 "  if (n < 0) { return -1; } else { total = 1; }\n"
                 "  while (n > 0) { n = n - 1; if (n == 3) continue; if (n == 1) break; }\n"
                 "  do { total = total + 1; } while (total < 4);\n"
                 "  for (i = 0; i < n; i = i + 1) total += i;\n"
                 "  for (int j = 0; j < 2; ++j) total += j;\n"
                 "  switch (total) {\n"
                 "    case 0: total = 9; break;\n"
                 "    case 1 + 1: break;\n"
                 "    default: break;\n"
                 "  }\n"
                 "  goto done;\n"
                 "  ;\n"
                 "done:\n"
                 "  return total;\n"
                 "}\n");
    expect_clean("a label at the end of a block",
                 "void f(void) { goto out;\nout:\n}\n");
}

void test_precedence()
{
    Parsed parsed;
    read(parsed,
         "int a, b, c, d, x, y, p_holder;\n"
         "int *p;\n"
         "void one(void) { a = b + c * d; }\n"
         "void two(void) { *p++; }\n"
         "void three(void) { a ? b : c; }\n"
         "void four(void) { x << 1 | y & 3; }\n"
         "void five(void) { a = b = c; }\n"
         "void six(void) { a - b - c; }\n");

    const Function *one = find(parsed.unit, "one");
    const Expression *e = one != nullptr ? first_expression(*one) : nullptr;
    bool ok = e != nullptr && e->kind == Expression::Kind::Assign &&
              e->right->kind == Expression::Kind::Binary &&
              e->right->binary_op == BinaryOp::Add &&
              e->right->right->kind == Expression::Kind::Binary &&
              e->right->right->binary_op == BinaryOp::Multiply;
    report(ok, "a = b + c * d groups as a = (b + (c * d))", "the tree is a different shape");

    const Function *two = find(parsed.unit, "two");
    e = two != nullptr ? first_expression(*two) : nullptr;
    ok = e != nullptr && e->kind == Expression::Kind::Unary &&
         e->unary_op == UnaryOp::Dereference && e->left->kind == Expression::Kind::Unary &&
         e->left->unary_op == UnaryOp::PostIncrement;
    report(ok, "*p++ groups as *(p++)", "the tree is a different shape");

    const Function *three = find(parsed.unit, "three");
    e = three != nullptr ? first_expression(*three) : nullptr;
    ok = e != nullptr && e->kind == Expression::Kind::Conditional &&
         e->left->kind == Expression::Kind::Name && e->right->kind == Expression::Kind::Name &&
         e->third->kind == Expression::Kind::Name;
    report(ok, "a ? b : c is one conditional", "the tree is a different shape");

    const Function *four = find(parsed.unit, "four");
    e = four != nullptr ? first_expression(*four) : nullptr;
    ok = e != nullptr && e->kind == Expression::Kind::Binary && e->binary_op == BinaryOp::BitOr &&
         e->left->kind == Expression::Kind::Binary &&
         e->left->binary_op == BinaryOp::ShiftLeft &&
         e->right->kind == Expression::Kind::Binary && e->right->binary_op == BinaryOp::BitAnd;
    report(ok, "x << 1 | y & 3 groups as (x << 1) | (y & 3)", "the tree is a different shape");

    const Function *five = find(parsed.unit, "five");
    e = five != nullptr ? first_expression(*five) : nullptr;
    ok = e != nullptr && e->kind == Expression::Kind::Assign &&
         e->right->kind == Expression::Kind::Assign;
    report(ok, "a = b = c groups to the right", "the tree is a different shape");

    const Function *six = find(parsed.unit, "six");
    e = six != nullptr ? first_expression(*six) : nullptr;
    ok = e != nullptr && e->kind == Expression::Kind::Binary &&
         e->binary_op == BinaryOp::Subtract && e->left->kind == Expression::Kind::Binary &&
         e->left->binary_op == BinaryOp::Subtract;
    report(ok, "a - b - c groups to the left", "the tree is a different shape");

    expect_clean("sizeof of a type and of an expression",
                 "unsigned long f(void) { int v; return sizeof(int) + sizeof v; }\n");
    expect_clean("a cast", "void *f(long a) { return (void *)a; }\n");
    expect_clean("the comma operator", "int f(void) { int a = 1, b = 2; return (a = 3, b); }\n");
    expect_clean("every compound assignment",
                 "int f(int a) { a += 1; a -= 1; a *= 2; a /= 2; a %= 2; a <<= 1; a >>= 1;\n"
                 "  a &= 1; a |= 1; a ^= 1; return a; }\n");
    expect_clean("prefix and postfix stepping",
                 "int f(int a) { ++a; --a; a++; a--; return +a + -a + !a + ~a; }\n");
}

void test_builtin_typedefs()
{
    struct Case {
        const char *name;
        int width;
        bool is_signed;
    };
    static const Case CASES[] = {
        {"int1", 1, true},      {"int2", 2, true},      {"int4", 4, true},
        {"int8", 8, true},      {"uint1", 1, false},    {"uint2", 2, false},
        {"uint4", 4, false},    {"uint8", 8, false},    {"byte", 1, false},
        {"word", 2, false},     {"dword", 4, false},    {"qword", 8, false},
        {"undefined", 1, false},{"undefined1", 1, false},{"undefined2", 2, false},
        {"undefined4", 4, false},{"undefined8", 8, false},
        {"xunknown1", 1, false},{"xunknown2", 2, false},{"xunknown4", 4, false},
        {"xunknown8", 8, false},
        {"int8_t", 1, true},    {"int16_t", 2, true},   {"int32_t", 4, true},
        {"int64_t", 8, true},   {"uint8_t", 1, false},  {"uint16_t", 2, false},
        {"uint32_t", 4, false}, {"uint64_t", 8, false},
    };
    TypeStore types;
    bool all = true;
    std::string detail;
    for (const Case &one : CASES) {
        TypePtr type = types.named(one.name);
        if (type == nullptr || type->kind != Type::Kind::Integer || type->width != one.width ||
            type->is_signed != one.is_signed) {
            all = false;
            detail += std::string(one.name) + " is not what it should be; ";
        }
    }
    report(all, "the decompiler's typedefs are built in", detail);

    TypeStore more;
    report(more.named("code") != nullptr &&
               more.named("code")->kind == Type::Kind::Void,
           "code is void", "code did not resolve to void");
    report(more.named("float4") != nullptr && more.named("float4")->width == 4 &&
               more.named("float8") != nullptr && more.named("float8")->width == 8,
           "float4 and float8 are the right widths", "one of them is not");

    // The same width reached through two names is one type.
    report(more.named("uint4") == more.named("uint32_t"),
           "uint4 and uint32_t are the same type", "they came out as two types");

    expect_clean("a whole function written in the decompiler's spellings",
                 "undefined8 f(uint4 a, byte b) {\n"
                 "  int8 total = 0;\n"
                 "  code *target;\n"
                 "  total = a + b;\n"
                 "  target = 0;\n"
                 "  return total;\n"
                 "}\n");
}

void test_pointer_arithmetic()
{
    Parsed parsed;
    read(parsed, "int *step(int *p) { return p + 1; }\n"
                 "long gap(int *a, int *b) { return a - b; }\n"
                 "char *bump(char *s) { return s + 1; }\n");
    report(parsed.ok, "pointer arithmetic reads cleanly",
           diagnostics_text(parsed.diagnostics, true));

    const Function *step = find(parsed.unit, "step");
    const Expression *value = nullptr;
    if (step != nullptr && step->body)
        for (const StatementPtr &statement : step->body->body)
            if (statement->kind == Statement::Kind::Return)
                value = statement->value.get();
    bool ok = value != nullptr && value->type != nullptr &&
              value->type->kind == Type::Kind::Pointer && value->type->target != nullptr &&
              value->type->target->kind == Type::Kind::Integer &&
              value->type->target->width == 4;
    report(ok, "p + 1 where p is int * has type int *", "the result type is not int *");
    report(ok && TypeStore::size_of(value->type->target) == 4,
           "the step of int * is four bytes", "the pointee size is wrong");

    const Function *gap = find(parsed.unit, "gap");
    value = nullptr;
    if (gap != nullptr && gap->body)
        for (const StatementPtr &statement : gap->body->body)
            if (statement->kind == Statement::Kind::Return)
                value = statement->value.get();
    ok = value != nullptr && value->type != nullptr && value->type->is_integer() &&
         value->type->width == 8;
    report(ok, "one pointer minus another counts elements", "the difference is not an integer");
}

void test_sizes_and_layout()
{
    TypeStore types;
    TypePtr byte = types.integer(1, false);
    TypePtr word = types.integer(2, false);
    TypePtr dword = types.integer(4, false);
    TypePtr qword = types.integer(8, false);

    report(TypeStore::size_of(qword) == 8 && TypeStore::size_of(types.pointer_to(byte)) == 8,
           "a long and a pointer are eight bytes", "one of them is not");
    report(TypeStore::size_of(types.array_of(dword, 10)) == 40,
           "an array of ten ints is forty bytes", "the array size is wrong");
    report(TypeStore::align_of(types.array_of(dword, 10)) == 4,
           "an array is aligned like its element", "the array alignment is wrong");

    std::vector<Type::Member> members;
    Type::Member first;
    first.name = "a";
    first.type = byte;
    members.push_back(first);
    Type::Member second;
    second.name = "b";
    second.type = dword;
    members.push_back(second);
    Type::Member third;
    third.name = "c";
    third.type = word;
    members.push_back(third);
    TypePtr structure = types.structure("mixed", members);
    bool ok = structure->members[0].offset == 0 && structure->members[1].offset == 4 &&
              structure->members[2].offset == 8 && TypeStore::size_of(structure) == 12 &&
              TypeStore::align_of(structure) == 4;
    report(ok, "a struct pads its members and rounds up its size",
           "the layout is " + std::to_string(structure->members[1].offset) + "/" +
               std::to_string(structure->members[2].offset) + " size " +
               std::to_string(TypeStore::size_of(structure)));

    (void)qword;
}

void test_preprocessor_lines()
{
    expect_clean("preprocessor lines are dropped",
                 "#include <stdio.h>\n"
                 "#if defined(__clang__)\n"
                 "#  pragma clang diagnostic ignored \"-Wint-conversion\"\n"
                 "#elif defined(__GNUC__)\n"
                 "#  pragma GCC diagnostic ignored \"-Wint-conversion\"\n"
                 "#endif\n"
                 "int f(void) { return 1; }\n");
    expect_clean("a preprocessor line that continues onto the next one",
                 "#define astralStore(a, b) \\\n"
                 "    do { } while (0)\n"
                 "int f(void) { return 2; }\n");
    expect_clean("the macros the runtime header leaves behind",
                 "astralInline long long SoftwareBreakpoint() { return 0; }\n"
                 "astralNoreturn astralInline void haltBaddata(void) { for (;;) { } }\n");
    expect_clean("an asm name and an attribute on a declaration",
                 "extern unsigned char sym[] __asm__(\"_real_name\");\n"
                 "__attribute__((noreturn)) void die(void);\n"
                 "void use(void) { die(); }\n");
}

void test_comments_and_literals()
{
    expect_clean("comments in every position",
                 "/* a leading comment */\n"
                 "int f(void) { // a line comment\n"
                 "  int a = /* inline */ 1;\n"
                 "  return a; /* trailing */\n"
                 "}\n");

    Parsed parsed;
    read(parsed,
         "int f(void) { return 10 + 0x1f + 017 + 'A' + '\\n' + '\\x41' + 1u + 2L + 3ULL; }\n");
    report(parsed.ok, "every integer and character literal spelling",
           diagnostics_text(parsed.diagnostics, true));

    std::vector<Diagnostic> diagnostics;
    std::vector<Token> tokens = tokenise("1 0x10 010 'a' '\\n' \"a\\tb\" \"c\" 1.5 1e3 2.0f",
                                         diagnostics);
    bool ok = tokens.size() >= 10 && tokens[0].integer_value == 1 &&
              tokens[1].integer_value == 16 && tokens[2].integer_value == 8 &&
              tokens[3].integer_value == 'a' && tokens[4].integer_value == '\n' &&
              tokens[5].bytes == "a\tb" && tokens[6].bytes == "c" &&
              tokens[7].float_value == 1.5 && tokens[8].float_value == 1000.0 &&
              tokens[9].float_value == 2.0;
    report(ok, "literal values come out right", "one of the literals read wrong");

    Parsed joined;
    read(joined, "char *f(void) { return \"one\" \"two\"; }\n");
    const Function *function = find(joined.unit, "f");
    const Expression *value = nullptr;
    if (function != nullptr && function->body)
        for (const StatementPtr &statement : function->body->body)
            if (statement->kind == Statement::Kind::Return)
                value = statement->value.get();
    report(value != nullptr && value->kind == Expression::Kind::StringLiteral &&
               value->text == "onetwo",
           "string literals written next to each other join", "they did not join");
}

void test_externals()
{
    Parsed parsed;
    read(parsed, "extern int elsewhere;\n"
                 "void other(int);\n"
                 "void here(void) { other(elsewhere); undeclared(1); }\n");
    bool has_variable = false;
    bool has_function = false;
    bool has_implicit = false;
    for (const External &external : parsed.unit.externals) {
        if (external.name == "elsewhere" && !external.is_function)
            has_variable = true;
        if (external.name == "other" && external.is_function)
            has_function = true;
        if (external.name == "undeclared" && external.is_function)
            has_implicit = true;
    }
    report(has_variable && has_function && has_implicit,
           "everything referenced and not defined is written down",
           "one of the three is missing from the externals");
    report(parsed.ok, "an implicit call is not an error",
           diagnostics_text(parsed.diagnostics, true));
}

void test_permissive_conversions()
{
    Parsed parsed;
    read(parsed, "long f(char *s) { long a = (long)s; char *b = a; return b - s; }\n"
                 "void g(void) { int *p = 0; p = 1; }\n");
    report(parsed.ok, "mixing integers and pointers is never an error",
           diagnostics_text(parsed.diagnostics, true));
    bool warned = false;
    for (const Diagnostic &diagnostic : parsed.diagnostics)
        if (diagnostic.message.compare(0, 9, "warning: ") == 0)
            warned = true;
    report(warned, "mixing integers and pointers does warn", "nothing was said about it");
}

void test_call_checking()
{
    Parsed parsed;
    read(parsed, "int two(int a, int b);\n"
                 "int f(void) { return two(1); }\n");
    // Neither too few nor too many arguments is a refusal: a recovered
    // prototype and a recovered call site are separate guesses, and the
    // emitter's own output has them disagree.
    bool warned = false;
    for (const Diagnostic &diagnostic : parsed.diagnostics)
        if (diagnostic.message.find("takes 2") != std::string::npos)
            warned = true;
    report(parsed.ok && warned, "too few arguments is reported but not refused",
           diagnostics_text(parsed.diagnostics, true));

    // Too many is only a warning: the emitter writes such calls whenever it
    // recovered more about a call site than about the function it reaches.
    Parsed extra;
    read(extra, "int one(int a);\n"
                "void f(void) { one(1, 2, 3); }\n");
    report(extra.ok, "too many arguments is not an error",
           diagnostics_text(extra.diagnostics, true));

    Parsed variadic;
    read(variadic, "int printf(const char *format, ...);\n"
                   "void f(void) { printf(\"%d %d\\n\", 1, 2); }\n");
    report(variadic.ok, "a variadic tail is allowed",
           diagnostics_text(variadic.diagnostics, true));

    Parsed unspecified;
    read(unspecified, "long unknown();\n"
                      "void f(void) { unknown(1, 2, 3); }\n");
    report(unspecified.ok, "an unspecified parameter list accepts anything",
           diagnostics_text(unspecified.diagnostics, true));
}

void test_malformed()
{
    struct Case {
        const char *name;
        const char *source;
    };
    static const Case CASES[] = {
        {"a missing semicolon", "int f(void) { int a = 1 return a; }\n"},
        {"an unclosed brace", "int f(void) { if (1) { return 0;\n"},
        {"an unclosed comment", "int f(void) { return 0; } /* never closed\n"},
        {"an unclosed string", "char *f(void) { return \"open;\n}\n"},
        {"a stray character", "int f(void) { return 1 @ 2; }\n"},
        {"nothing but punctuation", ") } ; ] ,\n"},
        {"an expression that stops early", "int f(void) { return 1 + ; }\n"},
        {"a goto to nowhere", "void f(void) { goto missing; }\n"},
        {"a call on something that cannot be called", "int f(void) { double a = 1; return a(); }\n"},
        {"a member of something with no members", "int f(void) { int a = 1; return a.x; }\n"},
    };
    for (const Case &one : CASES) {
        Parsed parsed;
        read(parsed, one.source);
        bool complained = false;
        bool located = false;
        for (const Diagnostic &diagnostic : parsed.diagnostics)
            if (diagnostic.message.compare(0, 9, "warning: ") != 0) {
                complained = true;
                if (diagnostic.line > 0 && diagnostic.column > 0)
                    located = true;
            }
        report(!parsed.ok && complained && located,
               std::string("malformed input is refused with a position: ") + one.name,
               "it was accepted or the complaint had no position");
    }
}

// Every prefix of a real-looking source, so a file cut off anywhere still
// stops rather than spinning or walking off the end.
void test_truncation()
{
    static const char SOURCE[] =
        "#include <stdint.h>\n"
        "typedef struct node { int value; struct node *next; } node;\n"
        "uint64_t (*hook)();\n"
        "static const char text[] = \"abc\";\n"
        "int walk(node *head, int n) {\n"
        "  int total = 0;\n"
        "  for (node *at = head; at; at = at->next) {\n"
        "    switch (at->value) {\n"
        "      case 1: total += n << 2 | 3; break;\n"
        "      default: goto done;\n"
        "    }\n"
        "  }\n"
        "done:\n"
        "  return total ? total : (int)(long)hook;\n"
        "}\n";
    std::string whole(SOURCE);
    bool survived = true;
    for (size_t length = 0; length <= whole.size(); ++length) {
        Parsed parsed;
        read(parsed, whole.substr(0, length));
        if (length == whole.size() && !parsed.ok)
            survived = false;
    }
    report(survived, "every prefix of a source is read without hanging or crashing",
           "the whole source did not read cleanly");
}

void test_comparison()
{
    const char *original = "int f(int count) {\n"
                           "  int total = 0;\n"
                           "  total = count + 1;\n"
                           "  puts(\"VK7JG\");\n"
                           "  return total;\n"
                           "}\n";
    const char *renamed = "int f(int n) {\n"
                          "  int sum = 0;\n"
                          "  /* a comment that changes nothing */\n"
                          "  sum   =   n + 1;\n"
                          "  puts(\"VK7JG\");\n"
                          "  return sum;\n"
                          "}\n";
    const char *literal_changed = "int f(int count) {\n"
                                  "  int total = 0;\n"
                                  "  total = count + 2;\n"
                                  "  puts(\"bleep\");\n"
                                  "  return total;\n"
                                  "}\n";
    const char *operator_changed = "int f(int count) {\n"
                                   "  int total = 0;\n"
                                   "  total = count - 1;\n"
                                   "  puts(\"VK7JG\");\n"
                                   "  return total;\n"
                                   "}\n";
    const char *reordered = "int f(int count) {\n"
                            "  int total = 0;\n"
                            "  puts(\"VK7JG\");\n"
                            "  total = count + 1;\n"
                            "  return total;\n"
                            "}\n";
    const char *other_callee = "int f(int count) {\n"
                               "  int total = 0;\n"
                               "  total = count + 1;\n"
                               "  fputs(\"VK7JG\");\n"
                               "  return total;\n"
                               "}\n";
    const char *other_spelling = "uint32_t f(uint4 count) {\n"
                                 "  int32_t total = 0;\n"
                                 "  total = count + 1;\n"
                                 "  puts(\"VK7JG\");\n"
                                 "  return total;\n"
                                 "}\n";
    const char *same_spelling = "uint4 f(uint32_t count) {\n"
                                "  int4 total = 0;\n"
                                "  total = count + 1;\n"
                                "  puts(\"VK7JG\");\n"
                                "  return total;\n"
                                "}\n";

    Parsed a;
    read(a, original);
    Parsed b;
    read(b, renamed);
    Parsed c;
    read(c, literal_changed);
    Parsed d;
    read(d, operator_changed);
    Parsed e;
    read(e, reordered);
    Parsed g;
    read(g, other_callee);
    Parsed h;
    read(h, other_spelling);
    Parsed i;
    read(i, same_spelling);

    const Function *fa = find(a.unit, "f");
    const Function *fb = find(b.unit, "f");
    const Function *fc = find(c.unit, "f");
    const Function *fd = find(d.unit, "f");
    const Function *fe = find(e.unit, "f");
    const Function *fg = find(g.unit, "f");
    const Function *fh = find(h.unit, "f");
    const Function *fi = find(i.unit, "f");
    if (fa == nullptr || fb == nullptr || fc == nullptr || fd == nullptr || fe == nullptr ||
        fg == nullptr || fh == nullptr || fi == nullptr) {
        report(false, "the comparison cases parse", "one of them did not produce a function");
        return;
    }

    report(same_meaning(*fa, *fb), "renaming locals and parameters compares equal",
           "a rename was seen as a change");
    report(meaning_digest(*fa) == meaning_digest(*fb),
           "a rename gives the same digest", "the digests differ");
    report(!same_meaning(*fa, *fc), "a changed literal compares unequal", "they compared equal");
    report(!same_meaning(*fa, *fd), "a changed operator compares unequal", "they compared equal");
    report(!same_meaning(*fa, *fe), "reordered statements compare unequal",
           "they compared equal");
    report(!same_meaning(*fa, *fg), "a different called function compares unequal",
           "they compared equal");
    report(same_meaning(*fh, *fi), "two spellings of the same types compare equal",
           "the spelling was seen as a change");

    std::vector<LiteralChange> changes;
    bool only = only_literals_differ(*fa, *fc, changes);
    bool found_number = false;
    bool found_text = false;
    for (const LiteralChange &change : changes) {
        if (change.before == "1" && change.after == "2" && !change.is_text)
            found_number = true;
        if (change.before == "VK7JG" && change.after == "bleep" && change.is_text)
            found_text = true;
    }
    report(only && changes.size() == 2 && found_number && found_text,
           "a literal-only change is reported with both values",
           "got " + std::to_string(changes.size()) + " changes");
    bool located = true;
    for (const LiteralChange &change : changes)
        if (change.where.line <= 0 || change.where.column <= 0)
            located = false;
    report(located && !changes.empty(), "each literal change says where it is",
           "a change had no position");

    changes.clear();
    report(!only_literals_differ(*fa, *fd, changes),
           "a changed operator is not a literal-only change", "it was reported as one");
    changes.clear();
    report(!only_literals_differ(*fa, *fe, changes),
           "reordered statements are not a literal-only change", "it was reported as one");
    changes.clear();
    report(only_literals_differ(*fa, *fb, changes) && changes.empty(),
           "a rename is a literal-only change with nothing in it",
           "it reported a change");
}

// The point of all of it: a whole file the decompiler wrote reads cleanly.
void test_real_output(const std::vector<std::string> &files)
{
    for (const std::string &path : files) {
        std::ifstream stream(path.c_str());
        if (!stream) {
            report(false, "reading " + path, "the file could not be opened");
            continue;
        }
        std::ostringstream text;
        text << stream.rdbuf();

        Parsed parsed;
        read(parsed, text.str());
        std::string errors = diagnostics_text(parsed.diagnostics, true);
        size_t shown = errors.size() > 600 ? 600 : errors.size();
        report(parsed.ok, "a real decompiled file reads cleanly: " + path,
               errors.substr(0, shown));
    }
}

} // namespace

int main(int argc, char **argv)
{
    test_declarations();
    test_statements();
    test_precedence();
    test_builtin_typedefs();
    test_pointer_arithmetic();
    test_sizes_and_layout();
    test_preprocessor_lines();
    test_comments_and_literals();
    test_externals();
    test_permissive_conversions();
    test_call_checking();
    test_malformed();
    test_truncation();
    test_comparison();

    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i)
        files.push_back(argv[i]);
    if (!files.empty())
        test_real_output(files);

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
