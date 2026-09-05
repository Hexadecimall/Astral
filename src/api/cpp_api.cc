// The C++ interface, compiled into the library so consumers need only -lAstral.
#include "astral/astral.hpp"

#include <cstdlib>

namespace astral {
namespace {

std::string text(const char *s) { return s != nullptr ? std::string(s) : std::string(); }

std::string last_message()
{
    const char *message = astral_last_error();
    return message != nullptr && *message != '\0' ? message : "unknown failure";
}

[[noreturn]] void raise(astral_status status) { throw Error(status, last_message()); }

void check(astral_status status)
{
    if (status != ASTRAL_OK)
        raise(status);
}

std::string take_string(char *owned, astral_status status_on_null)
{
    if (owned == nullptr)
        raise(status_on_null);
    std::string result(owned);
    astral_string_free(owned);
    return result;
}

} // namespace

Error::Error(astral_status status, const std::string &message)
    : std::runtime_error(message), status_(status)
{
}

std::string version() { return text(astral_version()); }

std::string upstream_version() { return text(astral_upstream_version()); }

void initialize(const std::string &spec_root)
{
    check(astral_init(spec_root.empty() ? nullptr : spec_root.c_str()));
}

void shutdown() { astral_shutdown(); }

Library::Library(const std::string &spec_root) { initialize(spec_root); }

Library::~Library() { shutdown(); }

std::vector<Language> languages()
{
    std::vector<Language> result;
    const int count = astral_language_count();
    result.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        result.push_back({text(astral_language_id(i)), text(astral_language_description(i))});
    return result;
}

void compile_sleigh(const std::string &slaspec, const std::string &sla)
{
    check(astral_compile_sleigh(slaspec.c_str(), sla.c_str()));
}

unsigned COptions::to_flags() const
{
    unsigned flags = ASTRAL_C_DEFAULT;
    if (!self_contained)
        flags |= ASTRAL_C_INCLUDE_RUNTIME;
    if (!comments)
        flags |= ASTRAL_C_NO_COMMENTS;
    if (explain)
        flags |= ASTRAL_C_EXPLAIN;
    return flags;
}

// ---------------------------------------------------------------- Function

Function::Function(astral_function *handle) noexcept : handle_(handle) {}

Function::~Function() { reset(); }

Function::Function(Function &&other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

Function &Function::operator=(Function &&other) noexcept
{
    if (this != &other) {
        reset();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

void Function::reset()
{
    if (handle_ != nullptr) {
        astral_function_free(handle_);
        handle_ = nullptr;
    }
}

std::string Function::name() const { return text(astral_function_name(handle_)); }

uint64_t Function::address() const { return astral_function_address(handle_); }

uint64_t Function::size() const { return astral_function_size(handle_); }

std::string Function::c_code() const { return text(astral_function_c_code(handle_)); }

std::string Function::signature() const { return text(astral_function_signature(handle_)); }

std::string Function::return_type() const { return text(astral_function_return_type(handle_)); }

std::string Function::calling_convention() const
{
    return text(astral_function_calling_convention(handle_));
}

std::vector<Variable> Function::parameters() const
{
    std::vector<Variable> result;
    const int count = astral_function_parameter_count(handle_);
    result.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        result.push_back({text(astral_function_parameter_name(handle_, i)),
                          text(astral_function_parameter_type(handle_, i))});
    return result;
}

std::vector<Variable> Function::locals() const
{
    std::vector<Variable> result;
    const int count = astral_function_local_count(handle_);
    result.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        result.push_back({text(astral_function_local_name(handle_, i)),
                          text(astral_function_local_type(handle_, i))});
    return result;
}

std::vector<Call> Function::callees() const
{
    std::vector<Call> result;
    const int count = astral_function_callee_count(handle_);
    result.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        result.push_back({astral_function_callee(handle_, i),
                          text(astral_function_callee_name(handle_, i))});
    return result;
}

std::vector<uint64_t> Function::block_addresses() const
{
    std::vector<uint64_t> result;
    const int count = astral_function_block_count(handle_);
    result.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        result.push_back(astral_function_block_address(handle_, i));
    return result;
}

// ----------------------------------------------------------------- Program

Program::Program(astral_program *handle) noexcept : handle_(handle) {}

Program::~Program() { reset(); }

Program::Program(Program &&other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

Program &Program::operator=(Program &&other) noexcept
{
    if (this != &other) {
        reset();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

void Program::reset()
{
    if (handle_ != nullptr) {
        astral_program_close(handle_);
        handle_ = nullptr;
    }
}

Program Program::open(const std::string &path, const std::string &language_id)
{
    astral_program *handle =
        astral_program_open(path.c_str(), language_id.empty() ? nullptr : language_id.c_str());
    if (handle == nullptr)
        raise(ASTRAL_ERR_UNKNOWN_FORMAT);
    return Program(handle);
}

Program Program::open(const void *data, size_t size, const std::string &language_id)
{
    astral_program *handle = astral_program_open_memory(
        data, size, language_id.empty() ? nullptr : language_id.c_str());
    if (handle == nullptr)
        raise(ASTRAL_ERR_UNKNOWN_FORMAT);
    return Program(handle);
}

Program Program::open_raw(const std::string &path, const std::string &language_id,
                          uint64_t base_address)
{
    astral_program *handle =
        astral_program_open_raw(path.c_str(), language_id.c_str(), base_address);
    if (handle == nullptr)
        raise(ASTRAL_ERR_UNKNOWN_LANGUAGE);
    return Program(handle);
}

Program Program::open_raw(const void *data, size_t size, const std::string &language_id,
                          uint64_t base_address)
{
    astral_program *handle =
        astral_program_open_raw_memory(data, size, language_id.c_str(), base_address);
    if (handle == nullptr)
        raise(ASTRAL_ERR_UNKNOWN_LANGUAGE);
    return Program(handle);
}

astral_format Program::format() const { return astral_program_format(handle_); }

std::string Program::format_name() const { return text(astral_program_format_name(handle_)); }

std::string Program::language_id() const { return text(astral_program_language_id(handle_)); }

std::string Program::compiler_spec() const { return text(astral_program_compiler_spec(handle_)); }

bool Program::big_endian() const { return astral_program_is_big_endian(handle_) != 0; }

int Program::pointer_size() const { return astral_program_pointer_size(handle_); }

uint64_t Program::image_base() const { return astral_program_image_base(handle_); }

std::vector<uint64_t> Program::entry_points() const
{
    std::vector<uint64_t> result;
    const int count = astral_program_entry_count(handle_);
    result.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        result.push_back(astral_program_entry(handle_, i));
    return result;
}

std::vector<Segment> Program::segments() const
{
    std::vector<Segment> result;
    const int count = astral_program_segment_count(handle_);
    result.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        result.push_back({text(astral_program_segment_name(handle_, i)),
                          astral_program_segment_address(handle_, i),
                          astral_program_segment_size(handle_, i),
                          astral_program_segment_is_executable(handle_, i) != 0,
                          astral_program_segment_is_writable(handle_, i) != 0});
    return result;
}

std::vector<Symbol> Program::symbols() const
{
    std::vector<Symbol> result;
    const int count = astral_program_symbol_count(handle_);
    result.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        result.push_back({text(astral_program_symbol_name(handle_, i)),
                          astral_program_symbol_address(handle_, i),
                          astral_program_symbol_size(handle_, i),
                          astral_program_symbol_is_function(handle_, i) != 0});
    return result;
}

std::optional<Symbol> Program::find_symbol(std::string_view name) const
{
    const int count = astral_program_symbol_count(handle_);
    for (int i = 0; i < count; ++i) {
        const char *candidate = astral_program_symbol_name(handle_, i);
        if (candidate != nullptr && name == candidate)
            return Symbol{candidate, astral_program_symbol_address(handle_, i),
                          astral_program_symbol_size(handle_, i),
                          astral_program_symbol_is_function(handle_, i) != 0};
    }
    return std::nullopt;
}

std::vector<uint8_t> Program::read(uint64_t address, size_t size) const
{
    std::vector<uint8_t> buffer(size);
    size_t got = astral_program_read(handle_, address, buffer.data(), size);
    buffer.resize(got);
    return buffer;
}

void Program::add_symbol(uint64_t address, const std::string &name, bool is_function)
{
    check(astral_program_add_symbol(handle_, address, name.c_str(), is_function ? 1 : 0));
}

void Program::set_option(const std::string &name, const std::string &value)
{
    check(astral_program_set_option(handle_, name.c_str(), value.c_str()));
}

std::string Program::disassemble(uint64_t address, int count) const
{
    return take_string(astral_disassemble(handle_, address, count), ASTRAL_ERR_NO_SUCH_ADDRESS);
}

std::string Program::disassemble_readable(uint64_t address, int count) const
{
    return take_string(astral_disassemble_readable(handle_, address, count),
                       ASTRAL_ERR_NO_SUCH_ADDRESS);
}

std::string Program::pcode(uint64_t address, int count) const
{
    return take_string(astral_pcode(handle_, address, count), ASTRAL_ERR_NO_SUCH_ADDRESS);
}

Function Program::decompile(uint64_t address, const std::string &name) const
{
    astral_function *handle =
        astral_decompile(handle_, address, name.empty() ? nullptr : name.c_str());
    if (handle == nullptr)
        raise(ASTRAL_ERR_DECOMPILE_FAILED);
    return Function(handle);
}

Function Program::decompile(std::string_view name) const
{
    std::optional<Symbol> symbol = find_symbol(name);
    if (!symbol.has_value())
        throw Error(ASTRAL_ERR_NO_SUCH_ADDRESS, "no symbol named " + std::string(name));
    return decompile(symbol->address, std::string(name));
}

std::string Program::emit_c(const std::vector<uint64_t> &addresses, const COptions &options) const
{
    return take_string(astral_emit_c(handle_, addresses.data(), addresses.size(),
                                     options.to_flags()),
                       ASTRAL_ERR_DECOMPILE_FAILED);
}

std::string Program::emit_c_all(const COptions &options) const
{
    return take_string(astral_emit_c_all(handle_, options.to_flags()),
                       ASTRAL_ERR_DECOMPILE_FAILED);
}

void Program::rename(uint64_t address, const std::string &name, bool learn)
{
    check(astral_program_rename(handle_, address, name.c_str(), learn ? 1 : 0));
}

int Program::learn_symbols() { return astral_program_learn_symbols(handle_); }

void Program::set_threads(int count) { astral_program_set_threads(handle_, count); }

int Program::threads() const { return astral_program_threads(handle_); }

void Program::set_auto_naming(bool enabled)
{
    astral_program_set_auto_naming(handle_, enabled ? 1 : 0);
}

bool Program::auto_naming() const { return astral_program_auto_naming(handle_) != 0; }

int Program::instruction_length(uint64_t address) const
{
    return astral_program_instruction_length(handle_, address);
}

void Program::patch_bytes(uint64_t address, const void *bytes, size_t size,
                          const std::string &note)
{
    check(astral_program_patch_bytes(handle_, address, bytes, size,
                                     note.empty() ? nullptr : note.c_str()));
}

void Program::patch_bytes(uint64_t address, const std::vector<uint8_t> &bytes,
                          const std::string &note)
{
    patch_bytes(address, bytes.data(), bytes.size(), note);
}

void Program::patch_nop(uint64_t address, int count)
{
    check(astral_program_patch_nop(handle_, address, count));
}

void Program::patch_assembly(uint64_t address, const std::string &text)
{
    check(astral_program_patch_assembly(handle_, address, text.c_str()));
}

void Program::patch_invert(uint64_t address)
{
    check(astral_program_patch_invert(handle_, address));
}

void Program::patch_return(uint64_t address, uint64_t value)
{
    check(astral_program_patch_return(handle_, address, value));
}

size_t Program::patch_count() const { return astral_program_patch_count(handle_); }

void Program::patch_undo() { astral_program_patch_undo(handle_); }

void Program::patch_clear() { astral_program_patch_clear(handle_); }

std::string Program::patch_text() const
{
    char *owned = astral_program_patch_serialize(handle_);
    if (owned == nullptr)
        return std::string(); // nothing queued is not a failure
    std::string result(owned);
    astral_string_free(owned);
    return result;
}

void Program::write_patched(const std::string &out_path) const
{
    check(astral_program_write_patched(handle_, out_path.c_str()));
}


// --- Debugging -------------------------------------------------------------

namespace {

// Splits a report the C API returns one item per line.
std::vector<std::string> lines_of(const std::string &text)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        const size_t stop = text.find('\n', at);
        const size_t end = stop == std::string::npos ? text.size() : stop;
        if (end > at)
            out.push_back(text.substr(at, end - at));
        if (stop == std::string::npos)
            break;
        at = stop + 1;
    }
    return out;
}

std::vector<const char *> pointers_to(const std::vector<std::string> &all)
{
    std::vector<const char *> out;
    out.reserve(all.size() + 1);
    for (const std::string &one : all)
        out.push_back(one.c_str());
    out.push_back(nullptr);
    return out;
}

} // namespace

Debugger Program::debug(uint64_t entry, const std::vector<std::string> &arguments,
                        const std::string &input, uint64_t step_limit)
{
    const std::vector<const char *> argv = pointers_to(arguments);
    astral_debugger *handle = astral_debugger_open(handle_, entry, argv.data(),
                                                   input.empty() ? nullptr : input.c_str(),
                                                   step_limit);
    if (handle == nullptr)
        raise(ASTRAL_ERR_INTERNAL);
    return Debugger(handle);
}

Debugger::Debugger(astral_debugger *handle) noexcept : handle_(handle) {}

Debugger::~Debugger() { reset(); }

Debugger::Debugger(Debugger &&other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

Debugger &Debugger::operator=(Debugger &&other) noexcept
{
    if (this != &other) {
        reset();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

void Debugger::reset()
{
    if (handle_ != nullptr) {
        astral_debugger_free(handle_);
        handle_ = nullptr;
    }
}

Debugger::State Debugger::state() const
{
    State out;
    out.stop = astral_debugger_stop_reason(handle_);
    out.reason = take_string(astral_debugger_reason(handle_), ASTRAL_ERR_INTERNAL);
    out.address = astral_debugger_address(handle_);
    out.function = take_string(astral_debugger_function(handle_), ASTRAL_ERR_INTERNAL);
    out.steps = astral_debugger_steps(handle_);
    out.live = astral_debugger_is_live(handle_) != 0;
    out.output = take_string(astral_debugger_output(handle_), ASTRAL_ERR_INTERNAL);
    out.calls = lines_of(take_string(astral_debugger_calls(handle_), ASTRAL_ERR_INTERNAL));
    return out;
}

Debugger::State Debugger::start()
{
    check(astral_debugger_start(handle_));
    return state();
}

Debugger::State Debugger::step()
{
    check(astral_debugger_step(handle_));
    return state();
}

Debugger::State Debugger::step_over()
{
    check(astral_debugger_step_over(handle_));
    return state();
}

Debugger::State Debugger::step_out()
{
    check(astral_debugger_step_out(handle_));
    return state();
}

Debugger::State Debugger::run_to(uint64_t address)
{
    check(astral_debugger_run_to(handle_, address));
    return state();
}

Debugger::State Debugger::go()
{
    check(astral_debugger_go(handle_));
    return state();
}

void Debugger::cancel() { astral_debugger_cancel(handle_); }

void Debugger::set_trace(bool on) { check(astral_debugger_set_trace(handle_, on ? 1 : 0)); }

std::vector<std::string> Debugger::trace() const
{
    return lines_of(take_string(astral_debugger_trace(handle_), ASTRAL_ERR_INTERNAL));
}

void Debugger::add_breakpoint(uint64_t address)
{
    check(astral_debugger_add_breakpoint(handle_, address));
}

void Debugger::remove_breakpoint(uint64_t address)
{
    check(astral_debugger_remove_breakpoint(handle_, address));
}

void Debugger::clear_breakpoints() { astral_debugger_clear_breakpoints(handle_); }

std::vector<uint64_t> Debugger::breakpoints() const
{
    std::vector<uint64_t> out;
    const int count = astral_debugger_breakpoint_count(handle_);
    for (int i = 0; i < count; ++i)
        out.push_back(astral_debugger_breakpoint(handle_, i));
    return out;
}

void Debugger::add_watchpoint(uint64_t address, uint64_t size)
{
    check(astral_debugger_add_watchpoint(handle_, address, size));
}

void Debugger::remove_watchpoint(uint64_t address)
{
    check(astral_debugger_remove_watchpoint(handle_, address));
}

void Debugger::clear_watchpoints() { astral_debugger_clear_watchpoints(handle_); }

std::vector<Debugger::Register> Debugger::registers() const
{
    std::vector<Register> out;
    for (const std::string &line :
         lines_of(take_string(astral_debugger_registers(handle_), ASTRAL_ERR_INTERNAL))) {
        const size_t gap = line.rfind(' ');
        if (gap == std::string::npos)
            continue;
        Register one;
        one.name = line.substr(0, gap);
        one.value = std::strtoull(line.c_str() + gap + 1, nullptr, 0);
        out.push_back(one);
    }
    return out;
}

uint64_t Debugger::register_value(const std::string &name) const
{
    uint64_t value = 0;
    check(astral_debugger_register(handle_, name.c_str(), &value));
    return value;
}

void Debugger::set_register(const std::string &name, uint64_t value)
{
    check(astral_debugger_set_register(handle_, name.c_str(), value));
}

std::vector<uint8_t> Debugger::read(uint64_t address, size_t size) const
{
    std::vector<uint8_t> out(size);
    out.resize(astral_debugger_read(handle_, address, out.data(), size));
    return out;
}

void Debugger::write(uint64_t address, const std::vector<uint8_t> &bytes)
{
    check(astral_debugger_write(handle_, address, bytes.data(), bytes.size()));
}

std::string Debugger::read_text(uint64_t address) const
{
    return take_string(astral_debugger_read_text(handle_, address), ASTRAL_ERR_INTERNAL);
}

std::vector<Debugger::Frame> Debugger::stack() const
{
    std::vector<Frame> out;
    for (const std::string &line :
         lines_of(take_string(astral_debugger_stack(handle_), ASTRAL_ERR_INTERNAL))) {
        Frame frame;
        const char *at = line.c_str();
        char *end = nullptr;
        frame.address = std::strtoull(at, &end, 0);
        frame.frame_pointer = std::strtoull(end, &end, 0);
        while (*end == ' ')
            ++end;
        frame.function = end;
        out.push_back(frame);
    }
    return out;
}

Debugger::CallResult Debugger::call(uint64_t address, const std::vector<std::string> &arguments,
                                    uint64_t step_limit)
{
    const std::vector<const char *> argv = pointers_to(arguments);
    CallResult out;
    char *output = nullptr;
    check(astral_debugger_call(handle_, address, argv.data(), step_limit, &out.result, &output));
    if (output != nullptr) {
        out.output = output;
        astral_string_free(output);
    }
    return out;
}

std::vector<uint8_t> Debugger::snapshot() const
{
    std::vector<uint8_t> out(astral_debugger_snapshot(handle_, nullptr, 0));
    astral_debugger_snapshot(handle_, out.data(), out.size());
    return out;
}

void Debugger::restore(const std::vector<uint8_t> &bytes)
{
    check(astral_debugger_restore(handle_, bytes.data(), bytes.size()));
}

} // namespace astral
