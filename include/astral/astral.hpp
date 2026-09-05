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
    // Named for other images to call.
    bool is_exported = false;
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
    // Say which names Astral chose and why.
    bool explain = false;

    unsigned to_flags() const;
};

class Debugger;

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
    // The same instructions written to be read: calls and branches by name,
    // labels where a branch comes back to, and what a loaded address holds.
    std::string disassemble_readable(uint64_t address, int count) const;
    std::string pcode(uint64_t address, int count) const;

    Function decompile(uint64_t address, const std::string &name = std::string()) const;
    Function decompile(std::string_view name) const;

    // Compilable C for the functions at these addresses.
    std::string emit_c(const std::vector<uint64_t> &addresses,
                       const COptions &options = COptions()) const;
    // Compilable C for every function symbol in the program.
    std::string emit_c_all(const COptions &options = COptions()) const;

    // Renames the function at an address. With `learn`, the new name is
    // recorded against a fingerprint of the body, so the same code is
    // recognised in another program.
    void rename(uint64_t address, const std::string &name, bool learn = false);
    // Records every named function against its fingerprint. Returns how many.
    int learn_symbols();
    // How many threads whole-program decompilation may use. Zero means one per
    // core, one means no extra threads. Each thread runs its own engine over
    // the same image, so the gain per thread is well short of a core, and two
    // different counts can produce slightly different output.
    void set_threads(int count);
    int threads() const;

    // Whether names are recovered from evidence rather than left as addresses.
    void set_auto_naming(bool enabled);
    bool auto_naming() const;

    // --- Patching ---------------------------------------------------------
    // Edits queue against the open image and apply to the file that
    // write_patched produces; the image in memory changes too, so a
    // decompilation taken afterwards reflects the edit.

    // Instruction length at an address, or 0 if it will not decode.
    int instruction_length(uint64_t address) const;
    // Queues a raw byte edit.
    void patch_bytes(uint64_t address, const void *bytes, size_t size,
                     const std::string &note = std::string());
    void patch_bytes(uint64_t address, const std::vector<uint8_t> &bytes,
                     const std::string &note = std::string());
    // Queues `count` no-ops at an address.
    void patch_nop(uint64_t address, int count = 1);
    // Queues replacing the instruction at an address with the one written here.
    void patch_assembly(uint64_t address, const std::string &text);
    // Queues inverting the conditional branch at an address.
    void patch_invert(uint64_t address);
    // Queues overwriting the function at an address so it only returns a value.
    void patch_return(uint64_t address, uint64_t value);

    size_t patch_count() const;
    void patch_undo();
    void patch_clear();
    // The queued patches as readable patches.astral text.
    std::string patch_text() const;
    // Writes the original file with every queued patch applied. An arm64
    // Mach-O is re-signed on the way out, because the kernel will not run one
    // whose signature no longer covers its bytes.
    void write_patched(const std::string &out_path) const;

    // --- Debugging --------------------------------------------------------
    // Opens the program on the emulator and holds it still. `entry` of zero is
    // the program's own entry point. The debugger reads this program, so it
    // must not outlive it.
    Debugger debug(uint64_t entry = 0, const std::vector<std::string> &arguments = {},
                   const std::string &input = std::string(), uint64_t step_limit = 0);

private:
    explicit Program(astral_program *handle) noexcept;
    void reset();

    astral_program *handle_ = nullptr;
};

// A program stopped where it was told, with everything it holds readable and
// changeable while it is stopped. Nothing here touches the operating system.
class Debugger {
public:
    Debugger() = default;
    explicit Debugger(astral_debugger *handle) noexcept;
    ~Debugger();

    Debugger(Debugger &&other) noexcept;
    Debugger &operator=(Debugger &&other) noexcept;
    Debugger(const Debugger &) = delete;
    Debugger &operator=(const Debugger &) = delete;

    explicit operator bool() const noexcept { return handle_ != nullptr; }

    // Why it is not running just now, and where it is.
    struct State {
        astral_stop stop = ASTRAL_STOP_NOT_STARTED;
        std::string reason;
        uint64_t address = 0;
        std::string function;
        uint64_t steps = 0;
        bool live = true;
        // What it wrote and which library calls it made, since the last stop.
        std::string output;
        std::vector<std::string> calls;
    };

    State start();
    State step();
    State step_over();
    State step_out();
    State run_to(uint64_t address);
    State go();
    // Safe to call from another thread while a run is in progress.
    void cancel();
    State state() const;

    // Whether to keep a line for every instruction executed from here on. Off
    // unless asked for: a line per instruction is millions of them on anything
    // the size of a real program.
    void set_trace(bool on);
    // Every instruction recorded since, in the order they ran.
    std::vector<std::string> trace() const;

    void add_breakpoint(uint64_t address);
    void remove_breakpoint(uint64_t address);
    void clear_breakpoints();
    std::vector<uint64_t> breakpoints() const;

    // Stops when any byte in the range is written.
    void add_watchpoint(uint64_t address, uint64_t size);
    void remove_watchpoint(uint64_t address);
    void clear_watchpoints();

    struct Register {
        std::string name;
        uint64_t value = 0;
    };
    std::vector<Register> registers() const;
    uint64_t register_value(const std::string &name) const;
    void set_register(const std::string &name, uint64_t value);

    // A short result means the range leaves the memory the program has.
    std::vector<uint8_t> read(uint64_t address, size_t size) const;
    void write(uint64_t address, const std::vector<uint8_t> &bytes);
    // The NUL-terminated text at an address, as the program holds it.
    std::string read_text(uint64_t address) const;

    struct Frame {
        uint64_t address = 0;
        uint64_t frame_pointer = 0;
        std::string function;
    };
    // Innermost first. Best effort: the walk stops rather than inventing frames.
    std::vector<Frame> stack() const;

    struct CallResult {
        uint64_t result = 0;
        std::string output;
    };
    // Runs one function and hands back what it answered, leaving the debugger
    // where it was. An argument written as a number is passed as that number;
    // anything else is written into memory the call can reach and passed as a
    // pointer to it.
    CallResult call(uint64_t address, const std::vector<std::string> &arguments = {},
                    uint64_t step_limit = 0);

    // Opaque bytes: registers, and the memory pages that have been written.
    std::vector<uint8_t> snapshot() const;
    void restore(const std::vector<uint8_t> &bytes);

private:
    void reset();

    astral_debugger *handle_ = nullptr;
};

} // namespace astral

#endif
