#ifndef ASTRAL_SESSION_HH
#define ASTRAL_SESSION_HH

#include "image.hh"

#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ghidra {
class Architecture;
}

namespace astral_internal {

// One thing a decompiled function references but does not define, rendered as
// the C declaration that makes it legal to use.
struct Declaration {
    std::string name;
    std::string text;
    uint64_t address = 0;
    bool is_function = false;
};

// Everything extracted from one decompiled function, detached from the
// architecture so the handle stays valid after further decompilation.
struct FunctionResult {
    std::string name;
    uint64_t address = 0;
    uint64_t size = 0;
    // The decompiler's listing, as Ghidra prints it.
    std::string c_code;
    std::string signature;
    // The same function rewritten into C that compiles.
    std::string c_code_real;
    std::string signature_real;
    std::string return_type;
    std::string calling_convention;
    std::vector<std::string> parameter_names;
    std::vector<std::string> parameter_types;
    std::vector<std::string> local_names;
    std::vector<std::string> local_types;
    std::vector<uint64_t> callees;
    std::vector<std::string> callee_names;
    std::vector<uint64_t> block_addresses;
    std::vector<Declaration> externals;
    // Explanations attached to the body by the knowledge base.
    std::vector<std::string> comments;
    // Names Astral chose, and why, so the user can judge them.
    std::string naming_reason;
    std::vector<std::pair<std::string, std::string>> applied_renames;
};

// Loads the SLEIGH specification tree. Safe to call repeatedly.
astral_status initialize(const char *spec_root);
void terminate();
bool is_initialized();

class Session {
public:
    ~Session();

    // Builds a session over an already-parsed image. Returns null and fills
    // `error` on failure. `language_override` may be empty.
    static std::unique_ptr<Session> create(BinaryImage image, const std::string &language_override,
                                           std::string &error);

    const BinaryImage &image() const { return image_; }
    const std::string &architecture_id() const { return archid_; }
    std::string language_id() const;
    std::string compiler_spec() const;
    bool big_endian() const;
    int pointer_size() const;

    bool add_symbol(uint64_t address, const std::string &name, bool is_function, std::string &error);

    // Renames whatever lives at `address`, and remembers the choice so the same
    // body is recognised in other programs.
    bool rename(uint64_t address, const std::string &name, bool learn, std::string &error);

    // Whether to name placeholders from evidence. On by default.
    void set_auto_naming(bool on) { auto_naming_ = on; }
    bool auto_naming() const { return auto_naming_; }
    bool set_option(const std::string &name, const std::string &value, std::string &error);

    bool disassemble(uint64_t address, int count, std::string &out, std::string &error);
    bool pcode(uint64_t address, int count, std::string &out, std::string &error);

    bool decompile(uint64_t address, const std::string &name, FunctionResult &out,
                   std::string &error);

    // Decompiles each address and renders them as one compilable C file.
    bool emit_c(const std::vector<uint64_t> &addresses, bool self_contained, bool comments,
                std::string &out, std::string &error);

    // Addresses of every function symbol the loader recovered.
    std::vector<uint64_t> function_addresses() const;

    // Records every named function in this program against a fingerprint of its
    // body, so the same code is recognised in programs that have no symbols.
    // Returns how many were added.
    int learn_symbols(std::string &error);

    // Warnings the decompiler produced while working on this program.
    std::string messages() const { return messages_.str(); }

private:
    Session() = default;

    // Cache of global-scope symbols, used to declare what a function references.
    struct GlobalSymbol {
        std::string type_text;
        uint64_t address = 0;
        bool is_function = false;
    };
    const std::map<std::string, GlobalSymbol> &globals() const;
    std::string apply_naming(void *funcdata, FunctionResult &out);
    void analyse_function(void *funcdata, FunctionResult &out);
    void apply_learned_names();
    void apply_known_prototype(const std::string &name);
    std::string learned_name_for(uint64_t address, uint64_t size) const;
    void collect_externals(FunctionResult &result, const void *funcdata) const;

    BinaryImage image_;
    std::string archid_;
    // The architecture holds a pointer to this for its whole life, so it has to
    // outlive every use of the architecture rather than being a local.
    std::ostringstream messages_;
    ghidra::Architecture *arch_ = nullptr;
    mutable std::map<std::string, GlobalSymbol> globals_;
    mutable bool globals_valid_ = false;
    bool auto_naming_ = true;
};

// Compiles a SLEIGH specification. Returns false and fills `error` on failure.
bool compile_sleigh(const std::string &slaspec, const std::string &sla, std::string &error);

// Language list, as loaded by initialize().
int language_count();
const char *language_id_at(int index);
const char *language_description_at(int index);

} // namespace astral_internal

#endif
