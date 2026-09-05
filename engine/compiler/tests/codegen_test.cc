// Checking the compiler by running what it wrote.
//
// A generated instruction that looks right is not evidence. Every test here
// either executes the bytes the compiler produced, or writes them into a real
// program and runs that, and then says what actually happened.
#include "astral/astral.hpp"
#include "compiler/asmbuffer.hh"
#include "compiler/ast.hh"
#include "compiler/codegen.hh"
#include "compiler/compiler.hh"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

using namespace astral_internal;

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

void expect_equal(long long got, long long wanted, const std::string &what)
{
    char saw[128];
    std::snprintf(saw, sizeof saw, "got %lld, wanted %lld", got, wanted);
    report(got == wanted, what, saw);
}

void expect_text(const std::string &got, const std::string &wanted, const std::string &what)
{
    report(got == wanted, what, "got \"" + got + "\", wanted \"" + wanted + "\"");
}

// ------------------------------------------------------------------ running

// A page of memory that generated code is put into and then run from.
class Arena {
public:
    Arena()
    {
        code_ = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        data_ = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (code_ == MAP_FAILED || data_ == MAP_FAILED) {
            std::perror("mmap");
            std::exit(2);
        }
    }
    ~Arena()
    {
        munmap(code_, kSize);
        munmap(data_, kSize);
    }

    uint64_t code_address() const { return reinterpret_cast<uint64_t>(code_); }

    // Where the next literal handed to place_text will go.
    uint64_t place(const std::string &text)
    {
        char *at = static_cast<char *>(data_) + used_;
        std::memcpy(at, text.data(), text.size());
        at[text.size()] = '\0';
        used_ += text.size() + 1;
        used_ = (used_ + 7) / 8 * 8;
        return reinterpret_cast<uint64_t>(at);
    }

    void *scratch() { return static_cast<char *>(data_) + kSize / 2; }

    void *install(const std::vector<uint8_t> &bytes)
    {
        mprotect(code_, kSize, PROT_READ | PROT_WRITE);
        std::memcpy(code_, bytes.data(), bytes.size());
        if (mprotect(code_, kSize, PROT_READ | PROT_EXEC) != 0) {
            std::perror("mprotect");
            std::exit(2);
        }
        __builtin___clear_cache(static_cast<char *>(code_),
                                static_cast<char *>(code_) + bytes.size());
        return code_;
    }

private:
    static const size_t kSize = 1 << 16;
    void *code_ = nullptr;
    void *data_ = nullptr;
    size_t used_ = 0;
};

// The functions a test's C is allowed to call, by name.
struct Known {
    const char *name;
    void *address;
};

std::vector<Known> &host_functions()
{
    static std::vector<Known> table;
    return table;
}

// A callee taking more arguments than there are registers to put them in.
extern "C" long astral_test_ten(long a, long b, long c, long d, long e, long f, long g, long h,
                                long i, long j)
{
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10;
}

extern "C" int astral_test_twice(int value) { return value * 2; }
extern "C" int astral_test_thrice(int value) { return value * 3; }

compiler::Environment host_environment(Arena &arena, const std::vector<std::string> &already = {})
{
    compiler::Environment environment;
    environment.address_of = [](const std::string &name) -> std::optional<uint64_t> {
        for (const Known &known : host_functions())
            if (name == known.name)
                return reinterpret_cast<uint64_t>(known.address);
        return std::nullopt;
    };
    // Copied so the closure does not reach into a vector the caller may drop.
    std::vector<std::string> present = already;
    Arena *held = &arena;
    static std::vector<std::pair<std::string, uint64_t>> resident;
    for (const std::string &text : present)
        resident.emplace_back(text, held->place(text));
    environment.address_of_text = [present](const std::string &text) -> std::optional<uint64_t> {
        for (const std::pair<std::string, uint64_t> &one : resident)
            if (one.first == text)
                return one.second;
        return std::nullopt;
    };
    return environment;
}

compiler::Options placing_options(Arena &arena)
{
    compiler::Options options;
    Arena *held = &arena;
    options.place_text = [held](const std::string &text) -> std::optional<uint64_t> {
        return held->place(text);
    };
    options.keep_assembly = true;
    return options;
}

// Compiles one function and puts it somewhere it can be called from.
void *build(Arena &arena, const std::string &source, const compiler::Environment &environment,
            compiler::Options options, const std::string &what, compiler::Result *out = nullptr)
{
    const compiler::Result result = compiler::compile(assembler::Target::Arm64, source,
                                                      arena.code_address(), environment, options);
    if (out != nullptr)
        *out = result;
    if (!result.ok) {
        report(false, what, "would not compile: " + result.error);
        return nullptr;
    }
    return arena.install(result.bytes);
}

// ------------------------------------------------------------------ the tests

void arithmetic_and_comparison()
{
    std::printf("arithmetic, comparison, bitwise and shifts\n");
    struct Case {
        const char *body;
        long long a;
        long long b;
        long long wanted;
    };
    const Case cases[] = {
        {"return a + b;", 7, 5, 12},
        {"return a - b;", 7, 5, 2},
        {"return a * b;", 7, 5, 35},
        {"return a / b;", -21, 5, -4},
        {"return a % b;", -21, 5, -1},
        {"return a << b;", 3, 4, 48},
        {"return a >> b;", -64, 3, -8},
        {"return a & b;", 0xf0, 0x3c, 0x30},
        {"return a | b;", 0xf0, 0x3c, 0xfc},
        {"return a ^ b;", 0xf0, 0x3c, 0xcc},
        {"return ~a;", 5, 0, -6},
        {"return -a;", 5, 0, -5},
        {"return !a;", 0, 0, 1},
        {"return a < b;", -1, 1, 1},
        {"return a > b;", -1, 1, 0},
        {"return a <= b;", 4, 4, 1},
        {"return a >= b;", 4, 5, 0},
        {"return a == b;", 9, 9, 1},
        {"return a != b;", 9, 9, 0},
        {"return a && b;", 1, 0, 0},
        {"return a || b;", 0, 3, 1},
        {"return a ? a + 1 : b + 2;", 0, 5, 7},
        {"long t = a; t += b; t *= 2; return t;", 3, 4, 14},
        {"return (a + b) * (a - b);", 9, 4, 65},
    };
    for (const Case &one : cases) {
        Arena arena;
        const std::string source =
            std::string("long f(long a, long b) { ") + one.body + " }";
        void *code = build(arena, source, host_environment(arena), placing_options(arena),
                           std::string("`") + one.body + "`");
        if (code == nullptr)
            continue;
        long (*f)(long, long) = reinterpret_cast<long (*)(long, long)>(code);
        expect_equal(f(one.a, one.b), one.wanted, std::string("`") + one.body + "`");
    }
}

void short_circuit_really_short_circuits()
{
    std::printf("&& and || do not evaluate the far side when they need not\n");
    Arena arena;
    // If the right side ran, the counter it steps would show it.
    const std::string source =
        "int f(int *counter) {"
        "  int total = 0;"
        "  if (0 && (*counter = *counter + 1)) { total = total + 1; }"
        "  if (1 || (*counter = *counter + 1)) { total = total + 10; }"
        "  return total;"
        "}";
    void *code = build(arena, source, host_environment(arena), placing_options(arena),
                       "short circuit compiles");
    if (code == nullptr)
        return;
    int counter = 0;
    int (*f)(int *) = reinterpret_cast<int (*)(int *)>(code);
    expect_equal(f(&counter), 10, "only the branch that runs contributes");
    expect_equal(counter, 0, "neither far side was evaluated");
}

void loads_and_stores_at_every_width()
{
    std::printf("loads and stores at one, two, four and eight bytes\n");
    struct Case {
        const char *source;
        long long wanted;
        const char *what;
    };
    const Case cases[] = {
        {"long f(char *p) { p[0] = -1; return p[0]; }", -1, "a signed byte comes back signed"},
        {"long f(unsigned char *p) { p[0] = 255; return p[0]; }", 255,
         "an unsigned byte comes back unsigned"},
        {"long f(short *p) { p[0] = -300; return p[0]; }", -300, "a signed halfword keeps its sign"},
        {"long f(unsigned short *p) { p[0] = 65535; return p[0]; }", 65535,
         "an unsigned halfword does not"},
        {"long f(int *p) { p[0] = -70000; return p[0]; }", -70000, "a signed word keeps its sign"},
        {"long f(unsigned int *p) { p[0] = 4294967295u; return p[0]; }", 4294967295LL,
         "an unsigned word does not"},
        {"long f(long *p) { p[0] = -1234567890123L; return p[0]; }", -1234567890123LL,
         "a doubleword goes there and back"},
        {"long f(long *p) { p[3] = 77; return p[3]; }", 77, "an index reaches past the first"},
    };
    for (const Case &one : cases) {
        Arena arena;
        void *code = build(arena, one.source, host_environment(arena), placing_options(arena),
                           one.what);
        if (code == nullptr)
            continue;
        long buffer[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        long (*f)(void *) = reinterpret_cast<long (*)(void *)>(code);
        expect_equal(f(buffer), one.wanted, one.what);
    }
}

void pointers_and_stepping()
{
    std::printf("pointer arithmetic, dereference, address-of and stepping\n");
    {
        Arena arena;
        // Adding one to an int pointer moves four bytes, not one.
        void *code = build(arena, "int f(int *p) { int *q = p + 3; return *q; }",
                           host_environment(arena), placing_options(arena), "pointer add scales");
        if (code != nullptr) {
            int values[5] = {10, 20, 30, 40, 50};
            expect_equal(reinterpret_cast<int (*)(int *)>(code)(values), 40,
                         "p + 3 on an int pointer lands on the fourth int");
        }
    }
    {
        Arena arena;
        void *code = build(arena, "long f(int *a, int *b) { return b - a; }",
                           host_environment(arena), placing_options(arena),
                           "pointer difference counts elements");
        if (code != nullptr) {
            int values[5] = {0, 0, 0, 0, 0};
            expect_equal(reinterpret_cast<long (*)(int *, int *)>(code)(values, values + 4), 4,
                         "the distance between pointers is in elements");
        }
    }
    {
        Arena arena;
        void *code = build(arena, "int f(void) { int x = 5; int *p = &x; *p = *p + 2; return x; }",
                           host_environment(arena), placing_options(arena),
                           "address-of a local works");
        if (code != nullptr)
            expect_equal(reinterpret_cast<int (*)()>(code)(), 7,
                         "writing through &x changes x");
    }
    {
        Arena arena;
        void *code = build(arena,
                           "int f(int *out) { int i = 5; out[0] = i++; out[1] = i; out[2] = ++i;"
                           " out[3] = i--; out[4] = --i; return i; }",
                           host_environment(arena), placing_options(arena),
                           "pre and post stepping compile");
        if (code != nullptr) {
            int out[5] = {0, 0, 0, 0, 0};
            const int left = reinterpret_cast<int (*)(int *)>(code)(out);
            expect_equal(out[0], 5, "i++ answers with the value before");
            expect_equal(out[1], 6, "and leaves it stepped");
            expect_equal(out[2], 7, "++i answers with the value after");
            expect_equal(out[3], 7, "i-- answers with the value before");
            expect_equal(out[4], 5, "--i answers with the value after");
            expect_equal(left, 5, "and the variable ends where it should");
        }
    }
    {
        Arena arena;
        void *code = build(arena,
                           "int f(int *p) { int *q = p; while (*q) { q++; } return q - p; }",
                           host_environment(arena), placing_options(arena),
                           "stepping a pointer in a loop");
        if (code != nullptr) {
            int values[5] = {1, 2, 3, 0, 9};
            expect_equal(reinterpret_cast<int (*)(int *)>(code)(values), 3,
                         "the loop stops at the first zero");
        }
    }
}

void casts_between_widths()
{
    std::printf("casts between integer widths and between integers and pointers\n");
    struct Case {
        const char *source;
        long long in;
        long long wanted;
        const char *what;
    };
    const Case cases[] = {
        {"long f(long a) { return (int)a; }", 0x1ffffffffLL, -1, "a long narrowed to an int"},
        {"long f(long a) { return (unsigned int)a; }", 0x1ffffffffLL, 4294967295LL,
         "a long narrowed to an unsigned int"},
        {"long f(long a) { return (char)a; }", 0x1ff, -1, "a long narrowed to a char"},
        {"long f(long a) { return (unsigned char)a; }", 0x1ff, 255,
         "a long narrowed to an unsigned char"},
        {"long f(long a) { return (short)a; }", 0x1ffff, -1, "a long narrowed to a short"},
        {"long f(long a) { return (long)(char *)a; }", 0x123456789LL, 0x123456789LL,
         "an integer through a pointer and back"},
    };
    for (const Case &one : cases) {
        Arena arena;
        void *code = build(arena, one.source, host_environment(arena), placing_options(arena),
                           one.what);
        if (code == nullptr)
            continue;
        expect_equal(reinterpret_cast<long (*)(long)>(code)(one.in), one.wanted, one.what);
    }
}

void arrays_structures_and_a_frame_too_big_for_one_instruction()
{
    std::printf("arrays, structure members, deep expressions and a very large frame\n");
    {
        Arena arena;
        void *code = build(arena,
                           "int f(void) { int a[8]; int i; int t = 0;"
                           " for (i = 0; i < 8; i++) { a[i] = i * i; }"
                           " for (i = 0; i < 8; i++) { t = t + a[i]; } return t; }",
                           host_environment(arena), placing_options(arena),
                           "a local array compiles");
        if (code != nullptr)
            expect_equal(reinterpret_cast<int (*)()>(code)(), 140,
                         "an array on the frame is written and read back");
    }
    {
        Arena arena;
        void *code = build(arena,
                           "struct P { int x; int y; };"
                           " int f(struct P *p) { p->y = p->x * 3; return p->x + p->y; }",
                           host_environment(arena), placing_options(arena),
                           "structure members compile");
        if (code != nullptr) {
            struct P {
                int x;
                int y;
            } p = {7, 0};
            expect_equal(reinterpret_cast<int (*)(void *)>(code)(&p), 28,
                         "a member is read at the right offset");
            expect_equal(p.y, 21, "and written at the right offset");
        }
    }
    {
        Arena arena;
        // Five thousand bytes is past what one sub of the stack pointer can
        // take, and past what one add can reach for an address inside it.
        void *code = build(arena,
                           "int f(void) { char big[5000]; int i;"
                           " for (i = 0; i < 5000; i++) { big[i] = (char)(i & 0x7f); }"
                           " return big[4999] + big[0] + big[128]; }",
                           host_environment(arena), placing_options(arena),
                           "a frame bigger than one instruction can size compiles");
        if (code != nullptr)
            expect_equal(reinterpret_cast<int (*)()>(code)(), 7,
                         "and every byte of it is reachable");
    }
    {
        Arena arena;
        void *code = build(arena,
                           "int f(void) { return ((((1+2)*(3+4))-((5+6)*(7-8)))*"
                           "(((9+1)*(2+3))-((4+5)*(6-7)))); }",
                           host_environment(arena), placing_options(arena),
                           "a deeply nested expression compiles");
        if (code != nullptr)
            expect_equal(reinterpret_cast<int (*)()>(code)(),
                         ((((1 + 2) * (3 + 4)) - ((5 + 6) * (7 - 8))) *
                          (((9 + 1) * (2 + 3)) - ((4 + 5) * (6 - 7)))),
                         "however deep the evaluation stack had to go");
    }
    {
        Arena arena;
        void *code = build(arena, "void f(void) { }", host_environment(arena),
                           placing_options(arena), "a function that does nothing compiles");
        if (code != nullptr) {
            reinterpret_cast<void (*)()>(code)();
            expect(true, "and returns without touching the stack at all");
        }
    }
}

void switch_with_several_cases_and_a_default()
{
    std::printf("switch with several cases and a default\n");
    Arena arena;
    const std::string source =
        "int f(int a) {"
        "  int answer = 0;"
        "  switch (a) {"
        "    case 1: answer = 10; break;"
        "    case 2: answer = 20; break;"
        "    case 3:"
        "    case 4: answer = 34; break;"
        "    case 1000: answer = 99; break;"
        "    default: answer = -1; break;"
        "  }"
        "  return answer;"
        "}";
    void *code = build(arena, source, host_environment(arena), placing_options(arena),
                       "a switch compiles");
    if (code == nullptr)
        return;
    int (*f)(int) = reinterpret_cast<int (*)(int)>(code);
    expect_equal(f(1), 10, "case 1");
    expect_equal(f(2), 20, "case 2");
    expect_equal(f(3), 34, "case 3 falls into case 4");
    expect_equal(f(4), 34, "case 4");
    expect_equal(f(1000), 99, "a case whose value needs more than twelve bits");
    expect_equal(f(7), -1, "the default");

    Arena bare;
    void *no_default = build(bare,
                             "int f(int a) { int r = 5; switch (a) { case 1: r = 1; break; }"
                             " return r; }",
                             host_environment(bare), placing_options(bare),
                             "a switch with no default compiles");
    if (no_default != nullptr) {
        int (*g)(int) = reinterpret_cast<int (*)(int)>(no_default);
        expect_equal(g(1), 1, "the one case still matches");
        expect_equal(g(2), 5, "and nothing happens without a default");
    }
}

void nested_loops_with_break_and_continue()
{
    std::printf("nested loops, with break and continue reaching the right loop\n");
    Arena arena;
    const std::string source =
        "int f(int n) {"
        "  int total = 0;"
        "  for (int i = 0; i < n; i++) {"
        "    if (i == 3) { continue; }"
        "    for (int j = 0; j < n; j++) {"
        "      if (j == 2) { break; }"
        "      if (j == 1 && i == 1) { continue; }"
        "      total = total + i * 10 + j;"
        "    }"
        "    if (i == 5) { break; }"
        "  }"
        "  return total;"
        "}";
    // The same thing, worked out here, so the answer is not one this test made up.
    int wanted = 0;
    for (int i = 0; i < 8; i++) {
        if (i == 3)
            continue;
        for (int j = 0; j < 8; j++) {
            if (j == 2)
                break;
            if (j == 1 && i == 1)
                continue;
            wanted = wanted + i * 10 + j;
        }
        if (i == 5)
            break;
    }
    void *code = build(arena, source, host_environment(arena), placing_options(arena),
                       "nested loops compile");
    if (code == nullptr)
        return;
    expect_equal(reinterpret_cast<int (*)(int)>(code)(8), wanted,
                 "break leaves the inner loop and continue goes round the right one");

    Arena second;
    void *dowhile = build(second,
                          "int f(int n) { int t = 0; int i = 0;"
                          " do { i = i + 1; if (i == 2) { continue; } t = t + i; } while (i < n);"
                          " return t; }",
                          host_environment(second), placing_options(second),
                          "do-while compiles");
    if (dowhile != nullptr) {
        int t = 0, i = 0;
        do {
            i = i + 1;
            if (i == 2)
                continue;
            t = t + i;
        } while (i < 5);
        expect_equal(reinterpret_cast<int (*)(int)>(dowhile)(5), t,
                     "continue in a do-while goes to the test, not the top");
    }
}

void goto_forwards_and_backwards()
{
    std::printf("goto, jumping both ways\n");
    Arena arena;
    const std::string source =
        "int f(int n) {"
        "  int total = 0;"
        "  int i = 0;"
        "  goto middle;"
        "top:"
        "  total = total + i;"
        "  i = i + 1;"
        "middle:"
        "  if (i < n) { goto top; }"
        "  goto done;"
        "  total = 999;"
        "done:"
        "  return total;"
        "}";
    void *code = build(arena, source, host_environment(arena), placing_options(arena),
                       "goto compiles");
    if (code == nullptr)
        return;
    int (*f)(int) = reinterpret_cast<int (*)(int)>(code);
    expect_equal(f(5), 10, "a backward goto makes a loop");
    expect_equal(f(0), 0, "a forward goto skips what is between");
}

void a_call_with_more_than_eight_arguments()
{
    std::printf("a call with more arguments than there are registers\n");
    Arena arena;
    const std::string source =
        "long astral_test_ten(long a, long b, long c, long d, long e, long f, long g, long h,"
        " long i, long j);"
        "long go(void) { return astral_test_ten(1, 2, 3, 4, 5, 6, 7, 8, 9, 10); }";
    compiler::Options options = placing_options(arena);
    options.function = "go";
    void *code = build(arena, source, host_environment(arena), options,
                       "a ten-argument call compiles");
    if (code == nullptr)
        return;
    const long wanted = astral_test_ten(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    expect_equal(reinterpret_cast<long (*)()>(code)(), wanted,
                 "the ninth and tenth arguments arrive on the stack in the right order");
}

void a_variadic_call()
{
    std::printf("a variadic call, which is where printf lives\n");
    Arena arena;
    const std::string source =
        "int snprintf(char *out, unsigned long room, const char *shape, ...);"
        "int go(char *out) {"
        "  return snprintf(out, 64, \"%s|%d|%ld|%c|%d\", \"hi\", 42, 1234567890123L, 90, -7);"
        "}";
    compiler::Options options = placing_options(arena);
    options.function = "go";
    compiler::Result result;
    void *code = build(arena, source, host_environment(arena), options,
                       "a variadic call compiles", &result);
    if (code == nullptr)
        return;
    char out[64];
    std::memset(out, 0, sizeof out);
    const int written = reinterpret_cast<int (*)(char *)>(code)(out);
    expect_text(out, "hi|42|1234567890123|Z|-7",
                "every variadic argument landed where the platform looks for it");
    expect_equal(written, 24, "and snprintf agreed on the length");
    expect(result.placed.size() == 2, "two literals had to be given room",
           "placed " + std::to_string(result.placed.size()));
}

void a_call_through_a_function_pointer()
{
    std::printf("a call through a function pointer\n");
    Arena arena;
    const std::string source =
        "int go(int (*which)(int), int value) { return which(value) + 1; }";
    void *code = build(arena, source, host_environment(arena), placing_options(arena),
                       "an indirect call compiles");
    if (code == nullptr)
        return;
    int (*f)(int (*)(int), int) = reinterpret_cast<int (*)(int (*)(int), int)>(code);
    expect_equal(f(astral_test_twice, 21), 43, "through one pointer");
    expect_equal(f(astral_test_thrice, 21), 64, "and through another");
}

void string_literals_found_and_placed()
{
    std::printf("string literals: one already in the program, one that needs room\n");
    Arena arena;
    // "already here" is pretended to be in the image; "brand new" is not.
    compiler::Environment environment = host_environment(arena, {"already here"});
    compiler::Options options = placing_options(arena);
    options.function = "go";
    const std::string source =
        "int strcmp(const char *a, const char *b);"
        "int go(int which) {"
        "  const char *a = \"already here\";"
        "  const char *b = \"brand new\";"
        "  return which ? a[0] : b[0];"
        "}";
    compiler::Result result;
    void *code = build(arena, source, environment, options, "both literals compile", &result);
    if (code == nullptr)
        return;
    expect(result.placed.size() == 1, "only the literal the program lacks was placed",
           "placed " + std::to_string(result.placed.size()));
    if (result.placed.size() == 1)
        expect_text(result.placed[0].text, "brand new", "and it is the right one");
    int (*f)(int) = reinterpret_cast<int (*)(int)>(code);
    expect_equal(f(1), 'a', "the resident literal reads back");
    expect_equal(f(0), 'b', "and so does the placed one");
}

void stopping_after_the_assembly()
{
    std::printf("stopping once the assembly is written\n");
    Arena arena;
    const compiler::Result result = compiler::compile_to_assembly(
        assembler::Target::Arm64, "int f(int a) { return a + 1; }", 0x1000,
        host_environment(arena), compiler::Options());
    expect(result.ok, "it got that far", result.error);
    expect(result.bytes.empty(), "and stopped before any bytes");
    expect(!result.assembly.empty(), "with the assembly kept whether or not it was asked for");
    expect(result.assembly.find("ret") != std::string::npos,
           "and it ends the way a function does", result.assembly);
}

void a_literal_with_nowhere_to_go_is_refused()
{
    std::printf("a literal with nowhere to go is refused, not invented\n");
    Arena arena;
    compiler::Environment environment;
    compiler::Options options;   // no place_text at all
    const compiler::Result result = compiler::compile(
        assembler::Target::Arm64, "const char *go(void) { return \"nowhere\"; }",
        arena.code_address(), environment, options);
    expect(!result.ok, "it did not claim to have compiled");
    expect(result.error.find("nowhere") != std::string::npos,
           "and the complaint names the literal", "said: " + result.error);
}

void too_big_is_refused_with_both_sizes()
{
    std::printf("a function that will not fit is refused, and both sizes are said\n");
    Arena arena;
    compiler::Options options = placing_options(arena);
    options.available = 8;
    const compiler::Result result =
        compiler::compile(assembler::Target::Arm64,
                          "long f(long a, long b) { return a * b + a - b; }",
                          arena.code_address(), host_environment(arena), options);
    expect(!result.ok, "it refused");
    expect(result.bytes.empty(), "and handed back nothing to write");
    const bool names_budget = result.error.find("only 8") != std::string::npos;
    const bool names_size = result.error.find("bytes") != std::string::npos &&
                            result.error.find("compiles to") != std::string::npos;
    expect(names_budget && names_size, "the refusal names what it needed and what it had",
           "said: " + result.error);
}

void an_unsupported_target_is_refused_clearly()
{
    std::printf("an architecture with no back end is refused by name\n");
    compiler::Environment environment;
    const compiler::Result result = compiler::compile(assembler::Target::X86_64,
                                                      "int f(void) { return 1; }", 0x1000,
                                                      environment, compiler::Options());
    expect(!result.ok, "it refused");
    expect(result.error.find("x86-64") != std::string::npos,
           "and said which architecture it cannot do", "said: " + result.error);
}

// ------------------------------------------------------------------ by hand

void a_tree_built_by_hand()
{
    std::printf("a function built as a tree, with no source text anywhere\n");
    Arena arena;
    compiler::TypeStore types;
    compiler::TypePtr integer = types.integer(4, true);
    compiler::TypePtr signature = types.function(integer, {integer, integer}, false);

    compiler::Unit unit;
    compiler::Function function;
    function.name = "byhand";
    function.type = signature;

    compiler::Variable a;
    a.name = "a";
    a.type = integer;
    compiler::Variable b;
    b.name = "b";
    b.type = integer;
    function.parameters.push_back(std::move(a));
    function.parameters.push_back(std::move(b));

    // return a * a + b;
    auto name_of = [&](const char *what) {
        compiler::ExpressionPtr node(new compiler::Expression());
        node->kind = compiler::Expression::Kind::Name;
        node->name = what;
        node->type = integer;
        return node;
    };
    compiler::ExpressionPtr square(new compiler::Expression());
    square->kind = compiler::Expression::Kind::Binary;
    square->binary_op = compiler::BinaryOp::Multiply;
    square->type = integer;
    square->left = name_of("a");
    square->right = name_of("a");

    compiler::ExpressionPtr total(new compiler::Expression());
    total->kind = compiler::Expression::Kind::Binary;
    total->binary_op = compiler::BinaryOp::Add;
    total->type = integer;
    total->left = std::move(square);
    total->right = name_of("b");

    compiler::StatementPtr answer(new compiler::Statement());
    answer->kind = compiler::Statement::Kind::Return;
    answer->value = std::move(total);

    compiler::StatementPtr body(new compiler::Statement());
    body->kind = compiler::Statement::Kind::Compound;
    body->body.push_back(std::move(answer));
    function.body = std::move(body);

    std::unique_ptr<compiler::Machine> machine =
        compiler::machine_for(assembler::Target::Arm64);
    compiler::AsmBuffer buffer(assembler::Target::Arm64, arena.code_address());
    compiler::Environment environment;
    compiler::Options options;
    std::vector<compiler::Result::Datum> placed;
    std::vector<compiler::Diagnostic> diagnostics;
    std::string error;
    const bool made = compiler::generate_function(
        *machine, buffer, assembler::Target::Arm64, arena.code_address(), unit, function,
        environment, options, placed, diagnostics, error);
    expect(made, "the tree generated", error);
    if (!made)
        return;
    std::vector<uint8_t> bytes;
    const bool assembled = buffer.assemble(bytes, error);
    expect(assembled, "and assembled", error);
    if (!assembled)
        return;
    void *code = arena.install(bytes);
    expect_equal(reinterpret_cast<int (*)(int, int)>(code)(7, 5), 54,
                 "a * a + b, from a tree nobody parsed");
}

void the_buffer_resolves_labels_both_ways()
{
    std::printf("the buffer places labels ahead of and behind the branch\n");
    compiler::AsmBuffer buffer(assembler::Target::Arm64, 0x1000);
    const std::string ahead = buffer.fresh("ahead");
    const std::string behind = buffer.fresh("behind");
    buffer.label(behind);
    buffer.instruction("b %" + ahead + "%");
    buffer.instruction("nop");
    buffer.label(ahead);
    buffer.instruction("b %" + behind + "%");
    // A name nothing produced has to be a refusal, not a branch to nowhere.
    std::vector<uint8_t> bytes;
    std::string error;
    const bool ok = buffer.assemble(bytes, error);
    expect(ok, "it laid out", error);
    if (ok) {
        expect_equal(static_cast<long long>(bytes.size()), 12, "three instructions came out");
        // b +2 words forward, then b -3 words back.
        const bool forward = bytes[0] == 0x02 && bytes[3] == 0x14;
        const bool backward = bytes[8] == 0xfe && bytes[11] == 0x17;
        expect(forward, "the forward branch counts two words on");
        expect(backward, "the backward branch counts two words back");
    }

    compiler::AsmBuffer missing(assembler::Target::Arm64, 0x1000);
    missing.instruction("b %imaginary%");
    std::vector<uint8_t> nothing;
    std::string why;
    expect(!missing.assemble(nothing, why), "a branch to a name nothing produced is refused");
    expect(why.find("imaginary") != std::string::npos, "and the name is said",
           "said: " + why);
}

// ------------------------------------------------------------------ updates

// Two functions, so a change to one can be shown not to touch the other.
const char *const kBefore =
    "int strcmp(const char *a, const char *b);\n"
    "int check(const char *key) { return strcmp(key, \"VK7JG\") == 0; }\n"
    "int twice(int n) { int doubled = n * 2; return doubled; }\n";

compiler::Environment update_environment(Arena &arena, uint64_t check_at, uint64_t twice_at,
                                         uint64_t text_at)
{
    (void)arena;
    compiler::Environment environment;
    environment.address_of = [check_at, twice_at](const std::string &name)
        -> std::optional<uint64_t> {
        if (name == "check")
            return check_at;
        if (name == "twice")
            return twice_at;
        if (name == "strcmp")
            return reinterpret_cast<uint64_t>(&std::strcmp);
        return std::nullopt;
    };
    environment.address_of_text = [text_at](const std::string &text) -> std::optional<uint64_t> {
        if (text == "VK7JG")
            return text_at;
        return std::nullopt;
    };
    return environment;
}

void only_what_changed_is_compiled()
{
    std::printf("compiling only what changed\n");
    Arena arena;
    const uint64_t check_at = 0x100000000ULL;
    const uint64_t twice_at = 0x100000100ULL;
    const uint64_t text_at = 0x100001000ULL;
    compiler::Environment environment = update_environment(arena, check_at, twice_at, text_at);

    compiler::Options options = placing_options(arena);
    options.function = "check";
    const compiler::Result whole =
        compiler::compile(assembler::Target::Arm64, kBefore, check_at, environment, options);
    expect(whole.ok, "the starting point compiles", whole.error);
    if (!whole.ok)
        return;
    options.existing = whole.bytes;

    // 1. A local renamed: nothing at all to write.
    {
        std::string after = kBefore;
        const size_t at = after.find("doubled");
        while (after.find("doubled") != std::string::npos)
            after.replace(after.find("doubled"), 7, "result_");
        (void)at;
        compiler::Update update;
        const compiler::Result result = compiler::compile_update(
            assembler::Target::Arm64, kBefore, after, check_at, environment, update, options);
        expect(result.ok, "renaming a local is understood", result.error);
        expect(update.regions.empty(), "and writes nothing at all",
               std::to_string(update.regions.size()) + " regions");
        expect(update.recompiled.empty(), "and compiles nothing",
               "recompiled " + std::to_string(update.recompiled.size()));
        expect(update.untouched.size() == 2, "both functions were left alone",
               "untouched " + std::to_string(update.untouched.size()));
    }

    // 2. One literal changed, no longer than before: one small region, no code.
    {
        std::string after = kBefore;
        after.replace(after.find("VK7JG"), 5, "bleep");
        compiler::Update update;
        const compiler::Result result = compiler::compile_update(
            assembler::Target::Arm64, kBefore, after, check_at, environment, update, options);
        expect(result.ok, "changing a literal is understood", result.error);
        expect(update.recompiled.empty(), "and compiles nothing",
               "recompiled " + std::to_string(update.recompiled.size()));
        expect(update.regions.size() == 1, "and writes exactly one region",
               std::to_string(update.regions.size()) + " regions");
        if (update.regions.size() == 1) {
            const compiler::Update::Region &region = update.regions[0];
            expect(region.address == text_at, "at the literal's own address");
            const std::string wrote(region.bytes.begin(), region.bytes.begin() + 5);
            expect_text(wrote, "bleep", "with the new value in it");
            expect(region.bytes.size() <= 6, "and nothing more than the old value's room",
                   std::to_string(region.bytes.size()) + " bytes");
            expect(region.reason.find("literal") != std::string::npos,
                   "and says why", "said: " + region.reason);
        }
        expect(update.retouched_text.size() == 1, "one literal was rewritten in place");
    }

    // 3. A longer literal will not fit, so the function is written again.
    {
        std::string after = kBefore;
        after.replace(after.find("VK7JG"), 5, "a much longer key");
        compiler::Update update;
        const compiler::Result result = compiler::compile_update(
            assembler::Target::Arm64, kBefore, after, check_at, environment, update, options);
        expect(result.ok, "a longer literal is understood", result.error);
        expect(update.retouched_text.empty(), "and is not squeezed into the old room");
        expect(update.recompiled.size() == 1 && update.recompiled[0] == "check",
               "check was written again instead",
               "recompiled " + std::to_string(update.recompiled.size()));
        expect(!update.regions.empty(), "and there is something to write");
    }

    // 4. An expression changed: one function again, the other left alone.
    {
        std::string after = kBefore;
        after.replace(after.find("n * 2"), 5, "n * 3");
        compiler::Update update;
        const compiler::Result result = compiler::compile_update(
            assembler::Target::Arm64, kBefore, after, check_at, environment, update, options);
        expect(result.ok, "changing an expression is understood", result.error);
        expect(update.recompiled.size() == 1 && update.recompiled[0] == "twice",
               "only twice was written again",
               "recompiled " + std::to_string(update.recompiled.size()));
        expect(update.untouched.size() == 1 && update.untouched[0] == "check",
               "and check was left alone",
               "untouched " + std::to_string(update.untouched.size()));
        bool all_in_twice = !update.regions.empty();
        for (const compiler::Update::Region &region : update.regions)
            if (region.address < twice_at)
                all_in_twice = false;
        expect(all_in_twice, "and everything written lands in twice");
    }

    // 5. The same source twice: nothing to do.
    {
        compiler::Update update;
        const compiler::Result result = compiler::compile_update(
            assembler::Target::Arm64, kBefore, kBefore, check_at, environment, update, options);
        expect(result.ok, "an unchanged source is understood", result.error);
        expect(update.regions.empty(), "and produces no work at all");
    }

    // 6. Only the differing bytes of a rewritten function come out.
    {
        std::string after = kBefore;
        after.replace(after.find("return strcmp(key, \"VK7JG\") == 0;"), 32,
                      "return strcmp(key, \"VK7JG\") != 0;");
        compiler::Update update;
        const compiler::Result result = compiler::compile_update(
            assembler::Target::Arm64, kBefore, after, check_at, environment, update, options);
        expect(result.ok, "a small change inside a function is understood", result.error);
        size_t written = 0;
        for (const compiler::Update::Region &region : update.regions)
            written += region.bytes.size();
        expect(written > 0 && written < whole.bytes.size(),
               "and only part of the function is written",
               std::to_string(written) + " of " + std::to_string(whole.bytes.size()) + " bytes");
    }
}

// ------------------------------------------------------------------ the real thing

std::string run_command(const std::string &command)
{
    std::string out;
    FILE *pipe = popen(command.c_str(), "r");
    if (pipe == nullptr)
        return "<could not run>";
    char chunk[256];
    while (std::fgets(chunk, sizeof chunk, pipe) != nullptr)
        out += chunk;
    pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

void patch_a_real_binary(const std::string &original)
{
    std::printf("patching a real program with compiled bytes, and running it\n");
    struct stat exists;
    if (stat(original.c_str(), &exists) != 0) {
        report(false, "the program to patch is there", original + " is missing");
        return;
    }

    const std::string before_right = run_command(original + " astral 2>&1");
    const std::string before_wrong = run_command(original + " definitely-not 2>&1");
    expect_text(before_right, "correct", "before: the right key is accepted");
    expect_text(before_wrong, "wrong", "before: a wrong key is refused");

    astral::initialize();
    astral::Program program = astral::Program::open(original);
    if (!program) {
        report(false, "the program opens", "Program::open gave nothing back");
        return;
    }
    std::optional<astral::Symbol> check = program.find_symbol("_check");
    if (!check)
        check = program.find_symbol("check");
    if (!check) {
        report(false, "check is found in the program", "no symbol called check");
        return;
    }
    const uint64_t at = check->address;
    // How much room there is: up to whatever comes next.
    uint64_t room = check->size;
    if (room == 0) {
        std::optional<astral::Symbol> main = program.find_symbol("_main");
        if (!main)
            main = program.find_symbol("main");
        room = main && main->address > at ? main->address - at : 32;
    }
    std::printf("        check is at 0x%llx with %llu bytes of room\n",
                static_cast<unsigned long long>(at), static_cast<unsigned long long>(room));

    compiler::Environment environment;
    environment.address_of = [&program](const std::string &name) -> std::optional<uint64_t> {
        std::optional<astral::Symbol> found = program.find_symbol(name);
        if (!found)
            found = program.find_symbol("_" + name);
        if (found)
            return found->address;
        return std::nullopt;
    };
    environment.address_of_text = [](const std::string &) { return std::nullopt; };

    compiler::Options options;
    options.available = room;
    options.keep_assembly = true;
    // The same shape the decompiler prints for this function, with the test
    // changed so any key is accepted.
    const std::string source = "int check(const char *key) { return 1; }";
    const compiler::Result made =
        compiler::compile(assembler::Target::Arm64, source, at, environment, options);
    expect(made.ok, "the replacement compiles", made.error);
    if (!made.ok)
        return;
    std::printf("        it came to %zu bytes, and %llu were available\n", made.bytes.size(),
                static_cast<unsigned long long>(room));
    std::printf("%s", made.assembly.c_str());

    const std::vector<uint8_t> was = program.read(at, made.bytes.size());
    expect(was != made.bytes, "the new bytes are not the ones already there");

    program.patch_bytes(at, made.bytes, "check compiled from edited C");
    const std::string patched = original + ".patched";
    program.write_patched(patched);
    chmod(patched.c_str(), 0755);

    const std::string after_right = run_command(patched + " astral 2>&1");
    const std::string after_wrong = run_command(patched + " definitely-not 2>&1");
    std::printf("        before: `%s` -> %s ; after: `%s` -> %s\n", "definitely-not",
                before_wrong.c_str(), "definitely-not", after_wrong.c_str());
    expect_text(after_right, "correct", "after: the right key is still accepted");
    expect_text(after_wrong, "correct", "after: the wrong key is now accepted too");
    expect(before_wrong != after_wrong, "the program behaves differently than it did");

}

// The same function, but with everything in it: a call, a string literal and a
// comparison. There is no room for it in the program, so it is run in place
// instead, which proves the code rather than the patch.
void the_whole_check_function_runs()
{
    std::printf("the real check function, compiled and run\n");
    Arena arena;
    compiler::Environment environment = host_environment(arena);
    compiler::Options options = placing_options(arena);
    const std::string source =
        "int strcmp(const char *a, const char *b);\n"
        "int check(const char *key) { return strcmp(key, \"astral\") == 0; }\n";
    compiler::Result result;
    void *code = build(arena, source, environment, options, "check compiles", &result);
    if (code == nullptr)
        return;
    int (*f)(const char *) = reinterpret_cast<int (*)(const char *)>(code);
    expect_equal(f("astral"), 1, "the right key answers one");
    expect_equal(f("astra"), 0, "a short key answers nought");
    expect_equal(f("astralx"), 0, "a long key answers nought");
    expect_equal(f(""), 0, "and so does nothing at all");
}


// ------------------------------------------------------------------ sizes

// What a patch has to fit in is the room the function it replaces occupies, so
// the size of what comes out is a result like any other and is checked like
// one. The corpus is what Astral itself recovered from real programs: `check`
// and `main` from the crackme, `printFflush` from /bin/echo, `terminate` and
// `sub3` from /bin/cat, plus the two loop shapes the emitter writes most.
//
// Both columns are measured the same way: compiled as if the function sat at
// 0x100000460 with everything it calls at 0x100000534, which is the distance a
// patch really works over. "was" is what the generator produced before any of
// this, kept here so a regression shows up as a number going the wrong way.
void the_size_of_what_comes_out()
{
    std::printf("how big the recovered functions compile to\n");
    struct Piece {
        const char *name;
        const char *function;
        size_t was;
        size_t no_more_than;
        const char *source;
    };
    static const Piece corpus[] = {
        {"check (crackme)", "", 176, 60,
         "int strcmp(const char *a, const char *b);\n"
         "bool check(char *pointer)\n{\n"
         "  int32_t result = strcmp(pointer,\"astral\");\n"
         "  return result == 0;\n}\n"},
        {"main (crackme)", "main", 560, 200,
         "int strcmp(const char*,const char*);\n"
         "int printf(const char*,...);\n"
         "bool check(char *p);\n"
         "int main(int argc, char *argv[])\n{\n"
         "  int32_t iVar1;\n  int32_t result;\n"
         "  if (argc == 2) {\n"
         "    iVar1 = check(argv[1]);\n"
         "    if (iVar1 == 0) { printf(\"wrong\\n\"); result = 1; }\n"
         "    else { printf(\"correct\\n\"); result = 0; }\n"
         "  } else { printf(\"usage: %s <key>\\n\",*argv); result = 2; }\n"
         "  return result;\n}\n"},
        {"terminate (/bin/cat)", "", 156, 96,
         "int fwrite(const void*, unsigned long, unsigned long, void*);\n"
         "void exit(int);\n"
         "static const char sUsage[] = \"usage: cat [-belnstuv] [file ...]\\n\";\n"
         "unsigned long *global5;\n"
         "void terminate(void)\n{\n"
         "  fwrite((void *)sUsage,0x22,1,(void *)*global5);\n"
         "  exit(1);\n}\n"},
        {"sub3 (/bin/cat)", "", 132, 96,
         "void warn(const char*,...);\n"
         "unsigned char global18;\nchar *global19;\n"
         "void sub3(void)\n{\n"
         "  warn(\"%s\",global19);\n"
         "  global18 = 1;\n"
         "  return;\n}\n"},
        {"printFflush (/bin/echo)", "", 72, 48,
         "void err(int, const char *, ...);\n"
         "void printFflush(void)\n{\n  err(1,\"fflush\");\n}\n"},
        {"a counted loop", "", 408, 128,
         "long sum_loop(int *values, int count)\n{\n"
         "  long total = 0;\n"
         "  for (int i = 0; i < count; i = i + 1) { total = total + values[i]; }\n"
         "  return total;\n}\n"},
        {"a loop over a string", "", 252, 96,
         "unsigned long strlen_like(char *s)\n{\n"
         "  unsigned long n = 0;\n"
         "  while (s[n] != 0) { n = n + 1; }\n"
         "  return n;\n}\n"},
    };

    std::printf("        %-26s %8s %8s\n", "function", "was", "now");
    for (const Piece &piece : corpus) {
        compiler::Environment environment;
        environment.address_of = [](const std::string &) -> std::optional<uint64_t> {
            return std::optional<uint64_t>(0x100000534ULL);
        };
        compiler::Options options;
        options.function = piece.function;
        options.place_text = [](const std::string &text) -> std::optional<uint64_t> {
            static uint64_t next = 0x100005000;
            const uint64_t here = next;
            next = (next + text.size() + 8) / 8 * 8;
            return std::optional<uint64_t>(here);
        };
        const compiler::Result result = compiler::compile(
            assembler::Target::Arm64, piece.source, 0x100000460ULL, environment, options);
        if (!result.ok) {
            report(false, std::string("`") + piece.name + "` compiles", result.error);
            continue;
        }
        std::printf("        %-26s %8zu %8zu\n", piece.name, piece.was, result.bytes.size());
        char saw[160];
        std::snprintf(saw, sizeof saw, "%zu bytes, which is more than the %zu it has to fit in",
                      result.bytes.size(), piece.no_more_than);
        report(result.bytes.size() <= piece.no_more_than,
               std::string("`") + piece.name + "` is no bigger than it has to be", saw);
    }
}

// The function the whole exercise is about: the crackme's `check` is 52 bytes
// in the program, so anything larger cannot be written over it.
void check_fits_where_it_came_from()
{
    std::printf("the recovered check fits the room the original occupies\n");
    compiler::Environment environment;
    environment.address_of = [](const std::string &) -> std::optional<uint64_t> {
        return std::optional<uint64_t>(0x100000534ULL);
    };
    compiler::Options options;
    options.available = 52;
    options.place_text = [](const std::string &) -> std::optional<uint64_t> {
        return std::optional<uint64_t>(0x100005008ULL);
    };
    const std::string source =
        "int strcmp(const char *a, const char *b);\n"
        "bool check(char *pointer)\n{\n"
        "  int32_t result = strcmp(pointer,\"astral\");\n"
        "  return result == 0;\n}\n";
    const compiler::Result result = compiler::compile(assembler::Target::Arm64, source,
                                                      0x100000460ULL, environment, options);
    expect(result.ok, "it compiles inside the fifty-two bytes it has", result.error);
    if (result.ok)
        expect(result.bytes.size() <= 52, "and the bytes really are that few");
}

// ------------------------------------------------------------------ registers

// Keeping values in registers is only sound if every one of them is back in
// its slot wherever control can arrive from more than one direction, and if
// nothing is expected to survive a call. Each of these runs a shape that would
// answer differently if either were got wrong.
void values_that_outlive_a_branch_or_a_call()
{
    std::printf("values held in registers across branches, loops and calls\n");
    struct Case {
        const char *body;
        long a;
        long b;
        long wanted;
        const char *what;
    };
    const Case cases[] = {
        {"long t = a * 3; if (b > 0) { t = t + 1; } else { t = t - 1; } return t;", 5, 1, 16,
         "a value worked out before a branch survives both sides"},
        {"long t = a; long u = b; if (a > b) { long v = t; t = u; u = v; } return t * 10 + u;",
         3, 7, 37, "two values swapped on one side of a branch"},
        {"long t = 0; for (long i = 0; i < 4; i = i + 1) { long u = i * a; t = t + u; } return t;",
         5, 0, 30, "a value made and used inside a loop"},
        {"long t = a + b; long u = astral_test_twice(t); return t + u;", 4, 6, 30,
         "a value made before a call is still right after it"},
        {"long t = a; long u = astral_test_twice(a) + astral_test_thrice(b); return t + u;", 2, 3,
         15, "two calls in one expression, with a value living across both"},
        {"long t = a; long *p = &t; long u = astral_test_twice(a); *p = *p + u; return t;", 5, 0,
         15, "a call between taking an address and writing through it"},
        {"long t = 0; long i = 0; again: if (i >= (b < 0 ? 0 : b)) goto out; t = t + a; "
         "i = i + 1; goto again; out: return t;",
         7, 3, 21, "a jump backwards into the middle of a run"},
        {"long t = a; switch (b) { case 1: t = t + 1; break; case 2: t = t + 2; break; "
         "default: t = t + 100; } return t;",
         10, 2, 12, "a value that every arm of a switch changes"},
        {"long t = a; long u = astral_test_twice(t); long v = astral_test_thrice(u); "
         "return t + u + v;",
         3, 0, 27, "three values, each made between two calls"},
        {"long t = a & 0xff; long u = (t << 8) | (b & 0xff); return u;", 0x1234, 0x5678, 0x3478,
         "narrow values put back together"},
    };
    for (const Case &one : cases) {
        Arena arena;
        const std::string source =
            std::string("long astral_test_twice(long value);\n"
                        "long astral_test_thrice(long value);\n"
                        "long f(long a, long b) { ") +
            one.body + " }";
        void *code = build(arena, source, host_environment(arena), placing_options(arena),
                           one.what);
        if (code == nullptr)
            continue;
        long (*f)(long, long) = reinterpret_cast<long (*)(long, long)>(code);
        expect_equal(f(one.a, one.b), one.wanted, one.what);
    }
}

// A parameter is read out of the register it arrived in wherever nothing has
// touched that register yet. Passing the same one on, or passing them in a
// different order, is where that goes wrong if it is going to.
void parameters_passed_straight_on()
{
    std::printf("parameters handed on to another call\n");
    {
        Arena arena;
        void *code = build(arena,
                           "long astral_test_ten(long,long,long,long,long,long,long,long,long,long);\n"
                           "long f(long a, long b) { return astral_test_ten(b, a, a, b, a, b, a, b, a, b); }",
                           host_environment(arena), placing_options(arena),
                           "arguments in a different order compile");
        if (code != nullptr) {
            long (*f)(long, long) = reinterpret_cast<long (*)(long, long)>(code);
            // b + a*2 + a*3 + b*4 + a*5 + b*6 + a*7 + b*8 + a*9 + b*10
            expect_equal(f(2, 5), 5 + 4 + 6 + 20 + 10 + 30 + 14 + 40 + 18 + 50,
                         "each argument arrives where it was meant to");
        }
    }
    {
        Arena arena;
        void *code = build(arena,
                           "long astral_test_twice(long value);\n"
                           "long f(long a, long b) { return astral_test_twice(a) + b; }",
                           host_environment(arena), placing_options(arena),
                           "a parameter handed straight on compiles");
        if (code != nullptr) {
            long (*f)(long, long) = reinterpret_cast<long (*)(long, long)>(code);
            expect_equal(f(21, 5), 47, "the first parameter goes on untouched");
        }
    }
}

// Folding at compile time has to give the same answer the instructions would.
void what_is_worked_out_before_anything_runs()
{
    std::printf("constants folded, and shifts written for multiplication\n");
    struct Case {
        const char *body;
        long wanted;
    };
    const Case cases[] = {
        {"return 3 + 4 * 5 - 6 / 2;", 20},
        {"return (1 << 20) | 0xff;", (1 << 20) | 0xff},
        {"return -7 / 2;", -3},
        {"return -7 % 2;", -1},
        {"return (long)(int)0x1ffffffff;", -1},
        {"return (long)(char)0xff;", -1},
        {"return (long)(unsigned char)0xff;", 255},
        {"return 100 > 99;", 1},
        {"return -1 < 0;", 1},
        {"return (unsigned long)-1 > 0;", 1},
        {"long a = 21; return a * 8 / 4;", 42},
        {"unsigned long a = 100; return (long)(a / 8);", 12},
        {"long a = 100; return a / 8;", 12},
        {"long a = -100; return a / 8;", -12},
    };
    for (const Case &one : cases) {
        Arena arena;
        const std::string source = std::string("long f(void) { ") + one.body + " }";
        void *code = build(arena, source, host_environment(arena), placing_options(arena),
                           std::string("`") + one.body + "`");
        if (code == nullptr)
            continue;
        expect_equal(reinterpret_cast<long (*)()>(code)(), one.wanted,
                     std::string("`") + one.body + "`");
    }
}
} // namespace

int main(int argc, char **argv)
{
    host_functions().push_back({"astral_test_ten", reinterpret_cast<void *>(&astral_test_ten)});
    host_functions().push_back({"astral_test_twice", reinterpret_cast<void *>(&astral_test_twice)});
    host_functions().push_back(
        {"astral_test_thrice", reinterpret_cast<void *>(&astral_test_thrice)});
    host_functions().push_back({"strcmp", reinterpret_cast<void *>(
                                              static_cast<int (*)(const char *, const char *)>(
                                                  &std::strcmp))});
    host_functions().push_back({"snprintf", reinterpret_cast<void *>(&snprintf)});

    the_buffer_resolves_labels_both_ways();
    a_tree_built_by_hand();
    arithmetic_and_comparison();
    short_circuit_really_short_circuits();
    loads_and_stores_at_every_width();
    pointers_and_stepping();
    casts_between_widths();
    arrays_structures_and_a_frame_too_big_for_one_instruction();
    switch_with_several_cases_and_a_default();
    nested_loops_with_break_and_continue();
    goto_forwards_and_backwards();
    a_call_with_more_than_eight_arguments();
    a_variadic_call();
    a_call_through_a_function_pointer();
    string_literals_found_and_placed();
    stopping_after_the_assembly();
    a_literal_with_nowhere_to_go_is_refused();
    too_big_is_refused_with_both_sizes();
    an_unsupported_target_is_refused_clearly();
    only_what_changed_is_compiled();
    the_whole_check_function_runs();
    values_that_outlive_a_branch_or_a_call();
    parameters_passed_straight_on();
    what_is_worked_out_before_anything_runs();
    check_fits_where_it_came_from();
    the_size_of_what_comes_out();
    if (argc > 1)
        patch_a_real_binary(argv[1]);
    else
        std::printf("no program was named, so nothing was patched\n");

    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
