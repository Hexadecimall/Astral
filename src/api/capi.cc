// The C ABI. Every entry point is noexcept: C++ exceptions are converted into
// status codes plus a thread-local message.
#include "astral/astral.h"

#include "error.hh"
#include "image.hh"
#include "contribute.hh"
#include "knowledge.hh"
#include "dotnet.hh"
#include "session.hh"
#include "source_learn.hh"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace astral_internal;

struct astral_program {
    std::unique_ptr<Session> session;
    std::string language_id;
    std::string compiler_spec;
    std::string format_name;
};

struct astral_function {
    FunctionResult result;
};

struct astral_contribution {
    Contribution value;
    ContributionPolicy policy;
    Submission submission;
};

namespace {

const char *cstr(const std::string &s) { return s.c_str(); }

char *copy_string(const std::string &s)
{
    char *copy = static_cast<char *>(std::malloc(s.size() + 1));
    if (copy != nullptr)
        std::memcpy(copy, s.c_str(), s.size() + 1);
    return copy;
}

template <typename T>
const char *element(const std::vector<T> &v, int index)
{
    if (index < 0 || static_cast<size_t>(index) >= v.size())
        return nullptr;
    return v[static_cast<size_t>(index)].c_str();
}

astral_program *finish_open(BinaryImage image, const char *language_id)
{
    std::string error;
    std::string override_id = language_id != nullptr ? language_id : "";
    std::unique_ptr<Session> session = Session::create(std::move(image), override_id, error);
    if (!session) {
        set_error(error.empty() ? "could not build an architecture for this image" : error);
        return nullptr;
    }
    auto *program = new (std::nothrow) astral_program();
    if (program == nullptr) {
        set_error("out of memory");
        return nullptr;
    }
    program->language_id = session->language_id();
    program->compiler_spec = session->compiler_spec();
    program->format_name = session->image().format_name;
    program->session = std::move(session);
    clear_error();
    return program;
}

} // namespace

extern "C" {

const char *astral_last_error(void) { return last_error(); }

const char *astral_version(void) { return ASTRAL_VERSION; }

const char *astral_upstream_version(void) { return ASTRAL_GHIDRA_VERSION; }

astral_status astral_init(const char *spec_root)
{
    try {
        astral_status status = initialize(spec_root);
        if (status != ASTRAL_OK)
            return fail(status, "no SLEIGH specifications found; build the specs or set "
                                "ASTRAL_SPECS to a directory of compiled language files");
        clear_error();
        return ASTRAL_OK;
    } catch (const std::exception &e) {
        return fail(ASTRAL_ERR_INTERNAL, e.what());
    } catch (...) {
        return fail(ASTRAL_ERR_INTERNAL, "unknown failure during initialization");
    }
}

void astral_shutdown(void)
{
    try {
        terminate();
    } catch (...) {
    }
}

int astral_language_count(void) { return language_count(); }

const char *astral_language_id(int index) { return language_id_at(index); }

const char *astral_language_description(int index) { return language_description_at(index); }

astral_status astral_compile_sleigh(const char *slaspec_path, const char *sla_path)
{
    if (slaspec_path == nullptr || sla_path == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null path");
    std::string error;
    if (!compile_sleigh(slaspec_path, sla_path, error))
        return fail(ASTRAL_ERR_IO, error);
    clear_error();
    return ASTRAL_OK;
}

astral_program *astral_program_open(const char *path, const char *language_id)
{
    if (path == nullptr) {
        set_error("null path");
        return nullptr;
    }
    std::vector<uint8_t> bytes;
    std::string error;
    if (!read_file(path, bytes, error)) {
        set_error(error);
        return nullptr;
    }
    BinaryImage image;
    image.path = path;
    if (!load_any(bytes, image, error)) {
        set_error(error);
        return nullptr;
    }
    return finish_open(std::move(image), language_id);
}

astral_program *astral_program_open_memory(const void *data, size_t size, const char *language_id)
{
    if (data == nullptr && size != 0) {
        set_error("null buffer");
        return nullptr;
    }
    std::vector<uint8_t> bytes(static_cast<const uint8_t *>(data),
                               static_cast<const uint8_t *>(data) + size);
    BinaryImage image;
    std::string error;
    if (!load_any(bytes, image, error)) {
        set_error(error);
        return nullptr;
    }
    return finish_open(std::move(image), language_id);
}

astral_program *astral_program_open_raw(const char *path, const char *language_id,
                                        uint64_t base_address)
{
    if (path == nullptr || language_id == nullptr) {
        set_error("a raw image needs both a path and a language id");
        return nullptr;
    }
    std::vector<uint8_t> bytes;
    std::string error;
    if (!read_file(path, bytes, error)) {
        set_error(error);
        return nullptr;
    }
    BinaryImage image = make_raw_image(bytes, base_address);
    image.path = path;
    return finish_open(std::move(image), language_id);
}

astral_program *astral_program_open_raw_memory(const void *data, size_t size,
                                               const char *language_id, uint64_t base_address)
{
    if (language_id == nullptr || (data == nullptr && size != 0)) {
        set_error("a raw image needs both a buffer and a language id");
        return nullptr;
    }
    std::vector<uint8_t> bytes(static_cast<const uint8_t *>(data),
                               static_cast<const uint8_t *>(data) + size);
    return finish_open(make_raw_image(bytes, base_address), language_id);
}

void astral_program_close(astral_program *program) { delete program; }

astral_format astral_program_format(const astral_program *program)
{
    return program == nullptr ? ASTRAL_FORMAT_UNKNOWN : program->session->image().format;
}

const char *astral_program_format_name(const astral_program *program)
{
    return program == nullptr ? nullptr : cstr(program->format_name);
}

const char *astral_program_language_id(const astral_program *program)
{
    return program == nullptr ? nullptr : cstr(program->language_id);
}

const char *astral_program_compiler_spec(const astral_program *program)
{
    return program == nullptr ? nullptr : cstr(program->compiler_spec);
}

int astral_program_is_big_endian(const astral_program *program)
{
    return program == nullptr ? 0 : (program->session->big_endian() ? 1 : 0);
}

int astral_program_pointer_size(const astral_program *program)
{
    return program == nullptr ? 0 : program->session->pointer_size();
}

uint64_t astral_program_image_base(const astral_program *program)
{
    return program == nullptr ? 0 : program->session->image().image_base;
}

int astral_program_entry_count(const astral_program *program)
{
    return program == nullptr ? 0 : static_cast<int>(program->session->image().entry_points.size());
}

uint64_t astral_program_entry(const astral_program *program, int index)
{
    if (program == nullptr)
        return 0;
    const auto &entries = program->session->image().entry_points;
    if (index < 0 || static_cast<size_t>(index) >= entries.size())
        return 0;
    return entries[static_cast<size_t>(index)];
}

int astral_program_segment_count(const astral_program *program)
{
    return program == nullptr ? 0 : static_cast<int>(program->session->image().segments.size());
}

#define SEGMENT_OR(prog, idx, fallback)                                                            \
    if ((prog) == nullptr)                                                                         \
        return fallback;                                                                           \
    const auto &segments = (prog)->session->image().segments;                                      \
    if ((idx) < 0 || static_cast<size_t>(idx) >= segments.size())                                  \
    return fallback

const char *astral_program_segment_name(const astral_program *program, int index)
{
    SEGMENT_OR(program, index, nullptr);
    return segments[static_cast<size_t>(index)].name.c_str();
}

uint64_t astral_program_segment_address(const astral_program *program, int index)
{
    SEGMENT_OR(program, index, 0);
    return segments[static_cast<size_t>(index)].address;
}

uint64_t astral_program_segment_size(const astral_program *program, int index)
{
    SEGMENT_OR(program, index, 0);
    return segments[static_cast<size_t>(index)].size;
}

int astral_program_segment_is_executable(const astral_program *program, int index)
{
    SEGMENT_OR(program, index, 0);
    return segments[static_cast<size_t>(index)].executable ? 1 : 0;
}

int astral_program_segment_is_writable(const astral_program *program, int index)
{
    SEGMENT_OR(program, index, 0);
    return segments[static_cast<size_t>(index)].writable ? 1 : 0;
}

#undef SEGMENT_OR

#define SYMBOL_OR(prog, idx, fallback)                                                             \
    if ((prog) == nullptr)                                                                         \
        return fallback;                                                                           \
    const auto &symbols = (prog)->session->image().symbols;                                        \
    if ((idx) < 0 || static_cast<size_t>(idx) >= symbols.size())                                   \
    return fallback

int astral_program_symbol_count(const astral_program *program)
{
    return program == nullptr ? 0 : static_cast<int>(program->session->image().symbols.size());
}

const char *astral_program_symbol_name(const astral_program *program, int index)
{
    SYMBOL_OR(program, index, nullptr);
    return symbols[static_cast<size_t>(index)].name.c_str();
}

uint64_t astral_program_symbol_address(const astral_program *program, int index)
{
    SYMBOL_OR(program, index, 0);
    return symbols[static_cast<size_t>(index)].address;
}

uint64_t astral_program_symbol_size(const astral_program *program, int index)
{
    SYMBOL_OR(program, index, 0);
    return symbols[static_cast<size_t>(index)].size;
}

int astral_program_symbol_is_function(const astral_program *program, int index)
{
    SYMBOL_OR(program, index, 0);
    return symbols[static_cast<size_t>(index)].is_function ? 1 : 0;
}

int astral_program_symbol_is_import(const astral_program *program, int index)
{
    SYMBOL_OR(program, index, 0);
    return symbols[static_cast<size_t>(index)].is_import ? 1 : 0;
}

int astral_program_symbol_is_exported(const astral_program *program, int index)
{
    SYMBOL_OR(program, index, 0);
    return symbols[static_cast<size_t>(index)].is_exported ? 1 : 0;
}

#undef SYMBOL_OR

size_t astral_program_read(const astral_program *program, uint64_t address, void *out, size_t size)
{
    if (program == nullptr || out == nullptr)
        return 0;
    return program->session->image().read(address, static_cast<uint8_t *>(out), size);
}

astral_status astral_program_add_symbol(astral_program *program, uint64_t address,
                                        const char *name, int is_function)
{
    if (program == nullptr || name == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null program or name");
    std::string error;
    if (!program->session->add_symbol(address, name, is_function != 0, error))
        return fail(ASTRAL_ERR_INTERNAL, error);
    clear_error();
    return ASTRAL_OK;
}

astral_status astral_program_rename(astral_program *program, uint64_t address, const char *name,
                                    int learn)
{
    if (program == nullptr || name == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null program or name");
    std::string error;
    if (!program->session->rename(address, name, learn != 0, error))
        return fail(ASTRAL_ERR_INTERNAL, error);
    clear_error();
    return ASTRAL_OK;
}

int astral_program_instruction_length(astral_program *program, uint64_t address)
{
    if (program == nullptr)
        return 0;
    return program->session->instruction_length(address);
}

astral_status astral_program_patch_bytes(astral_program *program, uint64_t address,
                                         const void *bytes, size_t size, const char *note)
{
    if (program == nullptr || bytes == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null program or bytes");
    const uint8_t *p = static_cast<const uint8_t *>(bytes);
    std::vector<uint8_t> data(p, p + size);
    std::string error;
    if (!program->session->patch_bytes(address, data, astral_internal::PatchTier::Manual,
                                        note != nullptr ? note : "", error))
        return fail(ASTRAL_ERR_INTERNAL, error);
    clear_error();
    return ASTRAL_OK;
}

astral_status astral_program_patch_nop(astral_program *program, uint64_t address, int count)
{
    if (program == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null program");
    std::string error;
    if (!program->session->patch_nop(address, count, error))
        return fail(ASTRAL_ERR_INTERNAL, error);
    clear_error();
    return ASTRAL_OK;
}

astral_status astral_program_patch_assembly(astral_program *program, uint64_t address,
                                            const char *text)
{
    if (program == nullptr || text == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "a program and some assembly are needed");
    std::string error;
    if (!program->session->patch_assembly(address, text, error))
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, error);
    clear_error();
    return ASTRAL_OK;
}

astral_status astral_program_patch_invert(astral_program *program, uint64_t address)
{
    if (program == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null program");
    std::string error;
    if (!program->session->patch_invert_branch(address, error))
        return fail(ASTRAL_ERR_INTERNAL, error);
    clear_error();
    return ASTRAL_OK;
}

astral_status astral_program_patch_return(astral_program *program, uint64_t address, uint64_t value)
{
    if (program == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null program");
    std::string error;
    if (!program->session->patch_return(address, value, error))
        return fail(ASTRAL_ERR_INTERNAL, error);
    clear_error();
    return ASTRAL_OK;
}

size_t astral_program_patch_count(astral_program *program)
{
    return program == nullptr ? 0 : program->session->patches().size();
}

void astral_program_patch_undo(astral_program *program)
{
    if (program != nullptr)
        program->session->undo_patch();
}

void astral_program_patch_clear(astral_program *program)
{
    if (program != nullptr)
        program->session->patches().clear();
}

char *astral_program_patch_serialize(astral_program *program)
{
    if (program == nullptr)
        return nullptr;
    return copy_string(program->session->patches().serialize());
}

astral_status astral_program_write_patched(astral_program *program, const char *out_path)
{
    if (program == nullptr || out_path == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null program or path");
    std::string error;
    if (!program->session->write_patched(out_path, error))
        return fail(ASTRAL_ERR_INTERNAL, error);
    clear_error();
    return ASTRAL_OK;
}

int astral_program_learn_symbols(astral_program *program)
{
    if (program == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null program");
    std::string error;
    const int learned = program->session->learn_symbols(error);
    if (learned == 0 && !error.empty())
        return fail(ASTRAL_ERR_IO, error);
    clear_error();
    return learned;
}

namespace {
// The policy is handed out as plain C, so its strings have to outlive the call.
ContributionPolicy g_policy;
} // namespace

astral_status astral_contribution_ask(const char *repo, astral_contribution_policy *policy)
{
    if (repo == nullptr || policy == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null repository or policy");
    g_policy = fetch_policy(repo);
    if (!g_policy.error.empty())
        return fail(ASTRAL_ERR_IO, g_policy.error);
    policy->accepted = g_policy.accepted ? 1 : 0;
    policy->method = g_policy.method.c_str();
    policy->message = g_policy.message.c_str();
    policy->record_limit = static_cast<int>(g_policy.record_limit);
    clear_error();
    return ASTRAL_OK;
}

astral_contribution *astral_contribution_prepare(const char *database_path,
                                                 const astral_contribution_policy *policy)
{
    if (database_path == nullptr || policy == nullptr) {
        set_error("null database path or policy");
        return nullptr;
    }
    std::string error;
    auto *contribution = new (std::nothrow) astral_contribution();
    if (contribution == nullptr) {
        set_error("out of memory");
        return nullptr;
    }
    contribution->policy = g_policy;
    contribution->value = prepare_contribution(database_path, contribution->policy, error);
    if (contribution->value.records == 0) {
        set_error(error.empty() ? "nothing to send" : error);
        delete contribution;
        return nullptr;
    }
    clear_error();
    return contribution;
}

void astral_contribution_free(astral_contribution *contribution) { delete contribution; }

int astral_contribution_records(const astral_contribution *c)
{
    return c == nullptr ? 0 : static_cast<int>(c->value.records);
}

int astral_contribution_withheld_kind(const astral_contribution *c)
{
    return c == nullptr ? 0 : static_cast<int>(c->value.withheld_kind);
}

int astral_contribution_withheld_private(const astral_contribution *c)
{
    return c == nullptr ? 0 : static_cast<int>(c->value.withheld_private);
}

int astral_contribution_example_count(const astral_contribution *c)
{
    return c == nullptr ? 0 : static_cast<int>(c->value.examples.size());
}

const char *astral_contribution_example(const astral_contribution *c, int index)
{
    return c == nullptr ? nullptr : element(c->value.examples, index);
}

const char *astral_contribution_send(const char *repo, astral_contribution *contribution,
                                     const char *title)
{
    if (repo == nullptr || contribution == nullptr) {
        set_error("null repository or contribution");
        return nullptr;
    }
    const std::string heading =
        title != nullptr ? title
                         : "Database submission: " +
                               std::to_string(contribution->value.records) + " records";
    std::string error;
    if (!send_contribution(repo, contribution->policy, contribution->value, heading,
                           contribution->submission, error)) {
        set_error(error);
        return nullptr;
    }
    clear_error();
    return contribution->submission.url.c_str();
}

astral_delivery astral_contribution_delivery(const astral_contribution *c)
{
    if (c == nullptr)
        return ASTRAL_DELIVERY_BROWSER;
    switch (c->submission.delivery) {
    case Delivery::Endpoint: return ASTRAL_DELIVERY_ENDPOINT;
    case Delivery::Api: return ASTRAL_DELIVERY_API;
    default: return ASTRAL_DELIVERY_BROWSER;
    }
}

const char *astral_contribution_file(const astral_contribution *c)
{
    return c == nullptr ? nullptr : c->submission.file.c_str();
}

int astral_learn_source(const char *const *paths, int count)
{
    if (paths == nullptr || count <= 0)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "no source paths given");
    std::vector<std::string> list;
    for (int i = 0; i < count; ++i)
        if (paths[i] != nullptr)
            list.emplace_back(paths[i]);
    std::string error;
    const int learned = learn_from_source(list, error);
    if (learned == 0 && !error.empty())
        return fail(ASTRAL_ERR_IO, error);
    clear_error();
    return learned;
}

int astral_knowledge_forget(const char *name)
{
    if (name == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null name");
    std::string error;
    const int removed = Knowledge::instance().forget(name, error);
    if (removed == 0 && !error.empty())
        return fail(ASTRAL_ERR_IO, error);
    clear_error();
    return removed;
}

astral_status astral_knowledge_forget_all(void)
{
    std::string error;
    if (!Knowledge::instance().forget_all(error))
        return fail(ASTRAL_ERR_IO, error);
    clear_error();
    return ASTRAL_OK;
}

void astral_program_set_auto_naming(astral_program *program, int enabled)
{
    if (program != nullptr)
        program->session->set_auto_naming(enabled != 0);
}

void astral_program_set_threads(astral_program *program, int count)
{
    if (program != nullptr)
        program->session->set_threads(count);
}

int astral_program_threads(const astral_program *program)
{
    return program == nullptr ? 0 : program->session->threads();
}

int astral_program_auto_naming(const astral_program *program)
{
    return program == nullptr ? 0 : (program->session->auto_naming() ? 1 : 0);
}

int astral_knowledge_size(void) { return static_cast<int>(Knowledge::instance().size()); }

int astral_knowledge_learned(void)
{
    return static_cast<int>(Knowledge::instance().learned_count());
}

const char *astral_knowledge_path(void) { return Knowledge::instance().user_path().c_str(); }

astral_status astral_knowledge_reload(const char *user_path)
{
    try {
        Knowledge::instance().reload(user_path != nullptr ? user_path : "");
        clear_error();
        return ASTRAL_OK;
    } catch (const std::exception &e) {
        return fail(ASTRAL_ERR_IO, e.what());
    }
}

astral_status astral_program_set_option(astral_program *program, const char *name,
                                        const char *value)
{
    if (program == nullptr || name == nullptr || value == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null program, name or value");
    std::string error;
    if (!program->session->set_option(name, value, error))
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, error);
    clear_error();
    return ASTRAL_OK;
}

char *astral_disassemble(astral_program *program, uint64_t address, int count)
{
    if (program == nullptr) {
        set_error("null program");
        return nullptr;
    }
    std::string text, error;
    if (!program->session->disassemble(address, count, text, error)) {
        set_error(error);
        return nullptr;
    }
    char *copy = static_cast<char *>(std::malloc(text.size() + 1));
    if (copy == nullptr) {
        set_error("out of memory");
        return nullptr;
    }
    std::memcpy(copy, text.c_str(), text.size() + 1);
    clear_error();
    return copy;
}

char *astral_disassemble_readable(astral_program *program, uint64_t address, int count)
{
    if (program == nullptr) {
        fail(ASTRAL_ERR_INVALID_ARGUMENT, "null program");
        return nullptr;
    }
    std::string out;
    std::string error;
    if (!program->session->disassemble_readable(address, count, out, error)) {
        fail(ASTRAL_ERR_NO_SUCH_ADDRESS, error);
        return nullptr;
    }
    clear_error();
    return copy_string(out);
}

char *astral_readable_trace(astral_program *program, const char *raw)
{
    if (program == nullptr || raw == nullptr) {
        fail(ASTRAL_ERR_INVALID_ARGUMENT, "null program");
        return nullptr;
    }
    clear_error();
    return copy_string(program->session->readable_trace(raw));
}

char *astral_pcode(astral_program *program, uint64_t address, int count)
{
    if (program == nullptr) {
        set_error("null program");
        return nullptr;
    }
    std::string text, error;
    if (!program->session->pcode(address, count, text, error)) {
        set_error(error);
        return nullptr;
    }
    char *copy = static_cast<char *>(std::malloc(text.size() + 1));
    if (copy == nullptr) {
        set_error("out of memory");
        return nullptr;
    }
    std::memcpy(copy, text.c_str(), text.size() + 1);
    clear_error();
    return copy;
}

void astral_string_free(char *string) { std::free(string); }

namespace {

char *emit_c_impl(astral_program *program, const std::vector<uint64_t> &addresses,
                  unsigned options)
{
    std::string text, error;
    const bool self_contained = (options & ASTRAL_C_INCLUDE_RUNTIME) == 0;
    const bool comments = (options & ASTRAL_C_NO_COMMENTS) == 0;
    const bool explain = (options & ASTRAL_C_EXPLAIN) != 0;
    if (!program->session->emit_c(addresses, self_contained, comments, explain, text, error)) {
        set_error(error);
        return nullptr;
    }
    char *copy = static_cast<char *>(std::malloc(text.size() + 1));
    if (copy == nullptr) {
        set_error("out of memory");
        return nullptr;
    }
    std::memcpy(copy, text.c_str(), text.size() + 1);
    clear_error();
    return copy;
}

} // namespace

char *astral_emit_c(astral_program *program, const uint64_t *addresses, size_t count,
                    unsigned options)
{
    if (program == nullptr || (addresses == nullptr && count != 0)) {
        set_error("null program or address list");
        return nullptr;
    }
    return emit_c_impl(program, std::vector<uint64_t>(addresses, addresses + count), options);
}

char *astral_emit_c_all(astral_program *program, unsigned options)
{
    if (program == nullptr) {
        set_error("null program");
        return nullptr;
    }
    std::vector<uint64_t> addresses = program->session->function_addresses();
    if (addresses.empty()) {
        set_error("the image records no function symbols; pass addresses explicitly");
        return nullptr;
    }
    return emit_c_impl(program, addresses, options);
}

astral_function *astral_decompile(astral_program *program, uint64_t address, const char *name)
{
    if (program == nullptr) {
        set_error("null program");
        return nullptr;
    }
    auto *function = new (std::nothrow) astral_function();
    if (function == nullptr) {
        set_error("out of memory");
        return nullptr;
    }
    std::string error;
    if (!program->session->decompile(address, name != nullptr ? name : "", function->result,
                                     error)) {
        delete function;
        set_error(error);
        return nullptr;
    }
    clear_error();
    return function;
}

void astral_function_free(astral_function *function) { delete function; }

const char *astral_function_name(const astral_function *f)
{
    return f == nullptr ? nullptr : cstr(f->result.name);
}

uint64_t astral_function_address(const astral_function *f)
{
    return f == nullptr ? 0 : f->result.address;
}

uint64_t astral_function_size(const astral_function *f)
{
    return f == nullptr ? 0 : f->result.size;
}

const char *astral_function_c_code(const astral_function *f)
{
    // The readable listing, not the printer's raw form: this is what a person
    // asked to see. The raw form stays inside the library, where the compilable
    // path is built from it.
    if (f == nullptr)
        return nullptr;
    return cstr(f->result.c_code_pseudo.empty() ? f->result.c_code : f->result.c_code_pseudo);
}

const char *astral_function_signature(const astral_function *f)
{
    return f == nullptr ? nullptr : cstr(f->result.signature);
}

const char *astral_function_return_type(const astral_function *f)
{
    return f == nullptr ? nullptr : cstr(f->result.return_type);
}

const char *astral_function_calling_convention(const astral_function *f)
{
    return f == nullptr ? nullptr : cstr(f->result.calling_convention);
}

int astral_function_parameter_count(const astral_function *f)
{
    return f == nullptr ? 0 : static_cast<int>(f->result.parameter_names.size());
}

const char *astral_function_parameter_name(const astral_function *f, int index)
{
    return f == nullptr ? nullptr : element(f->result.parameter_names, index);
}

const char *astral_function_parameter_type(const astral_function *f, int index)
{
    return f == nullptr ? nullptr : element(f->result.parameter_types, index);
}

int astral_function_local_count(const astral_function *f)
{
    return f == nullptr ? 0 : static_cast<int>(f->result.local_names.size());
}

const char *astral_function_local_name(const astral_function *f, int index)
{
    return f == nullptr ? nullptr : element(f->result.local_names, index);
}

const char *astral_function_local_type(const astral_function *f, int index)
{
    return f == nullptr ? nullptr : element(f->result.local_types, index);
}

int astral_function_callee_count(const astral_function *f)
{
    return f == nullptr ? 0 : static_cast<int>(f->result.callees.size());
}

uint64_t astral_function_callee(const astral_function *f, int index)
{
    if (f == nullptr || index < 0 || static_cast<size_t>(index) >= f->result.callees.size())
        return 0;
    return f->result.callees[static_cast<size_t>(index)];
}

const char *astral_function_callee_name(const astral_function *f, int index)
{
    return f == nullptr ? nullptr : element(f->result.callee_names, index);
}

const char *astral_function_naming_reason(const astral_function *f)
{
    return f == nullptr ? nullptr : cstr(f->result.naming_reason);
}

int astral_function_rename_count(const astral_function *f)
{
    return f == nullptr ? 0 : static_cast<int>(f->result.applied_renames.size());
}

const char *astral_function_rename_from(const astral_function *f, int index)
{
    if (f == nullptr || index < 0 ||
        static_cast<size_t>(index) >= f->result.applied_renames.size())
        return nullptr;
    return f->result.applied_renames[static_cast<size_t>(index)].first.c_str();
}

const char *astral_function_rename_to(const astral_function *f, int index)
{
    if (f == nullptr || index < 0 ||
        static_cast<size_t>(index) >= f->result.applied_renames.size())
        return nullptr;
    return f->result.applied_renames[static_cast<size_t>(index)].second.c_str();
}

int astral_function_comment_count(const astral_function *f)
{
    return f == nullptr ? 0 : static_cast<int>(f->result.comments.size());
}

const char *astral_function_comment(const astral_function *f, int index)
{
    return f == nullptr ? nullptr : element(f->result.comments, index);
}

int astral_function_block_count(const astral_function *f)
{
    return f == nullptr ? 0 : static_cast<int>(f->result.block_addresses.size());
}

uint64_t astral_function_block_address(const astral_function *f, int index)
{
    if (f == nullptr || index < 0 || static_cast<size_t>(index) >= f->result.block_addresses.size())
        return 0;
    return f->result.block_addresses[static_cast<size_t>(index)];
}

} // extern "C"

namespace {

bool slurp(const char *path, std::vector<uint8_t> &bytes)
{
    std::FILE *file = std::fopen(path, "rb");
    if (file == nullptr)
        return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    bytes.resize(size > 0 ? static_cast<size_t>(size) : 0);
    const size_t got = bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    return got == bytes.size();
}

} // namespace

int astral_is_dotnet(const char *path)
{
    std::vector<uint8_t> bytes;
    if (path == nullptr || !slurp(path, bytes))
        return 0;
    return astral_internal::is_dotnet_assembly(bytes) ? 1 : 0;
}

char *astral_dotnet_source(const char *path)
{
    std::vector<uint8_t> bytes;
    if (path == nullptr || !slurp(path, bytes)) {
        fail(ASTRAL_ERR_IO, "cannot read that file");
        return nullptr;
    }
    if (!astral_internal::is_dotnet_assembly(bytes)) {
        fail(ASTRAL_ERR_INVALID_ARGUMENT, "that file carries no managed code");
        return nullptr;
    }
    const astral_internal::DotnetAssembly assembly =
        astral_internal::read_dotnet_assembly(bytes);
    if (!assembly.ok) {
        fail(ASTRAL_ERR_INVALID_ARGUMENT, assembly.error);
        return nullptr;
    }
    clear_error();
    return copy_string(astral_internal::decompile_dotnet(assembly));
}

char *astral_program_run(astral_program *program, uint64_t entry, const char *const *arguments,
                         const char *input, uint64_t step_limit)
{
    if (program == nullptr) {
        fail(ASTRAL_ERR_INVALID_ARGUMENT, "null program");
        return nullptr;
    }
    astral_internal::emulator::RunOptions options;
    options.entry = entry;
    if (arguments != nullptr)
        for (const char *const *one = arguments; *one != nullptr; ++one)
            options.arguments.push_back(*one);
    if (input != nullptr)
        options.input = input;
    if (step_limit != 0)
        options.step_limit = step_limit;

    const astral_internal::emulator::RunResult outcome = program->session->run(options);
    if (!outcome.ok) {
        fail(ASTRAL_ERR_INTERNAL, outcome.error);
        return nullptr;
    }
    std::ostringstream report;
    if (!outcome.output.empty())
        report << outcome.output;
    if (!outcome.output.empty() && outcome.output.back() != '\n')
        report << '\n';
    report << "-- " << outcome.stopped_because;
    if (outcome.returned)
        report << " with " << static_cast<long long>(static_cast<int32_t>(outcome.result));
    report << ", after " << outcome.steps << " instructions";
    if (!outcome.calls.empty()) {
        report << "\n-- called:";
        for (const std::string &call : outcome.calls)
            report << ' ' << call;
    }
    report << '\n';
    clear_error();
    return copy_string(report.str());
}

/* --- Debugging ----------------------------------------------------------- */

struct astral_debugger {
    std::unique_ptr<astral_internal::emulator::Debugger> value;
};

namespace {

using astral_internal::emulator::Debugger;

// An argument written as a number is passed as that number; anything else is a
// string, which is what most functions worth calling by hand take.
bool argument_is_number(const char *text, uint64_t &value)
{
    if (text == nullptr || *text == '\0')
        return false;
    char *end = nullptr;
    const int base = (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) ? 16 : 10;
    const unsigned long long parsed = std::strtoull(text, &end, base);
    if (end == text || *end != '\0')
        return false;
    value = parsed;
    return true;
}

std::string lines_from(const std::vector<std::string> &all)
{
    std::string out;
    for (const std::string &one : all) {
        out += one;
        out += '\n';
    }
    return out;
}

} // namespace

astral_debugger *astral_debugger_open(astral_program *program, uint64_t entry,
                                      const char *const *arguments, const char *input,
                                      uint64_t step_limit)
{
    if (program == nullptr) {
        fail(ASTRAL_ERR_INVALID_ARGUMENT, "null program");
        return nullptr;
    }
    astral_internal::emulator::RunOptions options;
    options.entry = entry;
    if (arguments != nullptr)
        for (const char *const *one = arguments; *one != nullptr; ++one)
            options.arguments.push_back(*one);
    if (input != nullptr)
        options.input = input;
    if (step_limit != 0)
        options.step_limit = step_limit;

    std::string error;
    std::unique_ptr<Debugger> made = program->session->debug(options, error);
    if (!made) {
        fail(ASTRAL_ERR_INTERNAL, error.empty() ? "this program cannot be debugged" : error);
        return nullptr;
    }
    auto *handle = new (std::nothrow) astral_debugger();
    if (handle == nullptr) {
        set_error("out of memory");
        return nullptr;
    }
    handle->value = std::move(made);
    clear_error();
    return handle;
}

void astral_debugger_free(astral_debugger *debugger) { delete debugger; }

astral_status astral_debugger_start(astral_debugger *debugger)
{
    if (debugger == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null debugger");
    debugger->value->start();
    clear_error();
    return ASTRAL_OK;
}

astral_status astral_debugger_step(astral_debugger *debugger)
{
    if (debugger == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null debugger");
    debugger->value->step();
    clear_error();
    return ASTRAL_OK;
}

astral_status astral_debugger_step_over(astral_debugger *debugger)
{
    if (debugger == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null debugger");
    debugger->value->step_over();
    clear_error();
    return ASTRAL_OK;
}

astral_status astral_debugger_step_out(astral_debugger *debugger)
{
    if (debugger == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null debugger");
    debugger->value->step_out();
    clear_error();
    return ASTRAL_OK;
}

astral_status astral_debugger_run_to(astral_debugger *debugger, uint64_t address)
{
    if (debugger == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null debugger");
    debugger->value->run_to(address);
    clear_error();
    return ASTRAL_OK;
}

astral_status astral_debugger_go(astral_debugger *debugger)
{
    if (debugger == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null debugger");
    debugger->value->go();
    clear_error();
    return ASTRAL_OK;
}

void astral_debugger_cancel(astral_debugger *debugger)
{
    if (debugger != nullptr)
        debugger->value->cancel();
}

astral_stop astral_debugger_stop_reason(const astral_debugger *debugger)
{
    if (debugger == nullptr)
        return ASTRAL_STOP_NOT_STARTED;
    return static_cast<astral_stop>(debugger->value->state().stop);
}

char *astral_debugger_reason(const astral_debugger *debugger)
{
    if (debugger == nullptr)
        return nullptr;
    return copy_string(debugger->value->state().reason);
}

uint64_t astral_debugger_address(const astral_debugger *debugger)
{
    return debugger == nullptr ? 0 : debugger->value->state().address;
}

char *astral_debugger_function(const astral_debugger *debugger)
{
    if (debugger == nullptr)
        return nullptr;
    return copy_string(debugger->value->state().function);
}

uint64_t astral_debugger_steps(const astral_debugger *debugger)
{
    return debugger == nullptr ? 0 : debugger->value->state().steps;
}

int astral_debugger_is_live(const astral_debugger *debugger)
{
    return debugger != nullptr && debugger->value->state().live ? 1 : 0;
}

char *astral_debugger_output(const astral_debugger *debugger)
{
    if (debugger == nullptr)
        return nullptr;
    return copy_string(debugger->value->state().output);
}

char *astral_debugger_calls(const astral_debugger *debugger)
{
    if (debugger == nullptr)
        return nullptr;
    return copy_string(lines_from(debugger->value->state().calls));
}

astral_status astral_debugger_set_trace(astral_debugger *debugger, int on)
{
    if (debugger == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null debugger");
    debugger->value->set_trace(on != 0);
    clear_error();
    return ASTRAL_OK;
}

char *astral_debugger_trace(const astral_debugger *debugger)
{
    if (debugger == nullptr)
        return nullptr;
    return copy_string(lines_from(debugger->value->outcome().trace));
}

astral_status astral_debugger_add_breakpoint(astral_debugger *debugger, uint64_t address)
{
    if (debugger == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null debugger");
    debugger->value->add_breakpoint(address);
    clear_error();
    return ASTRAL_OK;
}

astral_status astral_debugger_remove_breakpoint(astral_debugger *debugger, uint64_t address)
{
    if (debugger == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null debugger");
    debugger->value->remove_breakpoint(address);
    clear_error();
    return ASTRAL_OK;
}

void astral_debugger_clear_breakpoints(astral_debugger *debugger)
{
    if (debugger != nullptr)
        debugger->value->clear_breakpoints();
}

int astral_debugger_breakpoint_count(const astral_debugger *debugger)
{
    if (debugger == nullptr)
        return 0;
    return static_cast<int>(debugger->value->breakpoints().size());
}

uint64_t astral_debugger_breakpoint(const astral_debugger *debugger, int index)
{
    if (debugger == nullptr || index < 0)
        return 0;
    const std::vector<uint64_t> all = debugger->value->breakpoints();
    if (static_cast<size_t>(index) >= all.size())
        return 0;
    return all[static_cast<size_t>(index)];
}

astral_status astral_debugger_add_watchpoint(astral_debugger *debugger, uint64_t address,
                                             uint64_t size)
{
    if (debugger == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null debugger");
    debugger->value->add_watchpoint(address, size);
    clear_error();
    return ASTRAL_OK;
}

astral_status astral_debugger_remove_watchpoint(astral_debugger *debugger, uint64_t address)
{
    if (debugger == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null debugger");
    debugger->value->remove_watchpoint(address);
    clear_error();
    return ASTRAL_OK;
}

void astral_debugger_clear_watchpoints(astral_debugger *debugger)
{
    if (debugger != nullptr)
        debugger->value->clear_watchpoints();
}

char *astral_debugger_registers(const astral_debugger *debugger)
{
    if (debugger == nullptr)
        return nullptr;
    std::string out;
    char line[64];
    for (const Debugger::Register &one : debugger->value->registers()) {
        std::snprintf(line, sizeof line, " 0x%llx\n", static_cast<unsigned long long>(one.value));
        out += one.name;
        out += line;
    }
    return copy_string(out);
}

astral_status astral_debugger_register(const astral_debugger *debugger, const char *name,
                                       uint64_t *out)
{
    if (debugger == nullptr || name == nullptr || out == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null debugger, name or result");
    for (const Debugger::Register &one : debugger->value->registers()) {
        if (one.name == name) {
            *out = one.value;
            clear_error();
            return ASTRAL_OK;
        }
    }
    return fail(ASTRAL_ERR_NO_SUCH_ADDRESS, std::string("this architecture has no register ") + name);
}

astral_status astral_debugger_set_register(astral_debugger *debugger, const char *name,
                                           uint64_t value)
{
    if (debugger == nullptr || name == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null debugger or name");
    std::string error;
    if (!debugger->value->set_register(name, value, error))
        return fail(ASTRAL_ERR_NO_SUCH_ADDRESS, error);
    clear_error();
    return ASTRAL_OK;
}

size_t astral_debugger_read(const astral_debugger *debugger, uint64_t address, void *out,
                            size_t size)
{
    if (debugger == nullptr || out == nullptr)
        return 0;
    const std::vector<uint8_t> bytes = debugger->value->read(address, size);
    std::memcpy(out, bytes.data(), bytes.size());
    return bytes.size();
}

astral_status astral_debugger_write(astral_debugger *debugger, uint64_t address, const void *bytes,
                                    size_t size)
{
    if (debugger == nullptr || (bytes == nullptr && size != 0))
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null debugger or bytes");
    const uint8_t *from = static_cast<const uint8_t *>(bytes);
    std::string error;
    if (!debugger->value->write(address, std::vector<uint8_t>(from, from + size), error))
        return fail(ASTRAL_ERR_NO_SUCH_ADDRESS, error);
    clear_error();
    return ASTRAL_OK;
}

char *astral_debugger_read_text(const astral_debugger *debugger, uint64_t address)
{
    if (debugger == nullptr)
        return nullptr;
    std::string out;
    for (uint64_t i = 0; i < 4096; ++i) {
        const std::vector<uint8_t> one = debugger->value->read(address + i, 1);
        if (one.empty() || one[0] == 0)
            break;
        out.push_back(static_cast<char>(one[0]));
    }
    return copy_string(out);
}

char *astral_debugger_stack(const astral_debugger *debugger)
{
    if (debugger == nullptr)
        return nullptr;
    std::string out;
    char line[80];
    for (const Debugger::Frame &frame : debugger->value->stack()) {
        std::snprintf(line, sizeof line, "0x%llx 0x%llx ",
                      static_cast<unsigned long long>(frame.address),
                      static_cast<unsigned long long>(frame.frame_pointer));
        out += line;
        out += frame.function;
        out += '\n';
    }
    return copy_string(out);
}

astral_status astral_debugger_call(astral_debugger *debugger, uint64_t address,
                                   const char *const *arguments, uint64_t step_limit,
                                   uint64_t *result, char **output)
{
    if (debugger == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null debugger");
    std::vector<Debugger::Argument> given;
    if (arguments != nullptr) {
        for (const char *const *one = arguments; *one != nullptr; ++one) {
            Debugger::Argument argument;
            if (argument_is_number(*one, argument.value)) {
                argument.is_text = false;
            } else {
                argument.is_text = true;
                argument.text = *one;
            }
            given.push_back(argument);
        }
    }
    const Debugger::CallResult answer = debugger->value->call(address, given, step_limit);
    if (!answer.ok)
        return fail(ASTRAL_ERR_INTERNAL, answer.error);
    if (result != nullptr)
        *result = answer.result;
    if (output != nullptr)
        *output = copy_string(answer.output);
    clear_error();
    return ASTRAL_OK;
}

size_t astral_debugger_snapshot(const astral_debugger *debugger, void *out, size_t size)
{
    if (debugger == nullptr)
        return 0;
    const std::vector<uint8_t> bytes = debugger->value->snapshot();
    if (out != nullptr && size >= bytes.size())
        std::memcpy(out, bytes.data(), bytes.size());
    return bytes.size();
}

astral_status astral_debugger_restore(astral_debugger *debugger, const void *bytes, size_t size)
{
    if (debugger == nullptr || bytes == nullptr)
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, "null debugger or snapshot");
    const uint8_t *from = static_cast<const uint8_t *>(bytes);
    std::string error;
    if (!debugger->value->restore(std::vector<uint8_t>(from, from + size), error))
        return fail(ASTRAL_ERR_INVALID_ARGUMENT, error);
    clear_error();
    return ASTRAL_OK;
}
