// Classes, run rather than parsed.
//
// A class in Nova is a struct that owns functions. That is the whole of it:
// the members are laid out as a struct's are, the functions it owns are lifted
// out under `Class::name` and compiled like any other, and `self` is a pointer
// to the class when it is written without a type. So the checks here are not
// about syntax - they compile a class and call it, because a language feature
// that parses and does not run is not a language feature.
#include "nova.hh"

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <map>
#include <optional>
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

class Arena {
public:
    Arena()
    {
        code_ = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (code_ == MAP_FAILED) {
            std::perror("mmap");
            std::exit(2);
        }
    }
    ~Arena()
    {
        if (code_ != MAP_FAILED)
            munmap(code_, kSize);
    }
    uint64_t code_address() const { return reinterpret_cast<uint64_t>(code_); }
    void *install(const std::vector<uint8_t> &bytes)
    {
        std::memcpy(code_, bytes.data(), bytes.size());
        if (mprotect(code_, kSize, PROT_READ | PROT_EXEC) != 0) {
            std::perror("mprotect");
            std::exit(2);
        }
        __builtin___clear_cache(static_cast<char *>(code_), static_cast<char *>(code_) + kSize);
        return code_;
    }

private:
    static constexpr size_t kSize = 64 * 1024;
    void *code_ = nullptr;
};

// Compiles one named function of `source` at `where`, resolving any call it
// makes through `known`. A class's methods are functions like any other, so
// they are compiled and placed like any other - which is also how a patch
// works: the function being replaced calls things the program already has.
std::vector<uint8_t> build_one(const std::string &source, const std::string &function,
                               uint64_t where, const std::map<std::string, uint64_t> &known,
                               const std::string &what)
{
    nova::Environment environment;
    environment.address_of = [&known](const std::string &name) -> std::optional<uint64_t> {
        const auto found = known.find(name);
        if (found == known.end())
            return std::nullopt;
        return found->second;
    };
    nova::Options options;
    options.function = function;
    const nova::Result result =
        nova::compile(assembler::Target::Arm64, source, where, environment, options);
    if (!result.ok) {
        report(false, what,
               "would not compile: " + result.error + diagnostics_text(result.diagnostics));
        return {};
    }
    return result.bytes;
}

// ---------------------------------------------------------------- the cases

const char *kCounter =
    "class Counter {\n"
    "    var total: i32;\n"
    "    var step: i32;\n"
    "\n"
    "    func bump(self: *Counter) {\n"
    "        self.total = self.total + self.step;\n"
    "    }\n"
    "\n"
    "    func value(self: *Counter): i32 {\n"
    "        return self.total;\n"
    "    }\n"
    "}\n"
    "\n"
    "func main(): i32 {\n"
    "    var c: Counter;\n"
    "    c.total = 0;\n"
    "    c.step = 7;\n"
    "    Counter::bump(&c);\n"
    "    Counter::bump(&c);\n"
    "    return Counter::value(&c);\n"
    "}\n";

// `self` with no type is the class it is written in, which is the only reason
// to write a function inside one.
const char *kImplicitSelf =
    "class Box {\n"
    "    var held: i32;\n"
    "    func get(self): i32 {\n"
    "        return self.held;\n"
    "    }\n"
    "}\n"
    "func main(): i32 {\n"
    "    var b: Box;\n"
    "    b.held = 9;\n"
    "    return Box::get(&b);\n"
    "}\n";

// Lays the named functions out one after another in the arena and runs the
// last of them. Every one is compiled knowing where the ones before it landed.
long long run_class(const std::string &source, const std::vector<std::string> &order,
                    const std::string &what, bool &ok)
{
    Arena arena;
    std::map<std::string, uint64_t> known;
    std::vector<uint8_t> image;
    uint64_t entry = 0;
    for (const std::string &name : order) {
        const uint64_t where = arena.code_address() + image.size();
        const std::vector<uint8_t> bytes = build_one(source, name, where, known, what);
        if (bytes.empty()) {
            ok = false;
            return 0;
        }
        known[name] = where;
        entry = where;
        image.insert(image.end(), bytes.begin(), bytes.end());
        while (image.size() % 4 != 0)
            image.push_back(0);
    }
    void *code = arena.install(image);
    ok = true;
    const uint64_t offset = entry - arena.code_address();
    return reinterpret_cast<int (*)()>(static_cast<char *>(code) + offset)();
}

void a_class_runs()
{
    std::printf("\na class is a struct that owns its functions\n");
    bool ok = false;
    const long long answer =
        run_class(kCounter, {"Counter::bump", "Counter::value", "main"}, "Counter compiles", ok);
    if (!ok)
        return;
    report(true, "Counter compiles", "");
    expect_equal(answer, 14, "two bumps of seven answer fourteen");
}

void self_needs_no_type()
{
    std::printf("\nself is the class it is written in\n");
    bool ok = false;
    const long long answer = run_class(kImplicitSelf, {"Box::get", "main"}, "Box compiles", ok);
    if (!ok)
        return;
    report(true, "Box compiles", "");
    expect_equal(answer, 9, "it answers what it holds");
}

void a_struct_is_still_data()
{
    std::printf("\na struct holds data and says so\n");
    const char *source =
        "struct Point {\n"
        "    var x: i32;\n"
        "    func moved(self): i32 { return self.x; }\n"
        "}\n";
    nova::Environment environment;
    nova::Options options;
    options.function = "Point::moved";
    const nova::Result result =
        nova::compile(assembler::Target::Arm64, source, 0x1000, environment, options);
    bool said = false;
    for (const nova::Diagnostic &d : result.diagnostics)
        said = said || d.message.find("write it as a class") != std::string::npos;
    report(!result.ok && said, "a function in a struct is refused, and it says to use a class",
           result.ok ? "it was allowed" : diagnostics_text(result.diagnostics));
}

} // namespace

int main()
{
    a_class_runs();
    self_needs_no_type();
    a_struct_is_still_data();
    std::printf("\nTotals: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
