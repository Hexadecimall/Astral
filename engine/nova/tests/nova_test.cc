// Checks on Nova from text to running bytes.
//
// A compiler is checked by running what it wrote. Every case here compiles a
// Nova function into a page of memory and calls it, at every level the
// language has, and then checks that changing the text costs what the change
// cost and no more: a rename costs nothing, a string costs its bytes, and only
// a changed function is written again.
#include "nova.hh"

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <sys/mman.h>
#include <vector>

using namespace astral_internal;

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

void expect_equal(long long got, long long wanted, const std::string &what)
{
    char saw[128];
    std::snprintf(saw, sizeof saw, "got %lld, wanted %lld", got, wanted);
    report(got == wanted, what, saw);
}

std::string diagnostics_text(const std::vector<nova::Diagnostic> &diagnostics)
{
    std::string text;
    for (const nova::Diagnostic &d : diagnostics)
        text += "\n     " + std::to_string(d.line) + ": " + d.message;
    return text;
}

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
    uint64_t place(const std::string &text)
    {
        char *at = static_cast<char *>(data_) + used_;
        std::memcpy(at, text.data(), text.size());
        at[text.size()] = '\0';
        used_ += text.size() + 1;
        used_ = (used_ + 7) / 8 * 8;
        return reinterpret_cast<uint64_t>(at);
    }
    void *install(const std::vector<uint8_t> &bytes)
    {
        mprotect(code_, kSize, PROT_READ | PROT_WRITE);
        std::memcpy(code_, bytes.data(), bytes.size());
        if (mprotect(code_, kSize, PROT_READ | PROT_EXEC) != 0) {
            std::perror("mprotect");
            std::exit(2);
        }
        __builtin___clear_cache(static_cast<char *>(code_), static_cast<char *>(code_) + bytes.size());
        return code_;
    }

private:
    static const size_t kSize = 1 << 16;
    void *code_ = nullptr;
    void *data_ = nullptr;
    size_t used_ = 0;
};

extern "C" int nova_test_twice(int value) { return value * 2; }

// The names a test's Nova may call, answered with real addresses.
nova::Environment host_environment(Arena &arena, const std::vector<std::string> &texts = {})
{
    nova::Environment environment;
    environment.address_of = [](const std::string &name) -> std::optional<uint64_t> {
        if (name == "strcmp")
            return reinterpret_cast<uint64_t>(&std::strcmp);
        if (name == "strlen")
            return reinterpret_cast<uint64_t>(&std::strlen);
        if (name == "twice")
            return reinterpret_cast<uint64_t>(&nova_test_twice);
        return std::nullopt;
    };
    static std::vector<std::pair<std::string, uint64_t>> resident;
    for (const std::string &text : texts)
        resident.emplace_back(text, arena.place(text));
    environment.address_of_text = [](const std::string &text) -> std::optional<uint64_t> {
        for (const auto &one : resident)
            if (one.first == text)
                return one.second;
        return std::nullopt;
    };
    return environment;
}

nova::Options placing_options(Arena &arena)
{
    nova::Options options;
    Arena *held = &arena;
    options.place_text = [held](const std::string &text) -> std::optional<uint64_t> {
        return held->place(text);
    };
    options.keep_assembly = true;
    return options;
}

void *build(Arena &arena, const std::string &source, const std::string &what, nova::Result *out = nullptr,
            const std::vector<std::string> &texts = {})
{
    nova::Environment environment = host_environment(arena, texts);
    nova::Options options = placing_options(arena);
    nova::Result result = nova::compile(assembler::Target::Arm64, source, arena.code_address(),
                                        environment, options);
    if (out)
        *out = result;
    if (!result.ok) {
        report(false, what, "would not compile: " + result.error + diagnostics_text(result.diagnostics));
        return nullptr;
    }
    return arena.install(result.bytes);
}

// ------------------------------------------------------------------ levels

const char *kLevel3 =
    "func check(string: *i8): bool {\n"
    "    return (strcmp(string, \"astral\") == 0);\n"
    "}\n";

const char *kLevel2 =
    "func check(string: *i8 @ x0): bool @ w0 {\n"
    "    var result: i32 @ w0 = strcmp(string, \"astral\");\n"
    "    return (result == 0);\n"
    "}\n";

const char *kLevel1 =
    "func check(@x0): @w0 {\n"
    "    x1 = &\"astral\";\n"
    "    call strcmp;\n"
    "    w0 = (w0 == 0);\n"
    "}\n";

// mov w0, #7 ; ret
const char *kLevel0 =
    "func seven @ 0x1000 {\n"
    "    asm {\n"
    "        mov w0, #7\n"
    "        ret\n"
    "    }\n"
    "}\n";

void every_level_runs()
{
    std::printf("every level compiles and runs\n");
    struct Case {
        const char *name;
        const char *source;
    };
    const Case cases[] = {{"level 3", kLevel3}, {"level 2", kLevel2}, {"level 1", kLevel1}};
    for (const Case &one : cases) {
        Arena arena;
        nova::Result result;
        void *code = build(arena, one.source, std::string(one.name) + " compiles", &result);
        if (!code)
            continue;
        auto check = reinterpret_cast<int (*)(const char *)>(code);
        expect_equal(check("astral"), 1, std::string(one.name) + ": the right key is accepted");
        expect_equal(check("wrong"), 0, std::string(one.name) + ": the wrong key is refused");
    }
    {
        Arena arena;
        nova::Result result;
        void *code = build(arena, kLevel0, "level 0 assembles", &result);
        if (code) {
            auto seven = reinterpret_cast<int (*)()>(code);
            expect_equal(seven(), 7, "level 0: the instructions ran as written");
            report(result.bytes.size() == 8, "level 0: exactly the two instructions, nothing around them",
                   std::to_string(result.bytes.size()) + " bytes");
        }
    }
}

// ------------------------------------------------------------------ language

void language_features()
{
    std::printf("the parts of the language that are not C\n");
    {
        Arena arena;
        void *code = build(arena,
            "func total(items: *i32, count: i32): i32 {\n"
            "    var sum: i32 = 0;\n"
            "    for (index in 0..count) {\n"
            "        sum += items[index];\n"
            "    }\n"
            "    return sum;\n"
            "}\n", "for over a range compiles");
        if (code) {
            int items[] = {1, 2, 3, 4, 5};
            expect_equal(reinterpret_cast<int (*)(int *, int)>(code)(items, 5), 15, "for over a range adds up");
        }
    }
    {
        Arena arena;
        void *code = build(arena,
            "enum Kind { Zero, One, Many = 10 }\n"
            "func classify(value: i32): i32 {\n"
            "    match (value) {\n"
            "        0 { return Kind.Zero; }\n"
            "        1 or 2 { return Kind.One; }\n"
            "        else { return Kind.Many; }\n"
            "    }\n"
            "    return -1;\n"
            "}\n", "match with enum constants compiles");
        if (code) {
            auto classify = reinterpret_cast<int (*)(int)>(code);
            expect_equal(classify(0), 0, "match: first arm");
            expect_equal(classify(2), 1, "match: an arm with two values");
            expect_equal(classify(9), 10, "match: else");
        }
    }
    {
        Arena arena;
        void *code = build(arena,
            "func countdown(start: i32): i32 {\n"
            "    var steps: i32 = 0;\n"
            "    var value: i32 = start;\n"
            "    loop {\n"
            "        if (value <= 0) { break; }\n"
            "        value -= 1;\n"
            "        steps += 1;\n"
            "    }\n"
            "    return steps;\n"
            "}\n", "loop with break compiles");
        if (code)
            expect_equal(reinterpret_cast<int (*)(int)>(code)(6), 6, "loop runs until break");
    }
    {
        Arena arena;
        void *code = build(arena,
            "struct Header {\n"
            "    var magic: u32 @ 0;\n"
            "    var size: u32 @ 4;\n"
            "    var low: u16 @ 4;\n"
            "}\n"
            "func low_half(header: *Header): u32 {\n"
            "    return header.low;\n"
            "}\n", "overlapping members compile");
        if (code) {
            struct { uint32_t magic; uint32_t size; } header = {1, 0x12345678};
            expect_equal(reinterpret_cast<unsigned (*)(void *)>(code)(&header), 0x5678,
                         "an overlapping member reads the bytes it names");
        }
    }
    {
        Arena arena;
        void *code = build(arena,
            "func widen(value: unknown32): u64 {\n"
            "    return (value as u32) as u64;\n"
            "}\n", "unknown storage widens once told how");
        if (code)
            expect_equal(static_cast<long long>(reinterpret_cast<uint64_t (*)(uint32_t)>(code)(0xffffffffu)),
                         0xffffffffLL, "unknown32 as u32 as u64 zero-extends");
    }
    {
        Arena arena;
        void *code = build(arena,
            "func through(value: i32): i32 {\n"
            "    return twice(value) + 1;\n"
            "}\n", "a call into the program compiles");
        if (code)
            expect_equal(reinterpret_cast<int (*)(int)>(code)(20), 41, "the program's function was called");
    }
}

// ------------------------------------------------------------------ refusals

void refusals()
{
    std::printf("what Nova refuses, and how it says so\n");
    struct Case {
        const char *what;
        const char *source;
        const char *expect;
    };
    const Case cases[] = {
        {"dividing unknown storage",
         "func half(value: unknown32): unknown32 { return value / 2; }\n", "signed"},
        {"writing a val",
         "func f(): i32 { val limit: i32 = 3; limit = 4; return limit; }\n", "val"},
        {"a parameter in the wrong register",
         "func f(a: i32 @ x1): i32 { return a; }\n", "x0"},
        {"a result in the wrong register",
         "func f(): i32 @ w3 { return 1; }\n", "x0"},
        {"reading an entry value",
         "func f(): u64 { return x29@entry; }\n", "entry"},
        {"asm mixed with statements",
         "func f(): i32 { var a: i32 = 1; asm { nop } return a; }\n", "asm"},
    };
    for (const Case &one : cases) {
        std::vector<nova::Diagnostic> diagnostics;
        bool ok = nova::check(one.source, diagnostics);
        std::string all = diagnostics_text(diagnostics);
        report(!ok && all.find(one.expect) != std::string::npos,
               std::string("refuses ") + one.what + " and says why",
               ok ? "it was accepted" : "the message does not mention '" + std::string(one.expect) + "':" + all);
    }
    {
        std::vector<nova::Diagnostic> diagnostics;
        bool ok = nova::check(kLevel3, diagnostics);
        bool clean = true;
        for (const nova::Diagnostic &d : diagnostics)
            if (d.message.compare(0, 9, "warning: ") != 0)
                clean = false;
        report(ok && clean, "accepts a clean level 3 function without complaint", diagnostics_text(diagnostics));
    }
}

// ------------------------------------------------------------------ updates

void incremental_updates()
{
    std::printf("an edit costs what the edit cost\n");
    Arena arena;
    const std::string before = kLevel3;
    nova::Environment environment = host_environment(arena, {"astral"});
    nova::Options options = placing_options(arena);

    nova::Result first = nova::compile(assembler::Target::Arm64, before, arena.code_address(), environment, options);
    if (!first.ok) {
        report(false, "the starting point compiles", first.error);
        return;
    }
    options.existing = first.bytes;

    {
        // Tier one: renaming a local and a parameter reaches no bytes.
        std::string renamed =
            "func check(candidate: *i8): bool {\n"
            "    # a comment, and a different name\n"
            "    return (strcmp(candidate, \"astral\") == 0);\n"
            "}\n";
        nova::Update update;
        nova::Result result = nova::compile_update(assembler::Target::Arm64, before, renamed,
                                                   arena.code_address(), environment, update, options);
        report(result.ok && update.regions.empty() && update.recompiled.empty()
                   && update.untouched.size() == 1,
               "a rename and a comment produce no regions and recompile nothing",
               result.ok ? std::to_string(update.regions.size()) + " regions, "
                                 + std::to_string(update.recompiled.size()) + " recompiled"
                         : result.error);
    }
    {
        // Tier two: a shorter string is written where the old one lives.
        std::string retexted =
            "func check(string: *i8): bool {\n"
            "    return (strcmp(string, \"bleep\") == 0);\n"
            "}\n";
        nova::Update update;
        nova::Result result = nova::compile_update(assembler::Target::Arm64, before, retexted,
                                                   arena.code_address(), environment, update, options);
        bool one_region = result.ok && update.regions.size() == 1 && update.recompiled.empty();
        report(one_region, "a changed string is one region and nothing is recompiled",
               result.ok ? std::to_string(update.regions.size()) + " regions, "
                                 + std::to_string(update.recompiled.size()) + " recompiled"
                         : result.error);
        if (one_region) {
            const nova::Update::Region &region = update.regions[0];
            std::string wrote(region.bytes.begin(), region.bytes.end());
            report(wrote.compare(0, 6, std::string("bleep\0", 6)) == 0 && region.bytes.size() == 7,
                   "the region is the new text, terminated, padded to the old length",
                   std::to_string(region.bytes.size()) + " bytes");
        }
    }
    {
        // Tier three: a real change recompiles that function only.
        std::string changed =
            "func check(string: *i8): bool {\n"
            "    return (strlen(string) == 6);\n"
            "}\n";
        nova::Update update;
        nova::Result result = nova::compile_update(assembler::Target::Arm64, before, changed,
                                                   arena.code_address(), environment, update, options);
        report(result.ok && update.recompiled.size() == 1 && !update.regions.empty(),
               "a changed body recompiles the function",
               result.ok ? std::to_string(update.recompiled.size()) + " recompiled, "
                                 + std::to_string(update.regions.size()) + " regions"
                         : result.error);
        if (result.ok && !update.regions.empty()) {
            // Apply the regions to a copy and run it.
            std::vector<uint8_t> image = first.bytes;
            for (const nova::Update::Region &region : update.regions) {
                size_t offset = region.address - arena.code_address();
                if (offset + region.bytes.size() > image.size())
                    image.resize(offset + region.bytes.size());
                std::memcpy(image.data() + offset, region.bytes.data(), region.bytes.size());
            }
            auto check = reinterpret_cast<int (*)(const char *)>(arena.install(image));
            expect_equal(check("sixsix"), 1, "the rewritten function runs with its new meaning");
            expect_equal(check("astral!"), 0, "and refuses what the old one accepted");
        }
    }
}

} // namespace

int main()
{
    every_level_runs();
    language_features();
    refusals();
    incremental_updates();
    std::printf("\nTotals: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
