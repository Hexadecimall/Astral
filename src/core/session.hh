#ifndef ASTRAL_SESSION_HH
#define ASTRAL_SESSION_HH

#include "image.hh"
#include "patch.hh"

#include <map>
#include <memory>
#include <sstream>
#include <functional>
#include <condition_variable>
#include <set>
#include <thread>
#include <string>
#include <utility>
#include <vector>

namespace ghidra {
class Architecture;
class Address;
class FuncProto;
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
    // `note_prototypes` records what each mangled name says about itself in the
    // shared knowledge base. A second engine over the same image sets it false:
    // the facts are already there, and writing them again would race the
    // threads reading them.
    static std::unique_ptr<Session> create(BinaryImage image, const std::string &language_override,
                                           std::string &error, bool note_prototypes = true);

    const BinaryImage &image() const { return image_; }
    const std::string &architecture_id() const { return archid_; }
    std::string language_id() const;
    std::string compiler_spec() const;
    bool big_endian() const;
    int pointer_size() const;

    // Another session over the same image, for a second thread. Building one
    // is cheap next to decompiling: the specifications are already parsed and
    // each thread keeps its own translator.
    std::unique_ptr<Session> clone(std::string &error) const;

    // How many threads decompilation may use. Zero means one per core. This is
    // per session and applies to whole-program work, where the functions are
    // independent of each other.
    void set_threads(int count) { threads_ = count < 0 ? 0 : count; }
    int threads() const { return threads_; }
    // The number actually used for a job of this size.
    int worker_count(size_t work) const;

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
                bool explain, std::string &out, std::string &error);

    // Addresses of every function symbol the loader recovered.
    std::vector<uint64_t> function_addresses() const;

    // Records every named function in this program against a fingerprint of its
    // body, so the same code is recognised in programs that have no symbols.
    // Returns how many were added.
    int learn_symbols(std::string &error);

    // --- Patching ---
    //
    // Edits accumulate here and are written back by write_patched(). Astral's
    // superpower: the C you read can be turned into bytes in the binary.

    PatchSet &patches() { return patches_; }
    const PatchSet &patches() const { return patches_; }

    // Length in bytes of the instruction at `address`; 0 if it will not decode.
    int instruction_length(uint64_t address) const;

    // Records a raw byte edit at a virtual address. Reads the bytes currently
    // there as the patch's original, resolves the file offset, and appends it.
    // Fails when the address has no file backing (a .bss region, or unmapped).
    bool patch_bytes(uint64_t address, const std::vector<uint8_t> &bytes, PatchTier tier,
                     const std::string &note, std::string &error);

    // Replaces `instruction_count` instructions starting at `address` with the
    // architecture's no-op. A recognised structural edit (tier byte-rewrite).
    bool patch_nop(uint64_t address, int instruction_count, std::string &error);

    // Inverts the conditional branch at `address` (b.cond, cbz/cbnz, tbz/tbnz
    // on arm64; the Jcc family on x86-64), so the path it guards is taken when
    // it was not and the other way round. A recognised structural edit.
    bool patch_invert_branch(uint64_t address, std::string &error);

    // Overwrites the start of the function at `address` so it does nothing but
    // return `value`. Enough of the prologue is replaced to hold the two
    // instructions; the rest becomes unreachable.
    bool patch_return(uint64_t address, uint64_t value, std::string &error);

    // The original file with every accumulated patch applied, written to
    // `out_path`. Re-reads the source file, so the original must still be on
    // disk at image().path.
    bool write_patched(const std::string &out_path, std::string &error) const;

    // Removes the most recent patch, restoring the bytes it replaced in the
    // in-memory image so disassembly and decompilation revert with it. Returns
    // false when there is nothing to undo.
    bool undo_patch();

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
    std::string name_for_entry(uint64_t address) const;
    // What the engine currently calls the function at an address.
    std::string current_name(uint64_t address) const;
    // Decompiles every address in `work`, spreading it over worker threads.
    // Each worker builds its own engine on its own thread, because a translator
    // belongs to the thread that made it. `apply_names`, when given, is the set
    // of names every engine must agree on before it starts. `names` collects
    // what each function settled on, and callees met along the way are added to
    // `found`.
    class Pool;
    int threads_ = 0;
    // Collects prototype overrides that give each printf-style call a concrete
    // signature recovered from its format string, applied to the current
    // function so its arguments come back. Built the way Ghidra's own override
    // does: internal storage, no input lock.
    std::vector<std::pair<ghidra::Address, ghidra::FuncProto *>>
    collect_vararg_overrides(void *funcdata);
    std::string learned_name_for(uint64_t address, uint64_t size) const;
    void collect_externals(FunctionResult &result, const void *funcdata) const;

    BinaryImage image_;
    PatchSet patches_;
    // Why each function got its name, kept so a later re-decompile (the second
    // pass of whole-program emission) can still explain a name that an earlier
    // pass already applied.
    std::map<uint64_t, std::string> naming_reasons_;
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
