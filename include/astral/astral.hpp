// C++17 interface to Astral.
//
// Link with -lAstral, the same library the C API lives in. Handles are
// move-only and own what they wrap; failures raise astral::Error.
#ifndef ASTRAL_HPP
#define ASTRAL_HPP

#include "astral/astral.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace astral {

class Error : public std::runtime_error {
public:
    Error(astral_status status, const std::string &message);
    astral_status status() const noexcept { return status_; }

private:
    astral_status status_;
};

std::string version();
std::string upstream_version();

// Loads the SLEIGH specifications. An empty path uses the installed default or
// the ASTRAL_SPECS environment variable.
void initialize(const std::string &spec_root = std::string());
void shutdown();

// Scoped form of initialize/shutdown.
class Library {
public:
    explicit Library(const std::string &spec_root = std::string());
    ~Library();
    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;
};

struct Language {
    std::string id;
    std::string description;
};

std::vector<Language> languages();

// Compiles a SLEIGH specification into the .sla the decompiler loads.
void compile_sleigh(const std::string &slaspec, const std::string &sla);

struct Segment {
    std::string name;
    uint64_t address = 0;
    uint64_t size = 0;
    bool executable = false;
    bool writable = false;
};

struct Symbol {
    std::string name;
    uint64_t address = 0;
    uint64_t size = 0;
    bool is_function = false;
};

struct Variable {
    std::string name;
    std::string type;
};

struct Call {
    uint64_t address = 0;
    std::string name;
};

// How astral::Program::emit_c renders its output.
struct COptions {
    // Inline the runtime rather than including <astral/decompiled.h>.
    bool self_contained = true;
    // Keep the decompiler's warning comments.
    bool comments = true;

    unsigned to_flags() const;
};

// One decompiled function.
class Function {
public:
    Function() = default;
    explicit Function(astral_function *handle) noexcept;
    ~Function();

    Function(Function &&other) noexcept;
    Function &operator=(Function &&other) noexcept;
    Function(const Function &) = delete;
    Function &operator=(const Function &) = delete;

    explicit operator bool() const noexcept { return handle_ != nullptr; }
    astral_function *get() const noexcept { return handle_; }

    std::string name() const;
    uint64_t address() const;
    uint64_t size() const;
    // The decompiler's listing, which reads as C but does not compile on its
    // own; use Program::emit_c for a translation unit that does.
    std::string c_code() const;
    std::string signature() const;
    std::string return_type() const;
    std::string calling_convention() const;

    std::vector<Variable> parameters() const;
    std::vector<Variable> locals() const;
    std::vector<Call> callees() const;
    std::vector<uint64_t> block_addresses() const;

private:
    void reset();

    astral_function *handle_ = nullptr;
};

// A loaded executable.
class Program {
public:
    Program() = default;
    ~Program();

    Program(Program &&other) noexcept;
    Program &operator=(Program &&other) noexcept;
    Program(const Program &) = delete;
    Program &operator=(const Program &) = delete;

    // Detects the container format and derives the language from the file.
    static Program open(const std::string &path, const std::string &language_id = std::string());
    static Program open(const void *data, size_t size,
                        const std::string &language_id = std::string());

    // Treats the bytes as a flat image mapped at `base_address`.
    static Program open_raw(const std::string &path, const std::string &language_id,
                            uint64_t base_address);
    static Program open_raw(const void *data, size_t size, const std::string &language_id,
                            uint64_t base_address);

    explicit operator bool() const noexcept { return handle_ != nullptr; }
    astral_program *get() const noexcept { return handle_; }

    astral_format format() const;
    std::string format_name() const;
    std::string language_id() const;
    std::string compiler_spec() const;
    bool big_endian() const;
    int pointer_size() const;
    uint64_t image_base() const;

    std::vector<uint64_t> entry_points() const;
    std::vector<Segment> segments() const;
    std::vector<Symbol> symbols() const;
    std::optional<Symbol> find_symbol(std::string_view name) const;

    // Reads mapped bytes. A short result means the range leaves the image.
    std::vector<uint8_t> read(uint64_t address, size_t size) const;

    void add_symbol(uint64_t address, const std::string &name, bool is_function = true);
    void set_option(const std::string &name, const std::string &value);

    std::string disassemble(uint64_t address, int count) const;
    std::string pcode(uint64_t address, int count) const;

    Function decompile(uint64_t address, const std::string &name = std::string()) const;
    Function decompile(std::string_view name) const;

    // Compilable C for the functions at these addresses.
    std::string emit_c(const std::vector<uint64_t> &addresses,
                       const COptions &options = COptions()) const;
    // Compilable C for every function symbol in the program.
    std::string emit_c_all(const COptions &options = COptions()) const;

private:
    explicit Program(astral_program *handle) noexcept;
    void reset();

    astral_program *handle_ = nullptr;
};

} // namespace astral

#endif
